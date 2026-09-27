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
	Blk *b, *nb;
	Arena *a;
	int i;

	p9setfs(f);
	for(i = 0; i < 3; i++)
		epochclean();
	for(i = 0; i < f->cmax; i++)
		free(f->blkall[i]);
	free(f->blkall);
	USED(b, nb);
	for(i = 0; i < f->narena; i++){
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

/* on the mutator: load the file system and the label */
static void
gefs_loadjob(struct gefsmount *gm, void *arg)
{
	int i;

	USED(arg);
	gefsdevpend(gm->gm_fd);
	loadfs("gefs");
	for(i = 0; i < fs->narena; i++)
		fs->arenas[i].sync = &fs->syncq[i%fs->nsyncers];
	gm->gm_mnt = getmount(gm->gm_label);
	if(!gm->gm_ronly && !(gm->gm_mnt->flag & Lmut)){
		clunkmount(gm->gm_mnt);
		gm->gm_mnt = nil;
		gefserr(Erdonly);
	}
	gm->gm_rootde = gefsroot(gm->gm_mnt);
	if(!gm->gm_ronly)
		gefsorphans(gm->gm_mnt);
}

/* on the mutator: everything to the disk, and let go of the label */
static void
gefs_unloadjob(struct gefsmount *gm, void *arg)
{
	USED(arg);
	if(!gm->gm_ronly && !agetl(&fs->rdonly))
		drainsyncq();
	clunkdent(gm->gm_mnt, gm->gm_rootde);
	clunkmount(gm->gm_mnt);
}

static void
gefs_commitjob(struct gefsmount *gm, void *arg)
{
	int flags = (int)(intptr_t)arg;

	gm->gm_lazyq = 0;
	commit(flags);
}

/* commit on the mutator, waiting for it, or not (MNT_LAZY) */
int
gefs_commit(struct gefsmount *gm, int wait, int flags)
{
	if(wait)
		return gefs_call(gm, gefs_commitjob, (void*)(intptr_t)flags);
	if(!gm->gm_lazyq){
		gm->gm_lazyq = 1;
		gefs_post(gm, gefs_commitjob, (void*)(intptr_t)flags);
	}
	return 0;
}

static int
gefs_mountfs(struct vnode *devvp, struct mount *mp, struct lwp *l,
    struct gefs_args *args)
{
	struct gefsmount *gm;
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

	gm = kmem_zalloc(sizeof(*gm), KM_SLEEP);
	gm->gm_mp = mp;
	gm->gm_devvp = devvp;
	gm->gm_ronly = (mp->mnt_flag & MNT_RDONLY) != 0;
	strlcpy(gm->gm_label, args->snap[0] ? args->snap : "main",
	    sizeof(gm->gm_label));
	gm->gm_fd = gefsdevadd(devvp, (long long)psize * secsize);
	if(gm->gm_fd < 0){
		kmem_free(gm, sizeof(*gm));
		return EMFILE;
	}
	csz = (vlong)(args->cachemb > 0 ? args->cachemb : gefs_cachemb) * MiB;
	gm->gm_fs = gefs_newfs(csz, gm->gm_ronly);
	p9setfs(nil);
	if((error = gefs_work_start(gm)) == 0){
		error = gefs_call(gm, gefs_loadjob, NULL);
		if(error)
			gefs_work_stop(gm);
	}
	if(error){
		printf("gefs: mount %s: error %d\n", gm->gm_label, error);
		gefs_freefs(gm->gm_fs);
		gefsdevdel(gm->gm_fd);
		kmem_free(gm, sizeof(*gm));
		return error;
	}

	mp->mnt_data = gm;
	mp->mnt_stat.f_fsidx.__fsid_val[0] = (long)devvp->v_rdev;
	mp->mnt_stat.f_fsidx.__fsid_val[1] = makefstype(MOUNT_GEFS);
	mp->mnt_stat.f_fsid = mp->mnt_stat.f_fsidx.__fsid_val[0];
	mp->mnt_stat.f_namemax = Maxname;
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_iflag |= IMNT_MPSAFE;
	mp->mnt_fs_bshift = Lgblk;
	mp->mnt_dev_bshift = DEV_BSHIFT;
	spec_node_setmountedfs(devvp, mp);
	return 0;
}

static int
gefs_mount(struct mount *mp, const char *path, void *data, size_t *data_len)
{
	struct lwp *l = curlwp;
	struct gefs_args *args = data;
	struct gefsmount *gm;
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
		/* nothing changes on update yet; ro <-> rw needs a remount */
		gm = VFSTOGEFS(mp);
		if(gm == NULL)
			return EINVAL;
		if(gm->gm_ronly != ((mp->mnt_flag & MNT_RDONLY) != 0))
			return EOPNOTSUPP;
		return 0;
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
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_OPEN(devvp, xflags, FSCRED);
	VOP_UNLOCK(devvp);
	if(error){
		vrele(devvp);
		return error;
	}
	error = gefs_mountfs(devvp, mp, l, args);
	if(error){
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		(void)VOP_CLOSE(devvp, xflags, NOCRED);
		VOP_UNLOCK(devvp);
		vrele(devvp);
		return error;
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
	struct gefsmount *gm = VFSTOGEFS(mp);
	struct vnode *devvp;
	int error, flags;

	flags = (mntflags & MNT_FORCE) ? FORCECLOSE : 0;
	if((error = vflush(mp, NULLVP, flags)) != 0)
		return error;
	error = gefs_call(gm, gefs_unloadjob, NULL);
	if(error){
		printf("gefs: unmount %s: error %d\n", gm->gm_label, error);
		if(!(mntflags & MNT_FORCE))
			return error;
	}
	gefs_work_stop(gm);
	{
		long long r, w, f;

		gefsdevstats(gm->gm_fd, &r, &w, &f);
		printf("gefs: %s unmounted: %lld commits, %lld skipped as clean; "
		    "device %lld reads, %lld writes, %lld cache flushes\n",
		    gm->gm_label, (long long)gm->gm_fs->ncommit,
		    (long long)gm->gm_fs->nskip, r, w, f);
	}
	gefs_freefs(gm->gm_fs);

	devvp = gm->gm_devvp;
	spec_node_setmountedfs(devvp, NULL);
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)VOP_CLOSE(devvp, gm->gm_ronly ? FREAD : FREAD|FWRITE, NOCRED);
	vput(devvp);
	gefsdevdel(gm->gm_fd);
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
 * The syncer calls this every syncdelay seconds (MNT_LAZY), sync(2) with
 * MNT_WAIT.  Dirty pages go into the tree first, so data and metadata
 * reach the disk in one commit; a clean file system writes nothing.
 */
static int
gefs_sync(struct mount *mp, int waitfor, kauth_cred_t cred)
{
	struct gefsmount *gm = VFSTOGEFS(mp);
	struct vnode_iterator *marker;
	struct vnode *vp;
	int error, flags;

	if(gm->gm_ronly)
		return 0;
	flags = (waitfor == MNT_WAIT) ? FSYNC_WAIT : FSYNC_LAZY;
	vfs_vnode_iterator_init(mp, &marker);
	while((vp = vfs_vnode_iterator_next(marker, gefs_sync_selector, NULL)) != NULL){
		if(vn_lock(vp, LK_EXCLUSIVE | LK_NOWAIT) != 0){
			vrele(vp);
			continue;
		}
		/* data only: the commit follows once for everything */
		rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
		(void)VOP_PUTPAGES(vp, 0, 0, PGO_ALLPAGES | PGO_CLEANIT |
		    (waitfor == MNT_WAIT ? PGO_SYNCIO : 0));
		vput(vp);
	}
	vfs_vnode_iterator_destroy(marker);
	USED(flags);

	/*
	 * The page writes above are queued on the mutator; the commit goes
	 * after them.  The syncer does not wait for it.
	 */
	if(waitfor == MNT_WAIT)
		error = gefs_commit(gm, 1, Clazy);
	else
		error = gefs_commit(gm, 0, 0);
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
}

static void
gefs_done(void)
{
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
		if((error = p9kinit()) != 0)
			return error;
		if((error = vfs_attach(&gefs_vfsops)) != 0)
			p9kfini();
		return error;
	case MODULE_CMD_FINI:
		if((error = vfs_detach(&gefs_vfsops)) != 0)
			return error;
		p9kfini();
		return 0;
	default:
		return ENOTTY;
	}
}
