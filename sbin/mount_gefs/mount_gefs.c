/*
 * mount_gefs [-o options] special node
 *
 *	options: the usual (ro, rw, update, noatime, nodev, nosuid,
 *	noexec, sync, async, force, noauto), and
 *	snap=LABEL	mount this snapshot label (default main)
 *	cache=MB	block cache size
 */
#include <sys/param.h>
#include <sys/mount.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../sys/fs/gefs/gefs.h"

static const struct {
	const char	*name;
	int		flag;
} flags[] = {
	{ "ro",		MNT_RDONLY },
	{ "rdonly",	MNT_RDONLY },
	{ "update",	MNT_UPDATE },
	{ "noatime",	MNT_NOATIME },
	{ "nodev",	MNT_NODEV },
	{ "nosuid",	MNT_NOSUID },
	{ "noexec",	MNT_NOEXEC },
	{ "sync",	MNT_SYNCHRONOUS },
	{ "async",	MNT_ASYNC },
	{ "force",	MNT_FORCE },
	{ "log",	0 },
	{ "noauto",	0 },
	{ "rw",		0 },
};

static void
usage(void)
{
	fprintf(stderr, "usage: mount_gefs [-o options] special node\n");
	exit(1);
}

static void
options(char *s, int *mflags, struct gefs_args *a)
{
	char *o, *v;
	size_t i;

	for(o = strtok(s, ","); o != NULL; o = strtok(NULL, ",")){
		if((v = strchr(o, '=')) != NULL)
			*v++ = 0;
		if(strcmp(o, "snap") == 0 && v != NULL){
			if(strlcpy(a->snap, v, sizeof(a->snap)) >= sizeof(a->snap))
				errx(1, "snapshot label too long");
			continue;
		}
		if(strcmp(o, "cache") == 0 && v != NULL){
			a->cachemb = atoi(v);
			continue;
		}
		if(strcmp(o, "rw") == 0){
			*mflags &= ~MNT_RDONLY;
			continue;
		}
		for(i = 0; i < __arraycount(flags); i++)
			if(strcmp(o, flags[i].name) == 0){
				*mflags |= flags[i].flag;
				break;
			}
		if(i == __arraycount(flags))
			errx(1, "unknown option %s", o);
	}
}

int
main(int argc, char **argv)
{
	struct gefs_args a;
	char special[MAXPATHLEN], node[MAXPATHLEN];
	int ch, mflags;

	memset(&a, 0, sizeof(a));
	mflags = 0;
	while((ch = getopt(argc, argv, "o:")) != -1){
		switch(ch){
		case 'o':
			options(optarg, &mflags, &a);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if(argc != 2)
		usage();
	if(realpath(argv[0], special) == NULL)
		err(1, "%s", argv[0]);
	if(realpath(argv[1], node) == NULL)
		err(1, "%s", argv[1]);
	a.fspec = special;
	if(mount(MOUNT_GEFS, node, mflags, &a, sizeof(a)) == -1)
		err(1, "%s on %s", special, node);
	return 0;
}
