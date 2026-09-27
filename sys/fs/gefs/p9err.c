/*
 * Error handling, allocation and small helpers the gefs core takes from
 * 9front's main.c and libc; shared by the kernel and userland.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"

int	debug;
int	permissive;
int	usereserve;
char	*reamuser = "glenda";
int	noneid = 0;
int	nogroupid = 9999;
int	admid = -1;
int	fuzzonly;
ulong	fuzzseed;
Blk	*blkbuf;

_Noreturn void	p9longjmp(jmp_buf);

jmp_buf*
_waserror(void)
{
	Errctx *c;

	c = *errctx;
	c->nerrlab++;
	assert(c->nerrlab > 0 && c->nerrlab < Estacksz);
	return c->errlab + (c->nerrlab-1);
}

_Noreturn static void
errorv(char *fmt, va_list ap, int isbroke)
{
	Errctx *c;

	c = *errctx;
	vsnprint(c->err, sizeof(c->err), fmt, ap);
	if(isbroke)
		fprint(2, "gefs: %s\n", c->err);
	assert(c->nerrlab > 0 && c->nerrlab < Estacksz);
	p9longjmp(c->errlab[--c->nerrlab]);
}

/*
 * The file system is broken: stop writing to it, and fail the operation.
 * The disk keeps the last commit.
 */
_Noreturn void
broke(char *fmt, ...)
{
	va_list ap;

	aincl(&fs->rdonly, 1);
	va_start(ap, fmt);
	errorv(fmt, ap, 1);
}

_Noreturn void
error(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	errorv(fmt, ap, 0);
}

_Noreturn void
nexterror(void)
{
	Errctx *c;

	c = *errctx;
	assert(c->nerrlab > 0 && c->nerrlab < Estacksz);
	p9longjmp(c->errlab[--c->nerrlab]);
}

_Noreturn void
_babort(vlong addr, char *msg)
{
	fprint(2, "gefs: assert failed for [%llx]: %s\n", addr, msg);
	abort();
}

void
_trace(char *msg, Bptr bp, vlong v0, vlong v1)
{
	Trace *t;
	ulong idx;

	idx = aincl(&fs->traceidx, 1);
	t = &fs->trace[(idx-1) % fs->ntrace];
	strecpy(t->msg, t->msg+sizeof(t->msg), msg);
	t->tid = (*errctx)->tid;
	t->qgen = agetv(&fs->qgen);
	t->bp = bp;
	t->v0 = v0;
	t->v1 = v1;
}

void*
emalloc(usize sz, int zero)
{
	void *p;

	if((p = mallocz(sz, zero)) == nil)
		error(Enomem);
	return p;
}

char*
estrdup(char *s)
{
	char *p;
	usize n;

	n = strlen(s) + 1;
	p = emalloc(n, 0);
	memcpy(p, s, n);
	return p;
}

char*
strecpy(char *to, char *e, char *from)
{
	if(to >= e)
		return to;
	while(to < e - 1 && *from != 0)
		*to++ = *from++;
	*to = 0;
	return to;
}

char*
smprint(char *fmt, ...)
{
	va_list args;
	char buf[256], *p;
	int n;

	va_start(args, fmt);
	n = vsnprint(buf, sizeof(buf), fmt, args);
	va_end(args);
	if(n < (int)sizeof(buf) - 1){
		p = malloc(n + 1);
		memcpy(p, buf, n + 1);
		return p;
	}
	/* long: format again into a buffer of the right size */
	n = 16*1024;
	p = malloc(n);
	va_start(args, fmt);
	vsnprint(p, n, fmt, args);
	va_end(args);
	return p;
}
