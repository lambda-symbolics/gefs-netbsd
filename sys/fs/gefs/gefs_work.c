/*
 * GEFS for NetBSD: the mutator and reader threads.
 *
 * amd64 kernel stacks are 12 KiB, and the core's deeper paths (a
 * commit, or a page write that splits tree nodes and allocates blocks)
 * do not fit under a VFS call chain.  As on 9front, where a mutator proc
 * and reader procs did the work, each device has a mutator thread for
 * everything that changes the trees (it holds fs->mutlk anyway) and a
 * couple of reader threads for page-in I/O; they start with empty
 * stacks.  Lookups, getattr and readdir stay in the caller.  All the
 * labels mounted from a device share its threads; a job carries the
 * mount it is for.
 */
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/queue.h>
#include <uvm/uvm.h>

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"
#include "gefs.h"

struct gefsjob {
	TAILQ_ENTRY(gefsjob) j_link;
	struct gefsmount *j_gm;
	struct buf	*j_bp;		/* page I/O, or */
	struct vnode	*j_vp;
	void		(*j_fn)(struct gefsmount *, void *);	/* a call */
	void		*j_arg;
	int		j_wait;		/* the caller sleeps until done */
	int		j_done;
	int		j_error;
};

static void
gefs_bufio(struct gefsmount *gm, struct vnode *vp, struct buf *bp)
{
	Dent *de = VTODE(vp);
	off_t off, size;
	long n, len;

	off = (off_t)bp->b_blkno << DEV_BSHIFT;
	len = bp->b_bcount;
	if(gefs_trace)
		printf("gefs: %s qid %llx %lld+%ld v_size %lld length %lld%s\n",
		    (bp->b_flags & B_READ) ? "read" : "write",
		    (unsigned long long)de->qid.path, (long long)off, len,
		    (long long)vp->v_size, (long long)de->length,
		    (de->gone || de->orphan) ? " (removed)" : "");
	if(bp->b_flags & B_READ){
		n = gefsread(gm->gm_mnt, de, bp->b_data, off, len);
		if(n < len)
			memset((char*)bp->b_data + n, 0, len - n);
	}else if(de->gone || de->orphan){
		/* a removed file's pages: nothing to keep */
	}else{
		size = vp->v_size;
		if(off < size){
			n = MIN(len, size - off);
			if(!waserror()){
				gefswrite(gm->gm_mnt, de, bp->b_data, off, n,
				    size, de->muid);
				poperror();
			}else{
				/*
				 * Full: what was freed since the last commit
				 * (rm -rf, then make) is not free until the
				 * next one.  Commit, and try once more.
				 */
				if(strcmp(errmsg(), Efull) != 0)
					nexterror();
				commit(Clazy);
				commit(Clazy);
				gefswrite(gm->gm_mnt, de, bp->b_data, off, n,
				    size, de->muid);
			}
		}
	}
}

static void
gefs_runjob(struct gefsdev *gd, struct gefsjob *j)
{
	struct buf *bp;
	int error;

	error = 0;
	p9setfs(gd->gd_fs);
	if(waserror()){
		error = gefserrno(errmsg());
	}else{
		if(j->j_bp != NULL)
			gefs_bufio(j->j_gm, j->j_vp, j->j_bp);
		else
			j->j_fn(j->j_gm, j->j_arg);
		poperror();
	}
	p9setfs(nil);
	if((bp = j->j_bp) != NULL){
		/* the data is gone; the next write, fsync or close says so */
		if(error != 0 && !(bp->b_flags & B_READ) && VTOGN(j->j_vp) != NULL)
			VTOGN(j->j_vp)->gn_werror = error;
		if(!(bp->b_flags & B_READ)){
			mutex_enter(&gd->gd_jlk);
			if(gd->gd_nwq-- == GEFS_MAXWQ)
				cv_broadcast(&gd->gd_wqcv);
			mutex_exit(&gd->gd_jlk);
		}
		kmem_free(j, sizeof(*j));
		bp->b_error = error;
		bp->b_resid = 0;
		biodone(bp);
	}else if(j->j_wait){
		mutex_enter(&gd->gd_jlk);
		j->j_error = error;
		j->j_done = 1;
		cv_broadcast(&gd->gd_dcv);
		mutex_exit(&gd->gd_jlk);
	}else
		kmem_free(j, sizeof(*j));
}

static void
gefs_thread(struct gefsdev *gd, struct gefsjobq *q, kcondvar_t *cv)
{
	struct gefsjob *j;

	for(;;){
		mutex_enter(&gd->gd_jlk);
		while(TAILQ_EMPTY(q) && !gd->gd_stop)
			cv_wait(cv, &gd->gd_jlk);
		if(TAILQ_EMPTY(q)){
			gd->gd_nthreads--;
			cv_broadcast(&gd->gd_dcv);
			mutex_exit(&gd->gd_jlk);
			kthread_exit(0);
		}
		j = TAILQ_FIRST(q);
		TAILQ_REMOVE(q, j, j_link);
		mutex_exit(&gd->gd_jlk);
		gefs_runjob(gd, j);
	}
}

/*
 * Commits.  The mutator runs them, between jobs, for two kinds of reason.
 *
 * Requests (group commit): fsync, O_SYNC writes and sync(2) ask for a
 * commit and fsync waits until one that covers its changes is durable.
 * Commits are numbered by their cut (fs->cmtcut, taken under mutlk in
 * fssync) and fs->cmtdone is the last durable one.  A waiter needs the
 * commit after the last cut if the tree is dirty, else the last cut:
 * found on the mutator, in a job queued after its page writes, so
 * nothing it changed is still queued and no commit runs meanwhile.  One
 * requested commit serves every waiter whose need it reaches; it runs
 * once the jobs queued before the request are done (so page writes
 * queued meanwhile join it), and not before vfs.gefs.groupwait
 * microseconds after the request (0 by default).
 *
 * Background: a dirty tree is committed when its oldest change is
 * vfs.gefs.interval seconds old, earlier when the space outside the
 * reserve runs low (at most once a second), and lazy updates alone go
 * out after vfs.gefs.lazymax.  vfs.gefs.dirtypct, off by default, also commits
 * when the blocks queued for writing pass that percent of the block
 * cache; memory does not need it (a full queue or cache is drained, which
 * writes without a commit), and with 25 a 1.5 GB dd took 145 commits and
 * 290 flushes instead of 1 and 2, and twice the time.  A clean file
 * system does nothing: the mutator sleeps without a timeout.
 */
int	gefs_interval = 30;	/* vfs.gefs.interval, s: oldest change, at most */
int	gefs_groupwait = 0;	/* vfs.gefs.groupwait, us: wait for more fsyncs */
int	gefs_dirtypct = 0;	/* vfs.gefs.dirtypct: queued blocks, % of the cache; 0: off */

#define GEFS_SPACEMIN	(2LL*GEFS_WMARGIN)	/* commit below this, for space */
#define NS		1000000000ULL

static uint64_t
gefs_uptime(void)
{
	struct timespec ts;

	nanouptime(&ts);
	return (uint64_t)ts.tv_sec * NS + ts.tv_nsec;
}

/* a mutator job, with gd_jlk */
static void
gefs_mqput(struct gefsdev *gd, struct gefsjob *j)
{
	KASSERT(mutex_owned(&gd->gd_jlk));
	TAILQ_INSERT_TAIL(&gd->gd_mq, j, j_link);
	gd->gd_jenq++;
	cv_signal(&gd->gd_mcv);
}

/* with gd_jlk: ask for a commit after the jobs queued so far */
static void
gefs_want(struct gefsdev *gd, int flags)
{
	KASSERT(mutex_owned(&gd->gd_jlk));
	if(!gd->gd_cmtwant){
		gd->gd_cmtwant = 1;
		gd->gd_cmtticket = gd->gd_jenq;
		gd->gd_cmtdue = gefs_uptime() + (uint64_t)gefs_groupwait * 1000;
		cv_signal(&gd->gd_mcv);
	}
	gd->gd_cmtflags |= flags;
}

/* the mutator: re-read the tunables */
void
gefs_kick(struct gefsdev *gd)
{
	mutex_enter(&gd->gd_jlk);
	cv_signal(&gd->gd_mcv);
	mutex_exit(&gd->gd_jlk);
}

static int
gefs_lowspace(Gefs *f)
{
	vlong fr, rs;
	int i;

	fr = rs = 0;
	for(i = 0; i < f->narena; i++){
		fr += f->arenas[i].size - f->arenas[i].used;
		rs += f->arenas[i].reserve;
	}
	return fr < rs/2 || gefsavail(f) < GEFS_SPACEMIN;
}

static long
gefs_queued(Gefs *f)
{
	long n;
	int i;

	n = 0;
	for(i = 0; i < f->nsyncers; i++)
		n += f->syncq[i].nheap;	/* a hint; only the mutator changes it */
	return n;
}

/*
 * On the mutator, with gd_jlk: note the state the last job left, and
 * say whether to commit now (the reason, or -1) and, if not, how long
 * the mutator may sleep (0: until woken).
 */
static int
gefs_cmtdue(struct gefsdev *gd, uint64_t *sleepns)
{
	Gefs *f = gd->gd_fs;
	uint64_t now, due, d;
	int64_t interval;

	*sleepns = 0;
	now = gefs_uptime();
	if(f->ncommit != gd->gd_ncmtseen){
		gd->gd_ncmtseen = f->ncommit;
		gd->gd_lastcmt = now;
	}
	if(!f->snap.dirty)
		gd->gd_dirtyat = 0;
	else if(gd->gd_dirtyat == 0)
		gd->gd_dirtyat = now;

	due = UINT64_MAX;
	if(gd->gd_cmtwant){
		if(gd->gd_jtaken < gd->gd_cmtticket)
			return -1;	/* jobs from before the request first */
		if(now >= gd->gd_cmtdue)
			return GEFS_WSYNC;
		due = gd->gd_cmtdue;
	}
	if(!agetl(&f->rdonly) && gd->gd_nrw > 0){
		interval = (int64_t)MAX(gefs_interval, 0) * NS;
		if(f->snap.dirty){
			if(now - gd->gd_dirtyat >= (uint64_t)interval)
				return GEFS_WAGE;
			if(gefs_dirtypct > 0 &&
			    gefs_queued(f) > f->cmax * gefs_dirtypct / 100)
				return GEFS_WDIRTY;
			if(now - gd->gd_spaceat >= NS && gefs_lowspace(f)){
				gd->gd_spaceat = now;
				return GEFS_WSPACE;
			}
			due = MIN(due, gd->gd_dirtyat + interval);
		}else if(agetl(&f->lazydirty)){
			if(now - gd->gd_lastcmt >= (uint64_t)Lazymax)
				return GEFS_WLAZY;
			due = MIN(due, gd->gd_lastcmt + Lazymax);
		}
	}
	if(due != UINT64_MAX){
		d = due > now ? due - now : 1;
		*sleepns = d;
	}
	return -1;
}

/*
 * On the mutator, without gd_jlk: commit, then tell the waiters.  A
 * commit that fails, or leaves a waiter's need unmet (a read-only or
 * broken file system), fails every waiter.
 */
static void
gefs_runcommit(struct gefsdev *gd, int flags, int why)
{
	Gefs *f = gd->gd_fs;
	int error;

	error = 0;
	p9setfs(f);
	if(waserror())
		error = gefserrno(errmsg());
	else{
		commit(flags);
		poperror();
	}
	p9setfs(nil);
	mutex_enter(&gd->gd_jlk);
	if(why >= 0)
		gd->gd_nwhy[why]++;
	if(agetv(&f->cmtdone) < gd->gd_cmtneed){
		gd->gd_nfail++;
		gd->gd_cmterr = error ? error : (agetl(&f->rdonly) ? EROFS : EIO);
		gd->gd_cmtneed = 0;
	}else if(gd->gd_cmtwant && gd->gd_jtaken >= gd->gd_cmtticket &&
	    !f->snap.dirty && !(gd->gd_cmtflags & Clazy)){
		/* a background commit got there first: nothing left to do */
		gd->gd_cmtwant = 0;
	}
	cv_broadcast(&gd->gd_cmtcv);
	mutex_exit(&gd->gd_jlk);
}

static void
gefs_mutator(void *arg)
{
	struct gefsdev *gd = arg;
	struct gefsjob *j;
	struct bintime bt;
	uint64_t sleepns;
	int why, flags;

	gd->gd_mutlwp = curlwp;
	mutex_enter(&gd->gd_jlk);
	gd->gd_lastcmt = gefs_uptime();
	for(;;){
		why = gefs_cmtdue(gd, &sleepns);
		if(why >= 0){
			flags = 0;
			if(why == GEFS_WSYNC){
				flags = gd->gd_cmtflags;
				gd->gd_cmtwant = 0;
				gd->gd_cmtflags = 0;
			}else if(why == GEFS_WLAZY)
				flags = Clazy;
			mutex_exit(&gd->gd_jlk);
			gefs_runcommit(gd, flags, why);
			mutex_enter(&gd->gd_jlk);
			continue;
		}
		if(!TAILQ_EMPTY(&gd->gd_mq)){
			j = TAILQ_FIRST(&gd->gd_mq);
			TAILQ_REMOVE(&gd->gd_mq, j, j_link);
			gd->gd_jtaken++;
			mutex_exit(&gd->gd_jlk);
			gefs_runjob(gd, j);
			mutex_enter(&gd->gd_jlk);
			continue;
		}
		if(gd->gd_stop)
			break;
		if(sleepns > 0){
			bt = ns2bintime(sleepns);
			(void)cv_timedwaitbt(&gd->gd_mcv, &gd->gd_jlk, &bt,
			    DEFAULT_TIMEOUT_EPSILON);
		}else
			cv_wait(&gd->gd_mcv, &gd->gd_jlk);
	}
	/* stopping: whoever still waits for a commit gets none */
	if(agetv(&gd->gd_fs->cmtdone) < gd->gd_cmtneed){
		gd->gd_nfail++;
		gd->gd_cmterr = EIO;
		cv_broadcast(&gd->gd_cmtcv);
	}
	gd->gd_nthreads--;
	cv_broadcast(&gd->gd_dcv);
	mutex_exit(&gd->gd_jlk);
	kthread_exit(0);
}

/*
 * On the mutator: what a durability request needs.  Its page writes were
 * queued before this job, so they are in the tree, and no commit is
 * running.
 */
struct needargs {
	int64_t		need;
	uint64_t	nfail;
	int		flags;
};

static void
gefs_needjob(struct gefsmount *gm, void *arg)
{
	struct gefsdev *gd = gm->gm_gd;
	struct needargs *n = arg;
	Gefs *f = gd->gd_fs;

	n->need = agetv(&f->cmtcut) + (f->snap.dirty ? 1 : 0);
	if((n->flags & Clazy) && agetl(&f->lazydirty))
		n->need = agetv(&f->cmtcut) + 1;
	mutex_enter(&gd->gd_jlk);
	gd->gd_nreq++;
	n->nfail = gd->gd_nfail;
	if(agetv(&f->cmtdone) >= n->need)
		gd->gd_njoin++;
	else{
		if(gd->gd_cmtwant)
			gd->gd_njoin++;
		gefs_want(gd, n->flags);
		if(n->need > gd->gd_cmtneed)
			gd->gd_cmtneed = n->need;
	}
	mutex_exit(&gd->gd_jlk);
}

/*
 * A durable fsync (or sync): returns once a commit that holds everything
 * the caller changed before calling is on the disk, sharing that commit
 * with every other caller it covers.  flags as commit()'s: Clazy also
 * writes out lazy updates.
 */
int
gefs_durable(struct gefsmount *gm, int flags)
{
	struct gefsdev *gd = gm->gm_gd;
	struct needargs n;
	int error;

	memset(&n, 0, sizeof(n));
	n.flags = flags;
	if((error = gefs_call(gm, gefs_needjob, &n)) != 0)
		return error;
	mutex_enter(&gd->gd_jlk);
	while(agetv(&gd->gd_fs->cmtdone) < n.need){
		if(gd->gd_nfail != n.nfail){
			error = gd->gd_cmterr;
			break;
		}
		cv_wait(&gd->gd_cmtcv, &gd->gd_jlk);
	}
	mutex_exit(&gd->gd_jlk);
	return error;
}

/* sync(2): a commit soon, after the page writes queued so far; no waiting */
void
gefs_cmtrequest(struct gefsmount *gm, int flags)
{
	struct gefsdev *gd = gm->gm_gd;

	mutex_enter(&gd->gd_jlk);
	gd->gd_nreq++;
	gefs_want(gd, flags);
	mutex_exit(&gd->gd_jlk);
}

static void
gefs_reader(void *arg)
{
	struct gefsdev *gd = arg;

	gefs_thread(gd, &gd->gd_rq, &gd->gd_rcv);
}

int
gefs_work_start(struct gefsdev *gd)
{
	int error, i;

	mutex_init(&gd->gd_jlk, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&gd->gd_mcv, "gefsmut");
	cv_init(&gd->gd_rcv, "gefsrd");
	cv_init(&gd->gd_dcv, "gefsjob");
	cv_init(&gd->gd_wqcv, "gefswq");
	cv_init(&gd->gd_cmtcv, "gefscmt");
	gd->gd_nwq = 0;
	TAILQ_INIT(&gd->gd_mq);
	TAILQ_INIT(&gd->gd_rq);
	gd->gd_stop = 0;
	gd->gd_nthreads = 0;
	error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, gefs_mutator,
	    gd, NULL, "gefsmut");
	if(error)
		goto fail;
	gd->gd_nthreads++;
	for(i = 0; i < GEFS_NREADERS; i++){
		error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, gefs_reader,
		    gd, NULL, "gefsrd");
		if(error)
			goto fail;
		gd->gd_nthreads++;
	}
	return 0;
fail:
	/* Drain started workers and destroy even a wholly unstarted queue. */
	gefs_work_stop(gd);
	return error;
}

void
gefs_work_stop(struct gefsdev *gd)
{
	mutex_enter(&gd->gd_jlk);
	gd->gd_stop = 1;
	cv_broadcast(&gd->gd_mcv);
	cv_broadcast(&gd->gd_rcv);
	while(gd->gd_nthreads > 0)
		cv_wait(&gd->gd_dcv, &gd->gd_jlk);
	mutex_exit(&gd->gd_jlk);
	cv_destroy(&gd->gd_mcv);
	cv_destroy(&gd->gd_rcv);
	cv_destroy(&gd->gd_dcv);
	cv_destroy(&gd->gd_wqcv);
	cv_destroy(&gd->gd_cmtcv);
	mutex_destroy(&gd->gd_jlk);
}

/* run fn(gm, arg) on the mutator and wait for it; returns its errno */
int
gefs_call(struct gefsmount *gm, void (*fn)(struct gefsmount *, void *), void *arg)
{
	struct gefsdev *gd = gm->gm_gd;
	struct gefsjob j;

	memset(&j, 0, sizeof(j));
	j.j_gm = gm;
	j.j_fn = fn;
	j.j_arg = arg;
	j.j_wait = 1;
	mutex_enter(&gd->gd_jlk);
	gefs_mqput(gd, &j);
	while(!j.j_done)
		cv_wait(&gd->gd_dcv, &gd->gd_jlk);
	mutex_exit(&gd->gd_jlk);
	return j.j_error;
}

/* queue fn(gm, arg) on the mutator without waiting; fn owns arg */
void
gefs_post(struct gefsmount *gm, void (*fn)(struct gefsmount *, void *), void *arg)
{
	struct gefsdev *gd = gm->gm_gd;
	struct gefsjob *j;

	j = kmem_zalloc(sizeof(*j), KM_SLEEP);
	j->j_gm = gm;
	j->j_fn = fn;
	j->j_arg = arg;
	mutex_enter(&gd->gd_jlk);
	gefs_mqput(gd, j);
	mutex_exit(&gd->gd_jlk);
}

/*
 * Page I/O: reads to the readers, writes to the mutator; biodone there.
 * Queued writes hold pager VA and dirty pages, so there are at most
 * GEFS_MAXWQ: writers wait for room.  The pagedaemon never waits here,
 * nor allocates what it cannot get at once: it gets ENOMEM, which UVM
 * takes as "try this page again later" and redirties it.
 */
void
gefs_queuebuf(struct gefsmount *gm, struct vnode *vp, struct buf *bp)
{
	struct gefsdev *gd = gm->gm_gd;
	struct gefsjob *j;
	int rd, pd;

	rd = (bp->b_flags & B_READ) != 0;
	pd = (curlwp == uvm.pagedaemon_lwp);
	/*
	 * The pagedaemon may take a page of a write that extends the file
	 * between the copy and the new v_size (gefs_write): the page write
	 * would drop what lies past v_size and leave the page clean.  Later.
	 */
	if(pd && !rd && vp->v_writesize > vp->v_size &&
	    ((off_t)bp->b_blkno << DEV_BSHIFT) + bp->b_bcount > vp->v_size)
		goto nomem;
	j = kmem_zalloc(sizeof(*j), pd ? KM_NOSLEEP : KM_SLEEP);
	if(j == NULL)
		goto nomem;
	j->j_gm = gm;
	j->j_bp = bp;
	j->j_vp = vp;
	mutex_enter(&gd->gd_jlk);
	if(!rd){
		if(pd && gd->gd_nwq >= GEFS_MAXWQ){
			mutex_exit(&gd->gd_jlk);
			kmem_free(j, sizeof(*j));
			goto nomem;
		}
		/* the mutator itself (never), and the pagedaemon, do not wait */
		while(!pd && curlwp != gd->gd_mutlwp && gd->gd_nwq >= GEFS_MAXWQ)
			cv_wait(&gd->gd_wqcv, &gd->gd_jlk);
		gd->gd_nwq++;
	}
	if(rd && gd->gd_nthreads > 1){
		TAILQ_INSERT_TAIL(&gd->gd_rq, j, j_link);
		cv_signal(&gd->gd_rcv);
	}else
		gefs_mqput(gd, j);
	mutex_exit(&gd->gd_jlk);
	return;
nomem:
	bp->b_error = ENOMEM;
	bp->b_resid = bp->b_bcount;
	biodone(bp);
}
