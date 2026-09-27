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
			gefswrite(gm->gm_mnt, de, bp->b_data, off, n, size,
			    de->muid);
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

static void
gefs_mutator(void *arg)
{
	struct gefsdev *gd = arg;

	gefs_thread(gd, &gd->gd_mq, &gd->gd_mcv);
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
	TAILQ_INIT(&gd->gd_mq);
	TAILQ_INIT(&gd->gd_rq);
	gd->gd_stop = 0;
	gd->gd_nthreads = 0;
	error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, gefs_mutator,
	    gd, NULL, "gefsmut");
	if(error)
		return error;
	gd->gd_nthreads++;
	for(i = 0; i < GEFS_NREADERS; i++){
		if(kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, gefs_reader,
		    gd, NULL, "gefsrd") == 0)
			gd->gd_nthreads++;
	}
	return 0;
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
	TAILQ_INSERT_TAIL(&gd->gd_mq, &j, j_link);
	cv_signal(&gd->gd_mcv);
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
	TAILQ_INSERT_TAIL(&gd->gd_mq, j, j_link);
	cv_signal(&gd->gd_mcv);
	mutex_exit(&gd->gd_jlk);
}

/* page I/O: reads to the readers, writes to the mutator; biodone there */
void
gefs_queuebuf(struct gefsmount *gm, struct vnode *vp, struct buf *bp)
{
	struct gefsdev *gd = gm->gm_gd;
	struct gefsjob *j;
	int rd;

	j = kmem_zalloc(sizeof(*j), KM_SLEEP);
	j->j_gm = gm;
	j->j_bp = bp;
	j->j_vp = vp;
	rd = (bp->b_flags & B_READ) != 0;
	mutex_enter(&gd->gd_jlk);
	if(rd && gd->gd_nthreads > 1){
		TAILQ_INSERT_TAIL(&gd->gd_rq, j, j_link);
		cv_signal(&gd->gd_rcv);
	}else{
		TAILQ_INSERT_TAIL(&gd->gd_mq, j, j_link);
		cv_signal(&gd->gd_mcv);
	}
	mutex_exit(&gd->gd_jlk);
}
