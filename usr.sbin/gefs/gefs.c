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
#include "ops.h"
#include "host.h"

static void
usage(void)
{
	fprint(2, "usage: gefs ream [-9] [-u user] dev\n"
		"       gefs check dev\n"
		"       gefs info dev\n"
		"       newfs_gefs [-9] [-u user] dev\n"
		"       fsck_gefs dev\n");
	exits("usage");
}

/*
 * A NetBSD root directory: owned by root:wheel, 0755, created now.
 * 9front's ream leaves it to adm (-1), 0775, at the epoch; -9 keeps that.
 */
static void
netbsdroot(char *dev)
{
	Mount *mnt;
	Dent *root;
	Gattr a;

	loadfs(dev);
	hostloaded();
	mnt = getmount("main");
	root = gefsroot(mnt);
	memset(&a, 0, sizeof(a));
	a.perm = 0755;
	a.uid = 0;
	a.gid = 0;
	a.atime = a.mtime = nsec();
	gefssetattr(mnt, root, &a, Aperm|Auid|Agid|Aatime|Amtime, 0);
	/* commit while the label is still mounted, or its tree is not synced */
	drainsyncq();
	clunkdent(mnt, root);
	clunkmount(mnt);
}

static int
lsent(void *arg, Xdir *d)
{
	USED(arg);
	print("%s qid %llx mode %luo uid %d gid %d len %lld mtime %lld\n",
		d->name, d->qid.path, d->mode, d->uid, d->gid, d->length, d->mtime);
	return 0;
}

/* the root of main and its entries, for debugging */
static void
lsroot(void)
{
	Mount *mnt;
	Dent *root;
	Scan s;

	mnt = getmount("main");
	root = gefsroot(mnt);
	print("/ qid %llx mode %luo uid %d gid %d mtime %lld\n", root->qid.path,
		root->mode, root->uid, root->gid, root->mtime);
	memset(&s, 0, sizeof(s));
	gefsreaddir(mnt, root, &s, lsent, nil);
	clunkdent(mnt, root);
	clunkmount(mnt);
}

int
main(int argc, char **argv)
{
	char *cmd, *dev, *p;
	int i, plan9;

	cmd = nil;
	p = strrchr(argv[0], '/');
	p = p ? p+1 : argv[0];
	if(strcmp(p, "newfs_gefs") == 0)
		cmd = "ream";
	else if(strcmp(p, "fsck_gefs") == 0)
		cmd = "check";
	i = 1;
	if(cmd == nil){
		if(argc < 3)
			usage();
		cmd = argv[i++];
	}
	plan9 = 0;
	for(; i < argc - 1; i++){
		if(strcmp(argv[i], "-9") == 0)
			plan9 = 1;
		else if(strcmp(argv[i], "-u") == 0 && i+1 < argc-1)
			reamuser = argv[++i];
		else if(argv[i][0] == '-' && strcmp(cmd, "check") == 0)
			continue;	/* fsck(8) options: -p, -y, -f */
		else
			usage();
	}
	if(i != argc - 1)
		usage();
	dev = argv[i];
	if(waserror())
		sysfatal("%s: %s", dev, errmsg());
	hostinit(64*MiB, 0);
	if(strcmp(cmd, "ream") == 0){
		reamfs(dev);
		if(!plan9){
			hostinit(64*MiB, 0);
			netbsdroot(dev);
		}
		exits(nil);
	}
	if(strcmp(cmd, "fixroot") == 0){
		netbsdroot(dev);
		exits(nil);
	}
	loadfs(dev);
	hostloaded();
	if(strcmp(cmd, "check") == 0){
		qlock(&fs->mutlk);
		if(!checkfs(2))
			sysfatal("broken fs");
		qunlock(&fs->mutlk);
		print("ok\n");
	}else if(strcmp(cmd, "ls") == 0){
		lsroot();
	}else if(strcmp(cmd, "info") == 0){
		print("narena %d nextqid %lld nextgen %lld snap %B\n",
			fs->narena, fs->nextqid, fs->nextgen, fs->snap.bp);
	}else
		usage();
	poperror();
	exits(nil);
}
