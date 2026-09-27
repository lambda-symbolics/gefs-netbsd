/*
 * What the userland tools need around the core: the file system state
 * (9front's main.c initfs), device flushes, and 9front's user table,
 * which NetBSD does not use.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"
#include "host.h"

void
loadusers(Tree *t)
{
	USED(t);
}

/* 9P stat names; NetBSD shows ids as numbers */
User*
uid2user(int id)
{
	static __thread User u;

	u.id = id;
	snprint(u.name, sizeof(u.name), "%d", id);
	return &u;
}

int
p9devflush(int fd)
{
	return fdatasync(fd);
}

void
hostinit(vlong csz, int rdonly)
{
	Bfree *f, *g, *bfbuf;
	Blk *b;
	Gefs *nfs;
	int i;

	if((nfs = mallocz(sizeof(Gefs), 1)) == nil)
		sysfatal("malloc: %r");
	p9setfs(nfs);
	aswapl(&fs->rdonly, rdonly);
	fs->lrurz.l = &fs->lrulk;
	fs->syncrz.l = &fs->synclk;
	fs->bfreerz.l = &fs->bfreelk;
	fs->cmax = csz/Blksz;
	if((fs->bcache = mallocz(fs->cmax*sizeof(Blk*), 1)) == nil)
		sysfatal("malloc: %r");
	fs->dlcount = 0;
	fs->dlcmax = fs->cmax/10;
	if(fs->dlcmax < 4)
		fs->dlcmax = 4;
	if(fs->dlcmax > 512)
		fs->dlcmax = 512;
	if((fs->dlcache = mallocz(fs->dlcmax*sizeof(Dlist*), 1)) == nil)
		sysfatal("malloc: %r");
	if((bfbuf = calloc(fs->cmax, sizeof(Bfree))) == nil)
		sysfatal("malloc: %r");
	g = nil;
	for(f = bfbuf; f != bfbuf+fs->cmax; f++){
		f->bp = Zb;
		f->next = g;
		g = f;
	}
	fs->bfree = g;
	if((blkbuf = calloc(fs->cmax, sizeof(Blk))) == nil)
		sysfatal("malloc: %r");
	for(b = blkbuf; b != blkbuf+fs->cmax; b++){
		b->bp = Zb;
		b->magic = Magic;
		lrutop(b);
	}
	initshow();
	fmtinstall('H', encodefmt);
	fmtinstall('B', Bconv);
	fmtinstall('M', Mconv);
	fmtinstall('P', Pconv);
	fmtinstall('K', Kconv);
	fmtinstall('R', Rconv);
	fmtinstall('Q', Qconv);
	/* one write queue; nothing drains it but commits and pressure */
	fs->nsyncers = 1;
	for(i = 0; i < fs->nsyncers; i++)
		qinit(&fs->syncq[i]);
}

/* after loadfs(): attach the write queue to the arenas */
void
hostloaded(void)
{
	int i;

	for(i = 0; i < fs->narena; i++)
		fs->arenas[i].sync = &fs->syncq[i%fs->nsyncers];
}
