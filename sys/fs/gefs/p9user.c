/*
 * The userland side of the Plan 9 compatibility layer (NetBSD and Linux):
 * locks, time, memory, output, device size, per-thread state.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __linux__
#include <linux/fs.h>
#endif
#ifdef __NetBSD__
#include <sys/disk.h>
#include <sys/dkio.h>
#endif

#include "dat.h"
#include "fns.h"

#undef pwrite
#undef pread

long long	p9nwrite, p9nread;

ssize_t
p9pwrite(int fd, const void *b, size_t n, off_t o)
{
	__atomic_add_fetch(&p9nwrite, 1, __ATOMIC_SEQ_CST);
	return pwrite(fd, b, n, o);
}

ssize_t
p9pread(int fd, void *b, size_t n, off_t o)
{
	__atomic_add_fetch(&p9nread, 1, __ATOMIC_SEQ_CST);
	return pread(fd, b, n, o);
}

static Gefs	*curfs;
static __thread Errctx	*curerr;
static __thread char	errstrbuf[ERRMAX];

Gefs**
p9fsp(void)
{
	return &curfs;
}

void
p9setfs(Gefs *f)
{
	curfs = f;
}

Errctx**
p9errctxp(void)
{
	if(curerr == nil){
		curerr = calloc(1, sizeof(Errctx));
		if(curerr == nil)
			abort();
	}
	return &curerr;
}

_Noreturn void
p9longjmp(jmp_buf j)
{
	_longjmp(j, 1);
}

/* memory */
void*
p9malloc(usize n)
{
	return malloc(n);
}

void*
p9mallocz(usize n, int zero)
{
	return zero ? calloc(1, n) : malloc(n);
}

void*
p9calloc(usize n, usize m)
{
	return calloc(n, m);
}

void*
p9realloc(void *p, usize n)
{
	return realloc(p, n);
}

void
p9free(void *p)
{
	free(p);
}

/* errors from the system */
void
werrstr(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprint(errstrbuf, sizeof(errstrbuf), fmt, args);
	va_end(args);
}

char*
p9errstr(void)
{
	if(errno != 0){
		snprint(errstrbuf, sizeof(errstrbuf), "%s", strerror(errno));
		errno = 0;
	}
	return errstrbuf;
}

/* output */
int
fprint(int fd, char *fmt, ...)
{
	char buf[1024];
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprint(buf, sizeof(buf), fmt, args);
	va_end(args);
	return write(fd, buf, n);
}

int
print(char *fmt, ...)
{
	char buf[1024];
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprint(buf, sizeof(buf), fmt, args);
	va_end(args);
	return write(1, buf, n);
}

_Noreturn void
sysfatal(char *fmt, ...)
{
	char buf[512];
	va_list args;

	va_start(args, fmt);
	vsnprint(buf, sizeof(buf), fmt, args);
	va_end(args);
	fprint(2, "gefs: %s\n", buf);
	exit(1);
}

_Noreturn void
exits(char *msg)
{
	exit(msg != nil && *msg != 0);
}

vlong	nsecskew;	/* tests move the clock */

vlong
nsec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (vlong)ts.tv_sec*1000000000LL + ts.tv_nsec + nsecskew;
}

void
p9sleep(long ms)
{
	if(ms <= 0)
		sched_yield();
	else
		usleep(ms*1000);
}

Dir*
dirfstat(int fd)
{
	struct stat st;
	Dir *d;

	if(fstat(fd, &st) == -1)
		return nil;
	if((d = calloc(1, sizeof(Dir))) == nil)
		return nil;
	d->length = st.st_size;
	if(S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)){
#if defined(__linux__)
		uint64_t sz;
		if(ioctl(fd, BLKGETSIZE64, &sz) == 0)
			d->length = sz;
#elif defined(__NetBSD__)
		off_t sz;
		if(ioctl(fd, DIOCGMEDIASIZE, &sz) == 0)
			d->length = sz;
#endif
	}
	return d;
}

/*
 * Locks.  Plan 9 locks are ready when zeroed; these initialize themselves
 * the first time they are used.
 */
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
		sched_yield();
	}
}

static void
mtxinit(void *a)
{
	pthread_mutex_init(a, nil);
}

static void
rwinit(void *a)
{
	pthread_rwlock_init(a, nil);
}

static void
cvinit(void *a)
{
	pthread_cond_init(a, nil);
}

#define LOCKED(l)	lazyinit(&(l)->init, mtxinit, &(l)->m)

void	lock(Lock *l)		{ LOCKED(l); pthread_mutex_lock(&l->m); }
int	canlock(Lock *l)	{ LOCKED(l); return pthread_mutex_trylock(&l->m) == 0; }
void	unlock(Lock *l)		{ pthread_mutex_unlock(&l->m); }
void	qlock(QLock *l)		{ LOCKED(l); pthread_mutex_lock(&l->m); }
int	canqlock(QLock *l)	{ LOCKED(l); return pthread_mutex_trylock(&l->m) == 0; }
void	qunlock(QLock *l)	{ pthread_mutex_unlock(&l->m); }

void	rlock(RWLock *l)	{ lazyinit(&l->init, rwinit, &l->l); pthread_rwlock_rdlock(&l->l); }
int	canrlock(RWLock *l)	{ lazyinit(&l->init, rwinit, &l->l); return pthread_rwlock_tryrdlock(&l->l) == 0; }
void	runlock(RWLock *l)	{ pthread_rwlock_unlock(&l->l); }
void	wlock(RWLock *l)	{ lazyinit(&l->init, rwinit, &l->l); pthread_rwlock_wrlock(&l->l); }
int	canwlock(RWLock *l)	{ lazyinit(&l->init, rwinit, &l->l); return pthread_rwlock_trywrlock(&l->l) == 0; }
void	wunlock(RWLock *l)	{ pthread_rwlock_unlock(&l->l); }

void
rsleep(Rendez *r)
{
	lazyinit(&r->init, cvinit, &r->cv);
	pthread_cond_wait(&r->cv, &r->l->m);
}

int
rwakeup(Rendez *r)
{
	lazyinit(&r->init, cvinit, &r->cv);
	pthread_cond_signal(&r->cv);
	return 1;
}

int
rwakeupall(Rendez *r)
{
	lazyinit(&r->init, cvinit, &r->cv);
	pthread_cond_broadcast(&r->cv);
	return 1;
}

void	lockfini(Lock *l)	{ if(l->init == Ready) pthread_mutex_destroy(&l->m); l->init = Uninit; }
void	qlockfini(QLock *l)	{ if(l->init == Ready) pthread_mutex_destroy(&l->m); l->init = Uninit; }
void	rwlockfini(RWLock *l)	{ if(l->init == Ready) pthread_rwlock_destroy(&l->l); l->init = Uninit; }
void	rendezfini(Rendez *r)	{ if(r->init == Ready) pthread_cond_destroy(&r->cv); r->init = Uninit; }
