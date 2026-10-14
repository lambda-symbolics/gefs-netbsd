/*
 * GEFS for NetBSD: file system operations (mount, unmount, sync, ...)
 * and the module.
 */
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/module.h>
#include <sys/disk.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/conf.h>
#include <miscfs/genfs/genfs.h>
#include <miscfs/specfs/specdev.h>

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"
#include "gefs.h"

MODULE(MODULE_CLASS_VFS, gefs, NULL);

static int	gefs_cachemb = 64;	/* default block cache, MiB */
int		gefs_trace;		/* vfs.gefs.trace: log page I/O and truncation */
static struct sysctllog *gefs_sysctl_log;

static krwlock_t	gefs_mntlock;	/* orders mounts and unmounts, guards the lists */
static LIST_HEAD(, gefsdev) gefs_devs = LIST_HEAD_INITIALIZER(gefs_devs);

/*
 * vfs.gefs.{commits,skipped,reads,writes,flushes,...}: totals over the
 * mounted devices, for measuring what the disk does (iostat counts
 * transfers; these count commits, the cache flushes that wake a drive,
 * and blocks), and why the commits ran.
 */
static int
gefs_sysctl_stat(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct gefsdev *gd;
	long long r, w, f, v;
	int which = node.sysctl_data == NULL ? 0 : *(int *)node.sysctl_data;
	quad_t q;

	q = 0;
	rw_enter(&gefs_mntlock, RW_READER);
	LIST_FOREACH(gd, &gefs_devs, gd_link){
		gefsdevstats(gd->gd_fd, &r, &w, &f);
		switch(which){
		case 0: v = gd->gd_fs->ncommit; break;
		case 1: v = gd->gd_fs->nskip; break;
		case 2: v = r; break;
		case 3: v = w; break;
		case 4: v = f; break;
		case 5: v = gd->gd_nreq; break;
		case 6: v = gd->gd_njoin; break;
		default: v = gd->gd_nwhy[which - 7]; break;
		}
		q += v;
	}
	rw_exit(&gefs_mntlock);
	node.sysctl_data = &q;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

static int gefs_statsel[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

/* a tunable changed: the mutators re-read it */
static int
gefs_sysctl_tune(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct gefsdev *gd;
	int error, v;

	v = *(int *)rnode->sysctl_data;
	node.sysctl_data = &v;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if(error || newp == NULL)
		return error;
	if(v < 0 || (rnode->sysctl_data == &gefs_dirtypct && v > 100) ||
	    (rnode->sysctl_data == &gefs_groupwait && v > 1000000))
		return EINVAL;
	*(int *)rnode->sysctl_data = v;
	rw_enter(&gefs_mntlock, RW_READER);
	LIST_FOREACH(gd, &gefs_devs, gd_link)
		gefs_kick(gd);
	rw_exit(&gefs_mntlock);
	return 0;
}

static void
gefs_sysctl_init(void)
{
	static const char *names[] = { "commits", "skipped", "reads", "writes",
		"flushes", "requests", "joined", "commits_sync", "commits_age",
		"commits_dirty", "commits_space", "commits_lazy" };
	static const char *descr[] = {
		"commits written", "commits skipped, nothing changed",
		"blocks read", "blocks written", "device cache flushes",
		"durability requests (fsync, O_SYNC writes, sync)",
		"requests that started no commit of their own",
		"commits run for requests", "commits run for the interval",
		"commits run for dirty memory", "commits run for space",
		"commits run for lazy updates alone" };
	static const struct { const char *n, *d; int *v; } tune[] = {
		{ "interval", "seconds a change may wait for a commit, at most",
		    &gefs_interval },
		{ "groupwait", "microseconds a requested commit waits for more requests",
		    &gefs_groupwait },
		{ "dirtypct", "commit when queued blocks pass this % of the cache (0: off)",
		    &gefs_dirtypct },
		{ "lazymax", "seconds atime and deferred frees may wait alone for a commit",
		    &gefs_lazymax },
	};
	const struct sysctlnode *node;
	int i;

	sysctl_createv(&gefs_sysctl_log, 0, NULL, &node, CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "gefs", SYSCTL_DESCR("gefs"), NULL, 0, NULL, 0,
	    CTL_VFS, CTL_CREATE, CTL_EOL);
	if(node == NULL)
		return;
	sysctl_createv(&gefs_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "trace",
	    SYSCTL_DESCR("log page I/O and truncations"), NULL, 0,
	    &gefs_trace, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&gefs_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "cachemb",
	    SYSCTL_DESCR("block cache of a new mount, MiB"), NULL, 0,
	    &gefs_cachemb, 0, CTL_CREATE, CTL_EOL);
	for(i = 0; i < (int)__arraycount(tune); i++)
		sysctl_createv(&gefs_sysctl_log, 0, &node, NULL,
		    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, tune[i].n,
		    SYSCTL_DESCR(tune[i].d), gefs_sysctl_tune, 0,
		    tune[i].v, 0, CTL_CREATE, CTL_EOL);
	for(i = 0; i < (int)__arraycount(names); i++)
		sysctl_createv(&gefs_sysctl_log, 0, &node, NULL,
		    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_QUAD, names[i],
		    SYSCTL_DESCR(descr[i]), gefs_sysctl_stat, 0,
		    &gefs_statsel[i], 0, CTL_CREATE, CTL_EOL);
}

/* 9front's user table is kept on disk but not used: ids are numeric */
void
loadusers(Tree *t)
{
	USED(t);
}

User*
uid2user(int id)
{
	static User u;

	u.id = id;
	snprint(u.name, sizeof(u.name), "%d", id);
	return &u;
}

/* the core's state for one file system, from 9front's initfs */
static Gefs*
gefs_newfs(vlong csz, int rdonly)
{
	Bfree *f, *g;
	Blk *b;
	Gefs *nfs;
	long i;

	nfs = mallocz(sizeof(Gefs), 1);
	nfs->lrurz.l = &nfs->lrulk;
	nfs->syncrz.l = &nfs->synclk;
	nfs->bfreerz.l = &nfs->bfreelk;
	nfs->rdonly = rdonly;
	nfs->cmax = csz/Blksz;
	nfs->bcache = mallocz(nfs->cmax*sizeof(Blk*), 1);
	nfs->dlcount = 0;
	nfs->dlcmax = nfs->cmax/10;
	if(nfs->dlcmax < 4)
		nfs->dlcmax = 4;
	if(nfs->dlcmax > 512)
		nfs->dlcmax = 512;
	nfs->dlcache = mallocz(nfs->dlcmax*sizeof(Dlist*), 1);
	g = nil;
	for(i = 0; i < nfs->cmax; i++){
		f = mallocz(sizeof(Bfree), 1);
		f->bp = Zb;
		f->next = g;
		g = f;
	}
	nfs->bfree = g;
	/* the blocks go on the LRU with this file system current */
	p9setfs(nfs);
	nfs->blkall = mallocz(nfs->cmax*sizeof(Blk*), 1);
	for(i = 0; i < nfs->cmax; i++){
		b = mallocz(sizeof(Blk), 1);
		nfs->blkall[i] = b;
		b->bp = Zb;
		b->magic = Magic;
		lrutop(b);
	}
	nfs->nsyncers = 1;
	for(i = 0; i < nfs->nsyncers; i++)
		qinit(&nfs->syncq[i]);
	return nfs;
}

/*
 * Free what the core allocated for a file system, after drainsyncq.
 * The core never frees on 9front (it exits); this covers the blocks,
 * queues, arenas and caches, which is nearly all of the memory.
 */
void
gefs_freefs(Gefs *f)
{
	Bfree *bf, *nbf;
	Arange *r, *nr;
	Dlist *dl, *ndl;
	Arena *a;
	int i;

	p9setfs(f);
	for(i = 0; i < 3; i++)
		epochclean();
	for(i = 0; i < f->cmax; i++)
		free(f->blkall[i]);
	free(f->blkall);
	for(i = 0; f->arenas != nil && i < f->narena; i++){
		a = &f->arenas[i];
		if(a->free != nil){
			for(r = (Arange*)avlmin(a->free); r != nil; r = nr){
				nr = (Arange*)avlnext(r);
				avldelete(a->free, r);
				free(r);
			}
			free(a->free);
		}
		qlockfini(&a->QLock);
	}
	for(dl = f->dlhead; dl != nil; dl = ndl){
		ndl = dl->cnext;
		free(dl);
	}
	for(bf = f->bfree; bf != nil; bf = nbf){
		nbf = (Bfree*)bf->next;
		free(bf);
	}
	for(i = 0; i < f->nsyncers; i++)
		free(f->syncq[i].heap);
	free(f->arenas);
	free(f->arenabp);
	free(f->bcache);
	free(f->dlcache);
	free(f->snapmnt);
	free(f);
	p9setfs(nil);
}

/*
 * Mounts and devices.  The first label mounted from a device opens it
 * and loads the file system; more labels share it; the last unmount
 * writes everything out and closes it.  gefs_mntlock orders mounts and
 * unmounts, and guards the lists.
 */

/*
 * On the mutator: load the file system (for the first label, arg set),
 * then the label.  A label is mounted once: two NetBSD mounts of one
 * tree would cache its pages twice.  "dump", 9front's view of all the
 * snapshots, is not a label here.
 */
static void
gefs_loadjob(struct gefsmount *gm, void *arg)
{
	struct gefsdev *gd = gm->gm_gd;
	struct gefsmount *o;
	int i;

	if(arg != NULL){
		gefsdevpend(gd->gd_fd);
		loadfs("gefs");
		for(i = 0; i < fs->narena; i++)
			fs->arenas[i].sync = &fs->syncq[i%fs->nsyncers];
	}
	if(strcmp(gm->gm_label, "dump") == 0)
		gefserr(Eimpl);
	LIST_FOREACH(o, &gd->gd_mounts, gm_link)
		if(strcmp(o->gm_label, gm->gm_label) == 0)
			gefserr(Esnapu);
	if(!gm->gm_ronly && agetl(&fs->broken))
		gefserr(Erdonly);
	gm->gm_mnt = getmount(gm->gm_label);
	if(waserror()){
		if(gm->gm_rootde != nil)
			clunkdent(gm->gm_mnt, gm->gm_rootde);
		gm->gm_rootde = nil;
		clunkmount(gm->gm_mnt);
		gm->gm_mnt = nil;
		if(!gm->gm_ronly && gd->gd_nrw == 0)
			aswapl(&fs->rdonly, 1);
		nexterror();
	}
	if(!gm->gm_ronly && !(gm->gm_mnt->flag & Lmut))
		gefserr(Erdonly);
	gm->gm_rootde = gefsroot(gm->gm_mnt);
	if(!gm->gm_ronly){
		if(gd->gd_nrw == 0)
			aswapl(&fs->rdonly, 0);
		gefsorphans(gm->gm_mnt);
		gd->gd_nrw++;
	}
	poperror();
}

/*
 * On the mutator: everything to the disk, and let go of the label.
 * The commit is for the whole file system; it goes before the label is
 * let go of, as a commit only takes the labels still mounted.
 */
static void
gefs_unloadjob(struct gefsmount *gm, void *arg)
{
	struct gefsdev *gd = gm->gm_gd;

	USED(arg);
	if(!gm->gm_ronly && !agetl(&fs->rdonly))
		drainsyncq();
	clunkdent(gm->gm_mnt, gm->gm_rootde);
	clunkmount(gm->gm_mnt);
	if(!gm->gm_ronly && --gd->gd_nrw == 0)
		aswapl(&fs->rdonly, 1);
}

/* on the mutator: read-only to read-write (the root, once rc has run fsck) */
static void
gefs_rwjob(struct gefsmount *gm, void *arg)
{
	struct gefsdev *gd = gm->gm_gd;

	USED(arg);
	if(!(gm->gm_mnt->flag & Lmut) || agetl(&fs->broken))
		gefserr(Erdonly);
	if(gd->gd_nrw++ == 0)
		aswapl(&fs->rdonly, 0);
	gm->gm_ronly = 0;
	gefsorphans(gm->gm_mnt);
}

/* on the mutator: everything to the disk, then read-only */
static void
gefs_rojob(struct gefsmount *gm, void *arg)
{
	struct gefsdev *gd = gm->gm_gd;

	USED(arg);
	drainsyncq();
	gm->gm_ronly = 1;
	if(--gd->gd_nrw == 0)
		aswapl(&fs->rdonly, 1);
}

/*
 * Everything changed so far to the disk: wait for a durable commit that
 * holds it (shared with other callers, gefs_durable), or only ask for
 * one soon (sync(2)).
 */
int
gefs_commit(struct gefsmount *gm, int wait, int flags)
{
	if(wait)
		return gefs_durable(gm, flags);
	gefs_cmtrequest(gm, flags);
	return 0;
}

/*
 * On the mutator, after the syncer's page writes: those pages may have
 * been dirty since the syncer's previous pass, so the tree's changes
 * count as that old.  With the default interval (= syncdelay) the
 * syncer's pass commits, as before; a longer interval delays it.
 */
static void
gefs_agejob(struct gefsmount *gm, void *arg)
{
	struct gefsdev *gd = gm->gm_gd;
	struct timespec ts;
	uint64_t now, back;

	USED(arg);
	nanouptime(&ts);
	now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	back = (uint64_t)syncdelay * 1000000000ULL;
	mutex_enter(&gd->gd_jlk);
	gd->gd_lazyq = 0;
	if(gd->gd_fs->snap.dirty && now > back &&
	    (gd->gd_dirtyat == 0 || gd->gd_dirtyat > now - back))
		gd->gd_dirtyat = now - back;
	mutex_exit(&gd->gd_jlk);
}

static struct gefsmount*
gefs_newgm(struct gefsdev *gd, struct mount *mp, struct gefs_args *args)
{
	struct gefsmount *gm;

	gm = kmem_zalloc(sizeof(*gm), KM_SLEEP);
	gm->gm_mp = mp;
	gm->gm_gd = gd;
	gm->gm_fs = gd->gd_fs;
	gm->gm_ronly = (mp->mnt_flag & MNT_RDONLY) != 0;
	strlcpy(gm->gm_label, args->snap[0] ? args->snap : "main",
	    sizeof(gm->gm_label));
	return gm;
}

/* the NetBSD side of a mounted label */
static void
gefs_setupmp(struct gefsmount *gm, struct mount *mp, int first)
{
	struct gefsdev *gd = gm->gm_gd;

	mp->mnt_data = gm;
	if(first){
		mp->mnt_stat.f_fsidx.__fsid_val[0] = (long)gd->gd_dev;
		mp->mnt_stat.f_fsidx.__fsid_val[1] = makefstype(MOUNT_GEFS);
		mp->mnt_stat.f_fsid = mp->mnt_stat.f_fsidx.__fsid_val[0];
	}else{
		/* labels share qids (a fork has its base's): st_dev tells them apart */
		vfs_getnewfsid(mp);
	}
	mp->mnt_stat.f_namemax = Maxname;
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_iflag |= IMNT_MPSAFE;
	mp->mnt_fs_bshift = Lgblk;
	mp->mnt_dev_bshift = DEV_BSHIFT;
	LIST_INSERT_HEAD(&gd->gd_mounts, gm, gm_link);
}

/* the first label of a device: open the file system; with gefs_mntlock */
static int
gefs_mountfs(struct vnode *devvp, struct mount *mp, struct lwp *l,
    struct gefs_args *args, int openmode)
{
	struct gefsmount *gm;
	struct gefsdev *gd;
	uint64_t psize;
	unsigned secsize;
	vlong csz;
	int error;

	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = vinvalbuf(devvp, V_SAVE, l->l_cred, l, 0, 0);
	VOP_UNLOCK(devvp);
	if(error)
		return error;
	if((error = getdisksize(devvp, &psize, &secsize)) != 0)
		return error;

	gd = kmem_zalloc(sizeof(*gd), KM_SLEEP);
	gd->gd_dev = devvp->v_rdev;
	gd->gd_devvp = devvp;
	gd->gd_openmode = openmode;
	LIST_INIT(&gd->gd_mounts);
	gd->gd_fd = gefsdevadd(devvp, (long long)psize * secsize);
	if(gd->gd_fd < 0){
		kmem_free(gd, sizeof(*gd));
		return EMFILE;
	}
	csz = (vlong)(args->cachemb > 0 ? args->cachemb : gefs_cachemb) * MiB;
	gd->gd_fs = gefs_newfs(csz, 1);
	p9setfs(nil);
	gm = gefs_newgm(gd, mp, args);
	if((error = gefs_work_start(gd)) == 0){
		error = gefs_call(gm, gefs_loadjob, gd);
		if(error)
			gefs_work_stop(gd);
	}
	if(error){
		printf("gefs: mount %s: error %d\n", gm->gm_label, error);
		gefs_freefs(gd->gd_fs);
		gefsdevdel(gd->gd_fd);
		kmem_free(gm, sizeof(*gm));
		kmem_free(gd, sizeof(*gd));
		return error;
	}
	LIST_INSERT_HEAD(&gefs_devs, gd, gd_link);
	gefs_setupmp(gm, mp, 1);
	spec_node_setmountedfs(devvp, mp);
	return 0;
}

/* another label of a device that is mounted; with gefs_mntlock */
static int
gefs_mountlabel(struct gefsdev *gd, struct mount *mp, struct gefs_args *args)
{
	struct gefsmount *gm;
	int error;

	gm = gefs_newgm(gd, mp, args);
	if((error = gefs_call(gm, gefs_loadjob, NULL)) != 0){
		kmem_free(gm, sizeof(*gm));
		return error;
	}
	gefs_setupmp(gm, mp, 0);
	return 0;
}

static int
gefs_mount(struct mount *mp, const char *path, void *data, size_t *data_len)
{
	struct lwp *l = curlwp;
	struct gefs_args *args = data;
	struct gefsmount *gm;
	struct gefsdev *gd;
	struct vnode *devvp;
	int error, xflags;

	if(args == NULL || *data_len < sizeof(*args))
		return EINVAL;
	if(mp->mnt_flag & MNT_GETARGS){
		if((gm = VFSTOGEFS(mp)) == NULL)
			return EIO;
		args->fspec = NULL;
		strlcpy(args->snap, gm->gm_label, sizeof(args->snap));
		*data_len = sizeof(*args);
		return 0;
	}
	if(mp->mnt_flag & MNT_UPDATE){
		gm = VFSTOGEFS(mp);
		if(gm == NULL)
			return EINVAL;
		if(args->fspec != NULL){
			/* the same device (the root: its name, for "root_device") */
			error = namei_simple_user(args->fspec,
			    NSM_FOLLOW_NOEMULROOT, &devvp);
			if(error)
				return error;
			error = (devvp->v_type != VBLK ||
			    devvp->v_rdev != gm->gm_gd->gd_dev) ? EINVAL : 0;
			vrele(devvp);
			if(error)
				return error;
		}
		error = 0;
		rw_enter(&gefs_mntlock, RW_WRITER);
		if(gm->gm_ronly && (mp->mnt_iflag & IMNT_WANTRDWR)){
			/* the root is mounted read-only; rc makes it writable */
			error = gefs_call(gm, gefs_rwjob, NULL);
		}else if(!gm->gm_ronly && (mp->mnt_flag & MNT_RDONLY)){
			error = vflush(mp, NULLVP,
			    (mp->mnt_flag & MNT_FORCE) ? FORCECLOSE : 0);
			if(error == 0)
				error = gefs_call(gm, gefs_rojob, NULL);
		}
		rw_exit(&gefs_mntlock);
		if(error == 0 && args->fspec != NULL)
			error = set_statvfs_info(path, UIO_USERSPACE, args->fspec,
			    UIO_USERSPACE, mp->mnt_op->vfs_name, mp, l);
		return error;
	}
	if(args->fspec == NULL)
		return EINVAL;
	args->snap[sizeof(args->snap)-1] = 0;

	error = namei_simple_user(args->fspec, NSM_FOLLOW_NOEMULROOT, &devvp);
	if(error)
		return error;
	if(devvp->v_type != VBLK){
		vrele(devvp);
		return ENOTBLK;
	}
	if(bdevsw_lookup(devvp->v_rdev) == NULL){
		vrele(devvp);
		return ENXIO;
	}
	xflags = (mp->mnt_flag & MNT_RDONLY) ? FREAD : FREAD|FWRITE;
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = kauth_authorize_system(l->l_cred, KAUTH_SYSTEM_MOUNT,
	    KAUTH_REQ_SYSTEM_MOUNT_DEVICE, mp, devvp,
	    KAUTH_ARG((xflags & FWRITE) ? VREAD|VWRITE : VREAD));
	VOP_UNLOCK(devvp);
	if(error){
		vrele(devvp);
		return error;
	}

	rw_enter(&gefs_mntlock, RW_WRITER);
	LIST_FOREACH(gd, &gefs_devs, gd_link)
		if(gd->gd_dev == devvp->v_rdev)
			break;
	if(gd != NULL){
		/* the device is open: mount another of its labels */
		vrele(devvp);
		error = gefs_mountlabel(gd, mp, args);
		rw_exit(&gefs_mntlock);
		if(error)
			return error;
	}else{
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		error = VOP_OPEN(devvp, xflags, FSCRED);
		VOP_UNLOCK(devvp);
		if(error){
			rw_exit(&gefs_mntlock);
			vrele(devvp);
			return error;
		}
		error = gefs_mountfs(devvp, mp, l, args, xflags);
		rw_exit(&gefs_mntlock);
		if(error){
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			(void)VOP_CLOSE(devvp, xflags, NOCRED);
			VOP_UNLOCK(devvp);
			vrele(devvp);
			return error;
		}
	}
	return set_statvfs_info(path, UIO_USERSPACE, args->fspec, UIO_USERSPACE,
	    mp->mnt_op->vfs_name, mp, l);
}

static int
gefs_start(struct mount *mp, int flags)
{
	return 0;
}

static int
gefs_unmount(struct mount *mp, int mntflags)
{
	struct gefsmount *gm = VFSTOGEFS(mp), *o;
	struct gefsdev *gd = gm->gm_gd;
	struct vnode *devvp;
	long long r, w, f;
	int error, flags;

	flags = (mntflags & MNT_FORCE) ? FORCECLOSE : 0;
	if((error = vflush(mp, NULLVP, flags)) != 0)
		return error;
	rw_enter(&gefs_mntlock, RW_WRITER);
	error = gefs_call(gm, gefs_unloadjob, NULL);
	if(error){
		printf("gefs: unmount %s: error %d\n", gm->gm_label, error);
		if(!(mntflags & MNT_FORCE)){
			rw_exit(&gefs_mntlock);
			return error;
		}
	}
	LIST_REMOVE(gm, gm_link);
	devvp = gd->gd_devvp;
	if(!LIST_EMPTY(&gd->gd_mounts)){
		/* other labels stay mounted */
		if(spec_node_getmountedfs(devvp) == mp){
			o = LIST_FIRST(&gd->gd_mounts);
			spec_node_setmountedfs(devvp, o->gm_mp);
		}
		printf("gefs: %s unmounted\n", gm->gm_label);
		rw_exit(&gefs_mntlock);
		goto out;
	}
	LIST_REMOVE(gd, gd_link);
	rw_exit(&gefs_mntlock);

	gefs_work_stop(gd);
	gefsdevstats(gd->gd_fd, &r, &w, &f);
	printf("gefs: %s unmounted: %lld commits, %lld skipped as clean; "
	    "%llu durability requests, %llu joined another's commit; "
	    "device %lld reads, %lld writes, %lld cache flushes\n",
	    gm->gm_label, (long long)gd->gd_fs->ncommit,
	    (long long)gd->gd_fs->nskip, (unsigned long long)gd->gd_nreq,
	    (unsigned long long)gd->gd_njoin, r, w, f);
	gefs_freefs(gd->gd_fs);
	spec_node_setmountedfs(devvp, NULL);
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)VOP_CLOSE(devvp, gd->gd_openmode, NOCRED);
	vput(devvp);
	gefsdevdel(gd->gd_fd);
	kmem_free(gd, sizeof(*gd));
out:
	kmem_free(gm, sizeof(*gm));
	mp->mnt_data = NULL;
	mp->mnt_flag &= ~MNT_LOCAL;
	return 0;
}

/* a vnode for a directory entry the caller holds */
int
gefs_vnget(struct mount *mp, Dent *de, struct vnode **vpp)
{
	return vcache_get(mp, &de, sizeof(de), vpp);
}

static int
gefs_root(struct mount *mp, int lktype, struct vnode **vpp)
{
	struct gefsmount *gm = VFSTOGEFS(mp);
	int error;

	if((error = gefs_vnget(mp, gm->gm_rootde, vpp)) != 0)
		return error;
	if((error = vn_lock(*vpp, lktype)) != 0){
		vrele(*vpp);
		*vpp = NULL;
	}
	return error;
}

static int
gefs_statvfs(struct mount *mp, struct statvfs *sbp)
{
	struct gefsmount *gm = VFSTOGEFS(mp);
	uvlong total, used;
	Gefs *f;
	int i;

	f = gm->gm_fs;
	total = used = 0;
	for(i = 0; i < f->narena; i++){
		total += f->arenas[i].size;
		used += f->arenas[i].used;
	}
	sbp->f_bsize = Blksz;
	sbp->f_frsize = Blksz;
	sbp->f_iosize = Blksz;
	sbp->f_blocks = total / Blksz;
	sbp->f_bfree = (total - used) / Blksz;
	sbp->f_bavail = sbp->f_bfree;
	sbp->f_bresvd = 0;
	sbp->f_files = f->nextqid;
	sbp->f_ffree = (Qdump - 1) - f->nextqid;
	sbp->f_favail = sbp->f_ffree;
	sbp->f_fresvd = 0;
	copy_statvfs_info(sbp, mp);
	return 0;
}

static bool
gefs_sync_selector(void *cl, struct vnode *vp)
{
	KASSERT(mutex_owned(vp->v_interlock));
	return vp->v_type == VREG && (vp->v_iflag & VI_ONWORKLST) != 0 &&
	    VTOGN(vp) != NULL;
}

/*
 * Dirty pages into the tree (not to the disk: a commit does that).
 * Waiting, busy vnodes are waited for and the writes are waited for.
 */
int
gefs_putall(struct mount *mp, int wait)
{
	struct vnode_iterator *marker;
	struct vnode *vp;
	int error, firsterror = 0;

	vfs_vnode_iterator_init(mp, &marker);
	while((vp = vfs_vnode_iterator_next(marker, gefs_sync_selector, NULL)) != NULL){
		if(vn_lock(vp, LK_EXCLUSIVE | (wait ? 0 : LK_NOWAIT)) != 0){
			vrele(vp);
			continue;
		}
		rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
		error = VOP_PUTPAGES(vp, 0, 0, PGO_ALLPAGES | PGO_CLEANIT |
		    (wait ? PGO_SYNCIO : 0));
		if(error == 0 && wait && VTOGN(vp) != NULL)
			error = VTOGN(vp)->gn_werror;
		if(firsterror == 0)
			firsterror = error;
		vput(vp);
	}
	vfs_vnode_iterator_destroy(marker);
	return firsterror;
}

/*
 * The syncer calls this every syncdelay seconds (MNT_LAZY), sync(2) with
 * MNT_NOWAIT, unmount and shutdown with MNT_WAIT.  Dirty pages go into
 * the tree first, so data and metadata reach the disk in one commit; a
 * clean file system writes nothing.
 */
static int
gefs_sync(struct mount *mp, int waitfor, kauth_cred_t cred)
{
	struct gefsmount *gm = VFSTOGEFS(mp);

	struct gefsdev *gd = gm->gm_gd;
	int error;

	if(gm->gm_ronly)
		return 0;
	/* A durable sync must wait for busy vnodes and their page writes. */
	error = gefs_putall(mp, waitfor == MNT_WAIT);
	if(error != 0)
		return error;
	/*
	 * The page writes above are queued on the mutator; a commit goes
	 * after them.  The syncer's pass leaves the commit to the background
	 * policy (vfs.gefs.interval); sync(2) asks for one now and does not
	 * wait; shutdown and unmount wait for it.
	 */
	if(waitfor == MNT_WAIT)
		return gefs_commit(gm, 1, Clazy);
	if(waitfor == MNT_NOWAIT)
		return gefs_commit(gm, 0, 0);
	mutex_enter(&gd->gd_jlk);
	if(gd->gd_lazyq){
		mutex_exit(&gd->gd_jlk);
		return 0;
	}
	gd->gd_lazyq = 1;
	mutex_exit(&gd->gd_jlk);
	gefs_post(gm, gefs_agejob, NULL);
	return 0;
}

/* --- administration: GEFSIOC_CTL and GEFSIOC_STATUS --- */

struct ctlargs {
	char	*cmd;
	int	status;
	char	*text;		/* the status text, from the core's malloc */
	long	ntext;
	char	*out;		/* the check's findings, then any error */
	int	outsz;
	int	nout;
	int	error;
};

static void
gefs_ctljob(struct gefsmount *gm, void *arg)
{
	struct ctlargs *c = arg;
	char *e;
	long sz;

	USED(gm);
	if(waserror()){
		c->nout = p9capture(nil, 0);
		c->error = gefserrno(errmsg());
		if(c->out != nil)
			c->nout += snprint(c->out + c->nout, c->outsz - c->nout,
			    "%s\n", errmsg());
		return;
	}
	if(c->status){
		for(sz = 16*KiB; ; sz *= 4){
			c->text = emalloc(sz, 0);
			epochstart(0);
			if(waserror()){
				epochend(0);
				nexterror();
			}
			e = seprintstatus(c->text, c->text + sz);
			epochend(0);
			poperror();
			if(e < c->text + sz - 1 || sz >= 4*MiB)
				break;
			free(c->text);
			c->text = nil;
		}
		c->ntext = e - c->text;
	}else{
		/* the check's findings go to the caller; room is left for an error */
		p9capture(c->out, c->outsz - 1024);
		ctlcmd(c->cmd, 2);
		c->nout = p9capture(nil, 0);
	}
	poperror();
}

/*
 * An administration request on a file of mp, from VOP_IOCTL.  Snapshots
 * take the state the files have now: for snap and fork of a mounted
 * label, and for sync, the dirty pages go into the trees first.
 */
int
gefs_admin(struct gefsmount *gm, struct gefs_ctl *gc, int status, kauth_cred_t cred)
{
	struct gefsdev *gd = gm->gm_gd;
	struct gefsmount *o;
	struct ctlargs c;
	char buf[256], *w[3];
	int error, nw;
	size_t n;

	memset(&c, 0, sizeof(c));
	if(status){
		c.status = 1;
		error = gefs_call(gm, gefs_ctljob, &c);
		if(error == 0 && (error = c.error) == 0){
			n = MIN(gc->gc_len, (size_t)c.ntext);
			if(gc->gc_buf != NULL && n > 0)
				error = copyout(c.text, gc->gc_buf, n);
			gc->gc_len = c.ntext;
		}
		if(c.text != NULL){
			struct Gefs *ofs = fs;

			p9setfs(gm->gm_fs);
			free(c.text);
			p9setfs(ofs);
		}
		return error;
	}

	/*
	 * Changing the labels is changing the mount: root, or the owner of a
	 * user mount; not at securelevel 2.  (KAUTH_SYSTEM_FS_SNAPSHOT is
	 * FFS's: its listener takes any vnode for an FFS inode.)
	 */
	error = kauth_authorize_system(cred, KAUTH_SYSTEM_MOUNT,
	    KAUTH_REQ_SYSTEM_MOUNT_UPDATE, gm->gm_mp,
	    KAUTH_ARG(gm->gm_mp->mnt_flag), NULL);
	if(error)
		return error;
	gc->gc_cmd[sizeof(gc->gc_cmd)-1] = 0;
	strlcpy(buf, gc->gc_cmd, sizeof(buf));
	nw = ctltokenize(buf, w, nelem(w));
	rw_enter(&gefs_mntlock, RW_READER);
	LIST_FOREACH(o, &gd->gd_mounts, gm_link){
		if(o->gm_ronly)
			continue;
		if((nw == 1 && strcmp(w[0], "sync") == 0)
		|| (nw == 3 && (strcmp(w[0], "snap") == 0 || strcmp(w[0], "fork") == 0)
		    && strcmp(w[1], o->gm_label) == 0))
			gefs_putall(o->gm_mp, 1);
	}
	rw_exit(&gefs_mntlock);

	c.cmd = gc->gc_cmd;
	c.outsz = 16*1024;
	c.out = kmem_zalloc(c.outsz, KM_SLEEP);
	error = gefs_call(gm, gefs_ctljob, &c);
	if(error == 0)
		error = c.error;
	/* with the NUL: when the ioctl fails, gc_len does not come back */
	if(gc->gc_buf != NULL && gc->gc_len > 0 && c.nout > 0)
		(void)copyout(c.out, gc->gc_buf, MIN(gc->gc_len, (size_t)c.nout + 1));
	gc->gc_len = c.nout;
	kmem_free(c.out, c.outsz);
	return error;
}

static int
gefs_vget(struct mount *mp, ino_t ino, int lktype, struct vnode **vpp)
{
	/* gefs keeps no index from qid to entry */
	return EOPNOTSUPP;
}

/* vcache: a vnode for the Dent in the key; the vnode keeps a reference */
static int
gefs_loadvnode(struct mount *mp, struct vnode *vp, const void *key,
    size_t key_len, const void **new_key)
{
	struct gefsmount *gm = VFSTOGEFS(mp);
	struct gefsnode *gn;
	Dent *de;

	KASSERT(key_len == sizeof(de));
	memcpy(&de, key, key_len);
	if(de->gone)
		return ENOENT;

	gn = kmem_zalloc(sizeof(*gn), KM_SLEEP);
	aincl(&de->ref, 1);
	gn->gn_de = de;
	gn->gn_vp = vp;
	gn->gn_gmp = gm;
	mutex_init(&gn->gn_scanlk, MUTEX_DEFAULT, IPL_NONE);
	de->vdata = gn;

	vp->v_tag = VT_NON;
	vp->v_data = gn;
	switch(gefsxtype(&de->Xdir)){
	case Gdir:	vp->v_type = VDIR;	break;
	case Glnk:	vp->v_type = VLNK;	break;
	case Gchr:	vp->v_type = VCHR;	break;
	case Gblk:	vp->v_type = VBLK;	break;
	case Gfifo:	vp->v_type = VFIFO;	break;
	case Gsock:	vp->v_type = VSOCK;	break;
	default:	vp->v_type = VREG;	break;
	}
	switch(vp->v_type){
	case VCHR:
	case VBLK:
		vp->v_op = gefs_specop_p;
		spec_node_init(vp, (dev_t)de->rdev);
		break;
	case VFIFO:
		vp->v_op = gefs_fifoop_p;
		break;
	default:
		vp->v_op = gefs_vnodeop_p;
		break;
	}
	if(de == gm->gm_rootde)
		vp->v_vflag |= VV_ROOT;
	genfs_node_init(vp, &gefs_genfsops);
	uvm_vnp_setsize(vp, (vp->v_type == VDIR) ? 0 : de->length);
	*new_key = &gn->gn_de;
	return 0;
}

/*
 * Boot environments: a kernel the boot loader read from a label
 * ("boot @pre-upgrade/netbsd") runs with that label as its root.  A fork
 * is mounted read-write as usual; a snapshot stays read-only.
 */
static void
gefs_rootlabel(char *label, size_t n)
{
	const char *bk, *e;

	label[0] = 0;
	if((bk = get_booted_kernel()) == NULL)
		return;
	while(*bk == '/')
		bk++;
	if(*bk++ != '@' || (e = strchr(bk, '/')) == NULL || e == bk ||
	    (size_t)(e - bk) >= n)
		return;
	memcpy(label, bk, e - bk);
	label[e - bk] = 0;
	printf("gefs: root label %s, from the boot loader\n", label);
}

/* the root file system: the kernel opened rootvp for reading */
static int
gefs_mountroot(void)
{
	struct gefs_args args;
	struct mount *mp;
	struct lwp *l = curlwp;
	int error;

	if(device_class(root_device) != DV_DISK)
		return ENODEV;
	if((error = vfs_rootmountalloc(MOUNT_GEFS, "root_device", &mp)) != 0){
		vrele(rootvp);
		return error;
	}
	memset(&args, 0, sizeof(args));
	gefs_rootlabel(args.snap, sizeof(args.snap));
	mp->mnt_flag |= MNT_FORCE;
	rw_enter(&gefs_mntlock, RW_WRITER);
	error = gefs_mountfs(rootvp, mp, l, &args, FREAD);
	rw_exit(&gefs_mntlock);
	if(error){
		vfs_unbusy(mp);
		vfs_rele(mp);
		return error;
	}
	mp->mnt_flag &= ~MNT_FORCE;
	mountlist_append(mp);
	(void)gefs_statvfs(mp, &mp->mnt_stat);
	vfs_unbusy(mp);
	/* for the clock, as FFS gives its superblock time: the root's mtime */
	setrootfstime((time_t)(VFSTOGEFS(mp)->gm_rootde->mtime / Nsec));
	return 0;
}

static int
gefs_fhtovp(struct mount *mp, struct fid *fhp, int lktype, struct vnode **vpp)
{
	return EOPNOTSUPP;
}

static int
gefs_vptofh(struct vnode *vp, struct fid *fhp, size_t *fh_size)
{
	return EOPNOTSUPP;
}

static void
gefs_init(void)
{
	rw_init(&gefs_mntlock);
	gefs_sysctl_init();
#ifndef _MODULE
	/* built in: no module command to set up the compatibility layer */
	if(p9kinit() != 0)
		panic("gefs: p9kinit");
#endif
}

static void
gefs_done(void)
{
	sysctl_teardown(&gefs_sysctl_log);
	rw_destroy(&gefs_mntlock);
}

static int
gefs_snapshot(struct mount *mp, struct vnode *vp, struct timespec *ctime)
{
	return EOPNOTSUPP;
}

static const struct vnodeopv_desc * const gefs_vnodeopv_descs[] = {
	&gefs_vnodeop_opv_desc,
	&gefs_specop_opv_desc,
	&gefs_fifoop_opv_desc,
	NULL,
};

struct vfsops gefs_vfsops = {
	.vfs_name = MOUNT_GEFS,
	.vfs_min_mount_data = sizeof(struct gefs_args),
	.vfs_mount = gefs_mount,
	.vfs_start = gefs_start,
	.vfs_unmount = gefs_unmount,
	.vfs_root = gefs_root,
	.vfs_quotactl = (void *)eopnotsupp,
	.vfs_statvfs = gefs_statvfs,
	.vfs_sync = gefs_sync,
	.vfs_vget = gefs_vget,
	.vfs_loadvnode = gefs_loadvnode,
	.vfs_fhtovp = gefs_fhtovp,
	.vfs_vptofh = gefs_vptofh,
	.vfs_mountroot = gefs_mountroot,
	.vfs_init = gefs_init,
	.vfs_done = gefs_done,
	.vfs_snapshot = gefs_snapshot,
	.vfs_extattrctl = vfs_stdextattrctl,
	.vfs_suspendctl = genfs_suspendctl,
	.vfs_renamelock_enter = genfs_renamelock_enter,
	.vfs_renamelock_exit = genfs_renamelock_exit,
	.vfs_fsync = (void *)eopnotsupp,
	.vfs_opv_descs = gefs_vnodeopv_descs
};

static int
gefs_modcmd(modcmd_t cmd, void *arg)
{
	int error;

	switch(cmd){
	case MODULE_CMD_INIT:
#ifdef _MODULE
		if((error = p9kinit()) != 0)
			return error;
#endif
		if((error = vfs_attach(&gefs_vfsops)) != 0){
#ifdef _MODULE
			p9kfini();
#endif
		}
		return error;
	case MODULE_CMD_FINI:
		if((error = vfs_detach(&gefs_vfsops)) != 0)
			return error;
#ifdef _MODULE
		p9kfini();
#endif
		return 0;
	default:
		return ENOTTY;
	}
}
