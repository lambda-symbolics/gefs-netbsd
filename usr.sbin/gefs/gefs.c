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
#include "host.h"

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
	hostinit(64*MiB, 0);
	if(strcmp(cmd, "ream") == 0){
		reamfs(dev);
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
	}else if(strcmp(cmd, "info") == 0){
		print("narena %d nextqid %lld nextgen %lld snap %B\n",
			fs->narena, fs->nextqid, fs->nextgen, fs->snap.bp);
	}else
		usage();
	exits(nil);
}
