/*
 * The part of Plan 9's <libc.h> that the gefs core uses, for the NetBSD
 * kernel (_KERNEL) and for userland (NetBSD or Linux).
 *
 * Two names are redirected, so that the core can stay as it is:
 *   fs      the current file system.  The core has one global Gefs; the
 *           kernel can mount several, so each thread that enters gefs
 *           sets the one it works on (p9setfs).
 *   errctx  the current thread's error context (waserror/error).
 */
#ifndef GEFS_LIBC_H
#define GEFS_LIBC_H

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/condvar.h>
#include <sys/errno.h>
#include <lib/libkern/libkern.h>
typedef label_t jmp_buf[1];
#define setjmp(j)	setjmp(j)
#else
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#undef setjmp
#undef longjmp
#define setjmp(j)	_setjmp(j)
#define longjmp(j, v)	_longjmp(j, v)
/* counted, so tests can check that an idle file system writes nothing */
extern long long	p9nwrite, p9nread;
ssize_t	p9pwrite(int, const void*, size_t, off_t);
ssize_t	p9pread(int, void*, size_t, off_t);
#define pwrite(f, b, n, o)	p9pwrite(f, b, n, o)
#define pread(f, b, n, o)	p9pread(f, b, n, o)
#endif

/* open modes (the same values as POSIX) */
enum {
	OREAD	= 0,
	OWRITE	= 1,
	ORDWR	= 2,
	OEXEC	= 3,
	OTRUNC	= 16,
	OCEXEC	= 32,
	ORCLOSE	= 64,
	OEXCL	= 0x1000,
};

#define ERRMAX		128
#define KNAMELEN	28

/* qid types and mode bits */
#define QTDIR		0x80
#define QTAPPEND	0x40
#define QTEXCL		0x20
#define QTMOUNT		0x10
#define QTAUTH		0x08
#define QTTMP		0x04
#define QTFILE		0x00

#define DMDIR		0x80000000
#define DMAPPEND	0x40000000
#define DMEXCL		0x20000000
#define DMMOUNT		0x10000000
#define DMAUTH		0x08000000
#define DMTMP		0x04000000
#define DMREAD		0x4
#define DMWRITE		0x2
#define DMEXEC		0x1

typedef struct Qid Qid;
struct Qid {
	uvlong	path;
	ulong	vers;
	uchar	type;
};

typedef struct Dir Dir;
struct Dir {
	vlong	length;
};

/* memory */
void*	p9malloc(usize);
void*	p9mallocz(usize, int);
void*	p9calloc(usize, usize);
void*	p9realloc(void*, usize);
void	p9free(void*);
#ifdef _KERNEL
#undef malloc
#undef free
#undef realloc
#define malloc(n)	p9malloc(n)
#define free(p)		p9free(p)
#define realloc(p, n)	p9realloc(p, n)
#define calloc(n, m)	p9calloc(n, m)
#endif
#define mallocz(n, z)	p9mallocz(n, z)
#define setmalloctag(p, t)	((void)0)

/* strings */
char*	strecpy(char*, char*, char*);
#ifdef _KERNEL
long long	strtoll(const char*, char**, int);
#endif

/* formatted printing */
typedef struct Fmt Fmt;
struct Fmt {
	char	*start;		/* buffer */
	char	*to;		/* next byte */
	char	*stop;		/* end of the buffer, room for the NUL excluded */
	int	nfmt;		/* bytes wanted, including any cut off */
	va_list	args;
	int	r;		/* the verb */
	int	width;
	int	prec;
	ulong	flags;
};

enum {
	FmtWidth	= 1,
	FmtLeft		= FmtWidth << 1,
	FmtPrec		= FmtLeft << 1,
	FmtSharp	= FmtPrec << 1,
	FmtSpace	= FmtSharp << 1,
	FmtSign		= FmtSpace << 1,
	FmtZero		= FmtSign << 1,
	FmtUnsigned	= FmtZero << 1,
	FmtShort	= FmtUnsigned << 1,
	FmtLong		= FmtShort << 1,
	FmtVLong	= FmtLong << 1,
	FmtComma	= FmtVLong << 1,
	FmtByte		= FmtComma << 1,
};

int	fmtinstall(int, int (*)(Fmt*));
int	fmtprint(Fmt*, char*, ...);
int	fmtstrcpy(Fmt*, char*);
char*	vseprint(char*, char*, char*, va_list);
char*	seprint(char*, char*, char*, ...);
int	vsnprint(char*, int, char*, va_list);
int	snprint(char*, int, char*, ...);
int	fprint(int, char*, ...);
int	print(char*, ...);
char*	smprint(char*, ...);
int	encodefmt(Fmt*);
void	werrstr(char*, ...);
char*	p9errstr(void);

/* locks */
typedef struct Lock Lock;
typedef struct QLock QLock;
typedef struct RWLock RWLock;
typedef struct Rendez Rendez;

#ifdef _KERNEL
struct Lock	{ kmutex_t m; volatile uint init; };
struct QLock	{ kmutex_t m; volatile uint init; };
struct RWLock	{ krwlock_t l; volatile uint init; };
struct Rendez	{ QLock *l; kcondvar_t cv; volatile uint init; };
#else
struct Lock	{ pthread_mutex_t m; volatile uint init; };
struct QLock	{ pthread_mutex_t m; volatile uint init; };
struct RWLock	{ pthread_rwlock_t l; volatile uint init; };
struct Rendez	{ QLock *l; pthread_cond_t cv; volatile uint init; };
#endif

void	lock(Lock*);
int	canlock(Lock*);
void	unlock(Lock*);
void	qlock(QLock*);
int	canqlock(QLock*);
void	qunlock(QLock*);
void	rlock(RWLock*);
int	canrlock(RWLock*);
void	runlock(RWLock*);
void	wlock(RWLock*);
int	canwlock(RWLock*);
void	wunlock(RWLock*);
void	rsleep(Rendez*);
int	rwakeup(Rendez*);
int	rwakeupall(Rendez*);
/* for structures that are freed: undo a lazy initialization */
void	lockfini(Lock*);
void	qlockfini(QLock*);
void	rwlockfini(RWLock*);
void	rendezfini(Rendez*);

/* atomics, sequentially consistent like Plan 9's */
#define P9A	__ATOMIC_SEQ_CST
static inline long	agetl(long *p)			{ return __atomic_load_n(p, P9A); }
static inline long	aincl(long *p, long v)		{ return __atomic_add_fetch(p, v, P9A); }
static inline long	aswapl(long *p, long v)		{ return __atomic_exchange_n(p, v, P9A); }
static inline int	acasl(long *p, long o, long n)	{ return __atomic_compare_exchange_n(p, &o, n, 0, P9A, P9A); }
static inline vlong	agetv(vlong *p)			{ return __atomic_load_n(p, P9A); }
static inline vlong	aincv(vlong *p, vlong v)	{ return __atomic_add_fetch(p, v, P9A); }
static inline vlong	aswapv(vlong *p, vlong v)	{ return __atomic_exchange_n(p, v, P9A); }
static inline int	acasv(vlong *p, vlong o, vlong n) { return __atomic_compare_exchange_n(p, &o, n, 0, P9A, P9A); }
static inline void*	agetp(void **p)			{ return __atomic_load_n(p, P9A); }
static inline void*	aswapp(void **p, void *v)	{ return __atomic_exchange_n(p, v, P9A); }
static inline int	acasp(void **p, void *o, void *n) { return __atomic_compare_exchange_n(p, &o, n, 0, P9A, P9A); }
#define coherence()	__atomic_thread_fence(P9A)

/* time */
vlong	nsec(void);
void	p9sleep(long);
#define sleep(ms)	p9sleep(ms)

/* the rest */
#define getcallerpc(x)	((uintptr)__builtin_return_address(0))
#ifdef _KERNEL
#define abort()		panic("gefs: abort at %s:%d", __FILE__, __LINE__)
#undef assert
#define assert(x)	((x) ? (void)0 : panic("gefs: assertion \"%s\" failed at %s:%d", #x, __FILE__, __LINE__))
#endif
_Noreturn void	sysfatal(char*, ...);
_Noreturn void	exits(char*);
Dir*	dirfstat(int);

/* per-thread state, see the top of this file */
struct Gefs;
struct Errctx;
struct Gefs**	p9fsp(void);
struct Errctx**	p9errctxp(void);
void	p9setfs(struct Gefs*);
#define fs	(*p9fsp())
#define errctx	(p9errctxp())

#endif
