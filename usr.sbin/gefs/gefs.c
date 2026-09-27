/*
 * gefs(8) userland: ream (newfs), check (fsck) and info, with the core
 * linked in.  Runs on NetBSD and Linux, on a device or on a plain file.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"

static vlong	cachesz = 64*MiB;

void
loadusers(Tree *t)
{
	USED(t);	/* NetBSD uses numeric ids; 9front's user table is kept but unused */
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

static void
initfs(vlong csz, int rdonly)
{
	Bfree *f, *g, *bfbuf;
	Blk *b;
	Gefs *nfs;

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
}

static void
usage(void)
{
	fprint(2, "usage: gefs ream [-u user] dev\n"
		"       gefs check dev\n"
		"       gefs info dev\n");
	exits("usage");
}

int
main(int argc, char **argv)
{
	char *cmd, *dev;

	if(argc < 3)
		usage();
	cmd = argv[1];
	if(strcmp(cmd, "ream") == 0 && argc == 5 && strcmp(argv[2], "-u") == 0){
		reamuser = argv[3];
		dev = argv[4];
	}else if(argc == 3)
		dev = argv[2];
	else
		usage();
	initfs(cachesz, 0);
	initshow();
	fmtinstall('H', encodefmt);
	fmtinstall('B', Bconv);
	fmtinstall('M', Mconv);
	fmtinstall('P', Pconv);
	fmtinstall('K', Kconv);
	fmtinstall('R', Rconv);
	fmtinstall('Q', Qconv);
	if(strcmp(cmd, "ream") == 0){
		reamfs(dev);
		exits(nil);
	}
	loadfs(dev);
	if(strcmp(cmd, "check") == 0){
		qlock(&fs->mutlk);
		if(!checkfs(2))
			sysfatal("broken fs");
		qunlock(&fs->mutlk);
		print("ok\n");
	}else if(strcmp(cmd, "info") == 0){
		print("narena %d nextqid %lld nextgen %lld snap %B\n",
			fs->narena, fs->nextqid, fs->nextgen, fs->snap.bp);
	}else
		usage();
	exits(nil);
}
