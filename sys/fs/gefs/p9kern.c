/*
 * The NetBSD kernel side of the Plan 9 compatibility layer: per-LWP
 * state (error context, current file system), locks, memory, time,
 * printing, and the device I/O the core does with pread/pwrite.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/dkio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/specificdata.h>
#include <miscfs/specfs/specdev.h>

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"
#include "gefs.h"

/* --- per-LWP state --- */

typedef struct Lwpctx Lwpctx;
struct Lwpctx {
	Errctx	*errp;		/* p9errctxp() returns &errp */
	Gefs	*fsp;		/* p9fsp() returns &fsp */
	int	pendfd;		/* the device the next open() returns */
	Errctx	err;
};

static specificdata_key_t	lwpkey;

static void
lwpdtor(void *p)
{
	if(p != NULL)
		kmem_free(p, sizeof(Lwpctx));
}

static Lwpctx*
lwpctx(void)
{
	Lwpctx *c;

	if((c = lwp_getspecific(lwpkey)) == NULL){
		c = kmem_zalloc(sizeof(*c), KM_SLEEP);
		c->errp = &c->err;
		c->pendfd = -1;
		lwp_setspecific(lwpkey, c);
	}
	return c;
}

Errctx**
p9errctxp(void)
{
	return &lwpctx()->errp;
}

Gefs**
p9fsp(void)
{
	return &lwpctx()->fsp;
}

void
p9setfs(Gefs *f)
{
	lwpctx()->fsp = f;
}

_Noreturn void
p9longjmp(jmp_buf j)
{
	longjmp(j);
}

/* --- memory: kmem with the size kept in front --- */

#define HDR	16

void*
p9malloc(usize n)
{
	char *p;

	p = kmem_alloc(n + HDR, KM_SLEEP);
	*(usize*)p = n;
	return p + HDR;
}

void*
p9mallocz(usize n, int zero)
{
	char *p;

	p = zero ? kmem_zalloc(n + HDR, KM_SLEEP) : kmem_alloc(n + HDR, KM_SLEEP);
	*(usize*)p = n;
	return p + HDR;
}

void*
p9calloc(usize n, usize m)
{
	return p9mallocz(n*m, 1);
}

void
p9free(void *v)
{
	char *p;

	if(v == nil)
		return;
	p = (char*)v - HDR;
	kmem_free(p, *(usize*)p + HDR);
}

void*
p9realloc(void *v, usize n)
{
	usize o;
	void *w;

	if(v == nil)
		return p9malloc(n);
	o = *(usize*)((char*)v - HDR);
	w = p9malloc(n);
	memcpy(w, v, o < n ? o : n);
	p9free(v);
	return w;
}

/* --- errors from the system, output --- */

static char	kerrstr[ERRMAX] = "";

void
werrstr(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprint(kerrstr, sizeof(kerrstr), fmt, args);
	va_end(args);
}

char*
p9errstr(void)
{
	return kerrstr;
}

int
fprint(int fd, char *fmt, ...)
{
	char buf[256];
	va_list args;
	int n;

	USED(fd);
	va_start(args, fmt);
	n = vsnprint(buf, sizeof(buf), fmt, args);
	va_end(args);
	printf("%s", buf);
	return n;
}

int
print(char *fmt, ...)
{
	char buf[256];
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprint(buf, sizeof(buf), fmt, args);
	va_end(args);
	printf("%s", buf);
	return n;
}

/* no process to kill: fail the operation instead */
_Noreturn void
sysfatal(char *fmt, ...)
{
	char buf[ERRMAX];
	va_list args;

	va_start(args, fmt);
	vsnprint(buf, sizeof(buf), fmt, args);
	va_end(args);
	error("%s", buf);
}

_Noreturn void
exits(char *msg)
{
	error("%s", msg != nil ? msg : "exits");
}

vlong
nsec(void)
{
	struct timespec ts;

	getnanotime(&ts);
	return (vlong)ts.tv_sec*1000000000LL + ts.tv_nsec;
}

void
p9sleep(long ms)
{
	int t;

	t = mstohz(ms);
	kpause("gefs", false, t > 0 ? t : 1, NULL);
}

/* --- locks, initialized the first time they are used --- */

enum { Uninit, Initing, Ready };

static void
lazyinit(volatile uint *st, void (*init)(void*), void *arg)
{
	uint s;

	for(;;){
		s = __atomic_load_n(st, __ATOMIC_ACQUIRE);
		if(s == Ready)
			return;
		if(s == Uninit && __atomic_compare_exchange_n(st, &s, Initing,
		    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)){
			init(arg);
			__atomic_store_n(st, Ready, __ATOMIC_RELEASE);
			return;
		}
		preempt_point();
	}
}

static void	mtxinit(void *a)	{ mutex_init(a, MUTEX_DEFAULT, IPL_NONE); }
static void	rwinit(void *a)		{ rw_init(a); }
static void	cvinit(void *a)		{ cv_init(a, "gefs"); }

#define LOCKED(l)	lazyinit(&(l)->init, mtxinit, &(l)->m)

void	lock(Lock *l)		{ LOCKED(l); mutex_enter(&l->m); }
int	canlock(Lock *l)	{ LOCKED(l); return mutex_tryenter(&l->m); }
void	unlock(Lock *l)		{ mutex_exit(&l->m); }
void	qlock(QLock *l)		{ LOCKED(l); mutex_enter(&l->m); }
int	canqlock(QLock *l)	{ LOCKED(l); return mutex_tryenter(&l->m); }
void	qunlock(QLock *l)	{ mutex_exit(&l->m); }

void	rlock(RWLock *l)	{ lazyinit(&l->init, rwinit, &l->l); rw_enter(&l->l, RW_READER); }
int	canrlock(RWLock *l)	{ lazyinit(&l->init, rwinit, &l->l); return rw_tryenter(&l->l, RW_READER); }
void	runlock(RWLock *l)	{ rw_exit(&l->l); }
void	wlock(RWLock *l)	{ lazyinit(&l->init, rwinit, &l->l); rw_enter(&l->l, RW_WRITER); }
int	canwlock(RWLock *l)	{ lazyinit(&l->init, rwinit, &l->l); return rw_tryenter(&l->l, RW_WRITER); }
void	wunlock(RWLock *l)	{ rw_exit(&l->l); }

void
rsleep(Rendez *r)
{
	lazyinit(&r->init, cvinit, &r->cv);
	cv_wait(&r->cv, &r->l->m);
}

int
rwakeup(Rendez *r)
{
	lazyinit(&r->init, cvinit, &r->cv);
	cv_signal(&r->cv);
	return 1;
}

int
rwakeupall(Rendez *r)
{
	lazyinit(&r->init, cvinit, &r->cv);
	cv_broadcast(&r->cv);
	return 1;
}

void	lockfini(Lock *l)	{ if(l->init == Ready) mutex_destroy(&l->m); l->init = Uninit; }
void	qlockfini(QLock *l)	{ if(l->init == Ready) mutex_destroy(&l->m); l->init = Uninit; }
void	rwlockfini(RWLock *l)	{ if(l->init == Ready) rw_destroy(&l->l); l->init = Uninit; }
void	rendezfini(Rendez *r)	{ if(r->init == Ready) cv_destroy(&r->cv); r->init = Uninit; }

/*
 * --- devices ---
 *
 * The core opens its device by name and uses the fd with pread/pwrite.
 * Here a mount registers the device vnode in a slot, the slot number is
 * the fd, and open() returns the slot the caller set up (gefsdevpend).
 */

#define Ndev	32

static struct {
	struct vnode	*vp;
	vlong		size;
	vlong		nread;
	vlong		nwrite;
	vlong		nflush;
} devs[Ndev];
static kmutex_t	devlk;

int
gefsdevadd(struct vnode *vp, vlong size)
{
	int i;

	mutex_enter(&devlk);
	for(i = 0; i < Ndev; i++)
		if(devs[i].vp == NULL){
			devs[i].vp = vp;
			devs[i].size = size;
			devs[i].nread = devs[i].nwrite = devs[i].nflush = 0;
			break;
		}
	mutex_exit(&devlk);
	return i < Ndev ? i : -1;
}

void
gefsdevdel(int fd)
{
	mutex_enter(&devlk);
	devs[fd].vp = NULL;
	mutex_exit(&devlk);
}

void
gefsdevpend(int fd)
{
	lwpctx()->pendfd = fd;
}

void
gefsdevstats(int fd, vlong *r, vlong *w, vlong *f)
{
	*r = devs[fd].nread;
	*w = devs[fd].nwrite;
	*f = devs[fd].nflush;
}

int
open(const char *name, int mode, ...)
{
	int fd;

	USED(name, mode);
	fd = lwpctx()->pendfd;
	if(fd < 0 || fd >= Ndev || devs[fd].vp == NULL)
		return -1;
	return fd;
}

int
close(int fd)
{
	USED(fd);
	return 0;
}

Dir*
dirfstat(int fd)
{
	Dir *d;

	if(fd < 0 || fd >= Ndev || devs[fd].vp == NULL)
		return nil;
	d = mallocz(sizeof(Dir), 1);
	d->length = devs[fd].size;
	return d;
}

static int
devio(int fd, void *buf, usize n, vlong off, int rw)
{
	struct vnode *vp;
	struct buf *bp;
	int err;

	if(fd < 0 || fd >= Ndev || (vp = devs[fd].vp) == NULL)
		return -1;
	if(off < 0 || off + (vlong)n > devs[fd].size || (n & (DEV_BSIZE-1)) != 0)
		return -1;
	bp = getiobuf(vp, true);
	bp->b_data = buf;
	bp->b_bcount = n;
	bp->b_resid = n;
	bp->b_blkno = off >> DEV_BSHIFT;
	bp->b_dev = vp->v_rdev;
	bp->b_proc = NULL;
	bp->b_cflags = BC_BUSY;
	bp->b_oflags = 0;
	if(rw == B_WRITE){
		bp->b_flags = B_WRITE;
		mutex_enter(vp->v_interlock);
		vp->v_numoutput++;
		mutex_exit(vp->v_interlock);
		devs[fd].nwrite++;
	}else{
		bp->b_flags = B_READ;
		devs[fd].nread++;
	}
	VOP_STRATEGY(vp, bp);
	err = biowait(bp);
	putiobuf(bp);
	return err ? -1 : (int)n;
}

ssize_t
pread(int fd, void *buf, size_t n, off_t off)
{
	return devio(fd, buf, n, off, B_READ);
}

ssize_t
pwrite(int fd, const void *buf, size_t n, off_t off)
{
	return devio(fd, (void*)buf, n, off, B_WRITE);
}

/* the device's volatile write cache to stable storage: a barrier */
int
p9devflush(int fd)
{
	struct vnode *vp;
	int force, err;

	if(fd < 0 || fd >= Ndev || (vp = devs[fd].vp) == NULL)
		return -1;
	devs[fd].nflush++;
	force = 1;
	err = VOP_IOCTL(vp, DIOCCACHESYNC, &force, FWRITE, NOCRED);
	/* devices without a write cache say so; that is fine */
	return (err == 0 || err == ENOTTY || err == ENODEV) ? 0 : -1;
}

/* --- module setup --- */

int
p9kinit(void)
{
	mutex_init(&devlk, MUTEX_DEFAULT, IPL_NONE);
	initshow();
	fmtinstall('H', encodefmt);
	fmtinstall('B', Bconv);
	fmtinstall('M', Mconv);
	fmtinstall('P', Pconv);
	fmtinstall('K', Kconv);
	fmtinstall('R', Rconv);
	fmtinstall('Q', Qconv);
	return lwp_specific_key_create(&lwpkey, lwpdtor);
}

void
p9kfini(void)
{
	lwp_specific_key_delete(lwpkey);
	mutex_destroy(&devlk);
}
