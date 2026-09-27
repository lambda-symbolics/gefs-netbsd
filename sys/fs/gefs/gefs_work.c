/*
 * GEFS for NetBSD: the mutator and reader threads.
 *
 * amd64 kernel stacks are 12 KiB, and the core's deeper paths (a
 * commit, or a page write that splits tree nodes and allocates blocks)
 * do not fit under a VFS call chain.  As on 9front, where a mutator proc
 * and reader procs did the work, each mount has a mutator thread for
 * everything that changes the tree (it holds fs->mutlk anyway) and a
 * couple of reader threads for page-in I/O; they start with empty
 * stacks.  Lookups, getattr and readdir stay in the caller.
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
gefs_runjob(struct gefsmount *gm, struct gefsjob *j)
{
	struct buf *bp;
	int error;

	error = 0;
	p9setfs(gm->gm_fs);
	if(waserror()){
		error = gefserrno(errmsg());
	}else{
		if(j->j_bp != NULL)
			gefs_bufio(gm, j->j_vp, j->j_bp);
		else
			j->j_fn(gm, j->j_arg);
		poperror();
	}
	p9setfs(nil);
	if((bp = j->j_bp) != NULL){
		kmem_free(j, sizeof(*j));
		bp->b_error = error;
		bp->b_resid = 0;
		biodone(bp);
	}else if(j->j_wait){
		mutex_enter(&gm->gm_jlk);
		j->j_error = error;
		j->j_done = 1;
		cv_broadcast(&gm->gm_dcv);
		mutex_exit(&gm->gm_jlk);
	}else
		kmem_free(j, sizeof(*j));
}

static void
gefs_thread(struct gefsmount *gm, struct gefsjobq *q, kcondvar_t *cv)
{
	struct gefsjob *j;

	for(;;){
		mutex_enter(&gm->gm_jlk);
		while(TAILQ_EMPTY(q) && !gm->gm_stop)
			cv_wait(cv, &gm->gm_jlk);
		if(TAILQ_EMPTY(q)){
			gm->gm_nthreads--;
			cv_broadcast(&gm->gm_dcv);
			mutex_exit(&gm->gm_jlk);
			kthread_exit(0);
		}
		j = TAILQ_FIRST(q);
		TAILQ_REMOVE(q, j, j_link);
		mutex_exit(&gm->gm_jlk);
		gefs_runjob(gm, j);
	}
}

static void
gefs_mutator(void *arg)
{
	struct gefsmount *gm = arg;

	gefs_thread(gm, &gm->gm_mq, &gm->gm_mcv);
}

static void
gefs_reader(void *arg)
{
	struct gefsmount *gm = arg;

	gefs_thread(gm, &gm->gm_rq, &gm->gm_rcv);
}

int
gefs_work_start(struct gefsmount *gm)
{
	int error, i;

	mutex_init(&gm->gm_jlk, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&gm->gm_mcv, "gefsmut");
	cv_init(&gm->gm_rcv, "gefsrd");
	cv_init(&gm->gm_dcv, "gefsjob");
	TAILQ_INIT(&gm->gm_mq);
	TAILQ_INIT(&gm->gm_rq);
	gm->gm_stop = 0;
	gm->gm_nthreads = 0;
	error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, gefs_mutator,
	    gm, NULL, "gefsmut");
	if(error)
		return error;
	gm->gm_nthreads++;
	for(i = 0; i < GEFS_NREADERS; i++){
		if(kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, gefs_reader,
		    gm, NULL, "gefsrd") == 0)
			gm->gm_nthreads++;
	}
	return 0;
}

void
gefs_work_stop(struct gefsmount *gm)
{
	mutex_enter(&gm->gm_jlk);
	gm->gm_stop = 1;
	cv_broadcast(&gm->gm_mcv);
	cv_broadcast(&gm->gm_rcv);
	while(gm->gm_nthreads > 0)
		cv_wait(&gm->gm_dcv, &gm->gm_jlk);
	mutex_exit(&gm->gm_jlk);
	cv_destroy(&gm->gm_mcv);
	cv_destroy(&gm->gm_rcv);
	cv_destroy(&gm->gm_dcv);
	mutex_destroy(&gm->gm_jlk);
}

/* run fn(gm, arg) on the mutator and wait for it; returns its errno */
int
gefs_call(struct gefsmount *gm, void (*fn)(struct gefsmount *, void *), void *arg)
{
	struct gefsjob j;

	memset(&j, 0, sizeof(j));
	j.j_fn = fn;
	j.j_arg = arg;
	j.j_wait = 1;
	mutex_enter(&gm->gm_jlk);
	TAILQ_INSERT_TAIL(&gm->gm_mq, &j, j_link);
	cv_signal(&gm->gm_mcv);
	while(!j.j_done)
		cv_wait(&gm->gm_dcv, &gm->gm_jlk);
	mutex_exit(&gm->gm_jlk);
	return j.j_error;
}

/* queue fn(gm, arg) on the mutator without waiting; fn owns arg */
void
gefs_post(struct gefsmount *gm, void (*fn)(struct gefsmount *, void *), void *arg)
{
	struct gefsjob *j;

	j = kmem_zalloc(sizeof(*j), KM_SLEEP);
	j->j_fn = fn;
	j->j_arg = arg;
	mutex_enter(&gm->gm_jlk);
	TAILQ_INSERT_TAIL(&gm->gm_mq, j, j_link);
	cv_signal(&gm->gm_mcv);
	mutex_exit(&gm->gm_jlk);
}

/* page I/O: reads to the readers, writes to the mutator; biodone there */
void
gefs_queuebuf(struct gefsmount *gm, struct vnode *vp, struct buf *bp)
{
	struct gefsjob *j;
	int rd;

	j = kmem_zalloc(sizeof(*j), KM_SLEEP);
	j->j_bp = bp;
	j->j_vp = vp;
	rd = (bp->b_flags & B_READ) != 0;
	mutex_enter(&gm->gm_jlk);
	if(rd && gm->gm_nthreads > 1){
		TAILQ_INSERT_TAIL(&gm->gm_rq, j, j_link);
		cv_signal(&gm->gm_rcv);
	}else{
		TAILQ_INSERT_TAIL(&gm->gm_mq, j, j_link);
		cv_signal(&gm->gm_mcv);
	}
	mutex_exit(&gm->gm_jlk);
}
