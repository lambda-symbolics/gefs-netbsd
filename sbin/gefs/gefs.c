/*
 * gefs(8): make, check and administer gefs file systems, with the core
 * linked in.  Runs on NetBSD and Linux, on a device or a plain file.
 *
 * The administration commands take a path on a mounted gefs, and the
 * kernel runs them (GEFSIOC_CTL, GEFSIOC_STATUS); or a device that is
 * not mounted (or an image file), which the tool loads and writes itself.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#ifdef __NetBSD__
#include <sys/statvfs.h>
#endif
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"
#include "host.h"
#include "gefs.h"

/* what newfs_gefs sets unless told otherwise: 9front's keeps every daily */
#define Retainnew	"60@m 24@h 14@d"

static void
usage(void)
{
	fprint(2,
	    "usage: gefs ream [-9] [-r spec] [-u user] dev\n"
	    "       gefs check path|dev\n"
	    "       gefs status path|dev\n"
	    "       gefs snap [-m] path|dev label new\n"
	    "       gefs fork path|dev label new\n"
	    "       gefs delsnap path|dev label\n"
	    "       gefs retain path|dev label|- spec...\n"
	    "       gefs sync path\n"
	    "       gefs ctl path|dev command...\n"
	    "       gefs info dev\n"
	    "       newfs_gefs [-9] [-r spec] [-u user] dev\n"
	    "       fsck_gefs [-fpy] dev\n");
	exits("usage");
}

/*
 * A NetBSD root directory: owned by root:wheel, 0755, created now; and
 * the default snapshot schedule.  9front's ream leaves the root to adm
 * (-1), 0775, at the epoch, and sets no schedule; -9 keeps that.
 */
static void
netbsdroot(char *dev, char *retain)
{
	char cmd[256];
	Mount *mnt;
	Dent *root;
	Gattr a;

	loadfs(dev);
	hostloaded();
	mnt = getmount("main");
	if(retain != nil){
		/* before the root changes, so this takes no snapshot */
		snprint(cmd, sizeof(cmd), "retain - %s", retain);
		ctlcmd(cmd, 2);
	}
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

/* a device the kernel has mounted must not be written behind its back */
static int
ismounted(char *dev)
{
#ifdef __NetBSD__
	struct statvfs *m;
	char raw[1024], *p;
	int i, n;

	n = getmntinfo(&m, MNT_NOWAIT);
	/* /dev/rld0d is /dev/ld0d */
	snprint(raw, sizeof(raw), "%s", dev);
	if((p = strrchr(raw, '/')) != nil && p[1] == 'r')
		memmove(p+1, p+2, strlen(p+2)+1);
	for(i = 0; i < n; i++)
		if(strcmp(m[i].f_mntfromname, dev) == 0
		|| strcmp(m[i].f_mntfromname, raw) == 0)
			return 1;
#else
	USED(dev);
#endif
	return 0;
}

/* 9front quoting, for arguments with blanks or quotes */
static char*
quote(char *p, char *e, char *s)
{
	if(*s != 0 && strpbrk(s, " \t\n'") == nil)
		return seprint(p, e, "%s", s);
	p = seprint(p, e, "'");
	for(; *s != 0; s++)
		p = seprint(p, e, (*s == '\'') ? "''" : "%c", *s);
	return seprint(p, e, "'");
}

enum { Kernel, Offline };

/*
 * Run cmd, or read the status, in the kernel; Offline if path is not on
 * a gefs (and not a directory, which then is not on one either).
 */
static int
kernel(char *path, char *cmd)
{
	struct gefs_ctl gc;
	struct stat st;
	size_t sz;
	char *buf;
	int fd, r, e;

	if((fd = open(path, O_RDONLY)) == -1)
		sysfatal("%s: %r", path);
	if(fstat(fd, &st) == -1)
		sysfatal("%s: %r", path);
	if(S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)){
		close(fd);
		return Offline;
	}
	sz = 64*1024;
	for(;;){
		memset(&gc, 0, sizeof(gc));
		if(cmd != nil)
			snprint(gc.gc_cmd, sizeof(gc.gc_cmd), "%s", cmd);
		if((buf = calloc(1, sz)) == nil)
			sysfatal("malloc: %r");
		gc.gc_buf = buf;
		gc.gc_len = sz;
		r = ioctl(fd, cmd != nil ? GEFSIOC_CTL : GEFSIOC_STATUS, &gc);
		e = errno;
		if(r == 0 && cmd == nil && gc.gc_len > sz){
			free(buf);
			sz = gc.gc_len + 1;
			continue;
		}
		break;
	}
	close(fd);
	if(r == -1 && e == ENOTTY){
		if(S_ISDIR(st.st_mode))
			sysfatal("%s: not on a gefs file system", path);
		free(buf);
		return Offline;
	}
	/* a failed ioctl copies nothing back but what the kernel put in buf */
	if(r == -1){
		buf[sz-1] = 0;
		if(buf[0] != 0)
			fprint(2, "gefs: %s", buf);
		else
			fprint(2, "gefs: %s: %s\n", cmd ? cmd : "status", strerror(e));
		exits("failed");
	}
	if(gc.gc_len > sz)
		gc.gc_len = sz;
	if(gc.gc_len > 0)
		write(1, buf, gc.gc_len);
	free(buf);
	return Kernel;
}

/* the same, on a device that is not mounted */
static void
offline(char *dev, char *cmd)
{
	char *st, *e;

	if(ismounted(dev))
		sysfatal("%s is mounted; give a path on it", dev);
	hostinit(64*MiB, cmd == nil);
	loadfs(dev);
	hostloaded();
	if(cmd == nil){
		st = emalloc(4*MiB, 0);
		e = seprintstatus(st, st + 4*MiB);
		write(1, st, e - st);
		return;
	}
	ctlcmd(cmd, 2);
	drainsyncq();
}

static void
admin(char *path, char *cmd)
{
	if(kernel(path, cmd) == Offline)
		offline(path, cmd);
}

int
main(int argc, char **argv)
{
	char *cmd, *dev, *p, *e, *retain, buf[256];
	int i, plan9, mut;

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
	if(waserror())
		sysfatal("%s", errmsg());

	/* administration: gefs cmd path args... */
	mut = 0;
	if(strcmp(cmd, "snap") == 0 && i < argc && strcmp(argv[i], "-m") == 0){
		mut = 1;
		i++;
	}
	if(strcmp(cmd, "status") == 0){
		if(argc - i != 1)
			usage();
		admin(argv[i], nil);
		exits(nil);
	}
	if(strcmp(cmd, "snap") == 0 || strcmp(cmd, "fork") == 0
	|| strcmp(cmd, "delsnap") == 0 || strcmp(cmd, "retain") == 0
	|| strcmp(cmd, "sync") == 0 || strcmp(cmd, "ctl") == 0){
		if((strcmp(cmd, "snap") == 0 || strcmp(cmd, "fork") == 0) && argc - i != 3)
			usage();
		if(strcmp(cmd, "delsnap") == 0 && argc - i != 2)
			usage();
		if(strcmp(cmd, "retain") == 0 && argc - i < 3)
			usage();
		if(strcmp(cmd, "sync") == 0 && argc - i != 1)
			usage();
		if(strcmp(cmd, "ctl") == 0 && argc - i < 2)
			usage();
		p = buf;
		e = buf + sizeof(buf);
		if(strcmp(cmd, "ctl") != 0)
			p = seprint(p, e, "%s", mut ? "fork" : cmd);
		for(dev = argv[i++]; i < argc; i++){
			if(p != buf)
				p = seprint(p, e, " ");
			/* ctl takes the command as words, or as one argument */
			p = (strcmp(cmd, "ctl") == 0) ? seprint(p, e, "%s", argv[i])
				: quote(p, e, argv[i]);
		}
		if(p >= e-1)
			sysfatal("command too long");
		admin(dev, buf);
		exits(nil);
	}

	/* ream, check, info, ls: gefs cmd [options] dev */
	plan9 = 0;
	retain = Retainnew;
	for(; i < argc - 1; i++){
		if(strcmp(argv[i], "-9") == 0){
			plan9 = 1;
			retain = nil;
		}else if(strcmp(argv[i], "-u") == 0 && i+1 < argc-1)
			reamuser = argv[++i];
		else if(strcmp(argv[i], "-r") == 0 && i+1 < argc-1)
			retain = argv[++i];
		else if(argv[i][0] == '-' && strcmp(cmd, "check") == 0)
			continue;	/* fsck(8) options: -p, -y, -f */
		else
			usage();
	}
	if(i != argc - 1)
		usage();
	dev = argv[i];
	poperror();
	if(waserror())
		sysfatal("%s: %s", dev, errmsg());
	if(strcmp(cmd, "ream") == 0){
		if(ismounted(dev))
			sysfatal("%s is mounted", dev);
		hostinit(64*MiB, 0);
		reamfs(dev);
		if(!plan9 || retain != nil){
			hostinit(64*MiB, 0);
			netbsdroot(dev, retain);
		}
		exits(nil);
	}
	if(strcmp(cmd, "check") == 0){
		struct stat st;

		if(stat(dev, &st) == 0 && S_ISDIR(st.st_mode)){
			admin(dev, "check");
			exits(nil);
		}
		hostinit(64*MiB, 1);
		loadfs(dev);
		hostloaded();
		qlock(&fs->mutlk);
		if(!checkfs(2))
			sysfatal("broken fs");
		qunlock(&fs->mutlk);
		print("ok\n");
		exits(nil);
	}
	if(strcmp(cmd, "fixroot") == 0){
		hostinit(64*MiB, 0);
		netbsdroot(dev, nil);
		exits(nil);
	}
	hostinit(64*MiB, 1);
	loadfs(dev);
	hostloaded();
	if(strcmp(cmd, "ls") == 0){
		lsroot();
	}else if(strcmp(cmd, "info") == 0){
		print("narena %d nextqid %lld nextgen %lld snap %B\n",
			fs->narena, fs->nextqid, fs->nextgen, fs->snap.bp);
	}else
		usage();
	poperror();
	exits(nil);
}
