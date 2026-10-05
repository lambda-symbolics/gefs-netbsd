/*
 * GEFS for NetBSD: vnode operations.
 *
 * File data lives in UBC.  VOP_BMAP maps a file block to itself and
 * VOP_STRATEGY does the I/O by file offset through the core (as NFS
 * does), so genfs's getpages/putpages, mmap and exec work unchanged.
 * Writes only dirty pages; the data reaches the tree when pages are
 * put (syncer, fsync, memory pressure) and the disk at the next commit.
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
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/lockf.h>
#include <sys/uio.h>
#include <sys/fstrans.h>
#include <miscfs/genfs/genfs.h>
#include <miscfs/specfs/specdev.h>
#include <miscfs/fifofs/fifo.h>
#include <uvm/uvm_extern.h>
#include <uvm/uvm_page.h>
#include <uvm/uvm_page_array.h>

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"
#include "gefs.h"

#define GMP(vp)		(VFSTOGEFS((vp)->v_mount))
#define GMNT(vp)	(GMP(vp)->gm_mnt)

static enum vtype
gtype2v(int t)
{
	switch(t){
	case Gdir:	return VDIR;
	case Glnk:	return VLNK;
	case Gchr:	return VCHR;
	case Gblk:	return VBLK;
	case Gfifo:	return VFIFO;
	case Gsock:	return VSOCK;
	}
	return VREG;
}

static int
vtype2g(enum vtype t)
{
	switch(t){
	case VDIR:	return Gdir;
	case VLNK:	return Glnk;
	case VCHR:	return Gchr;
	case VBLK:	return Gblk;
	case VFIFO:	return Gfifo;
	case VSOCK:	return Gsock;
	case VREG:	return Greg;
	default:	return Gnon;
	}
}

/* BSD file flags gefs can keep: append-only and nodump */
static u_long
g2flags(ulong f)
{
	u_long r;

	r = 0;
	if(f & Gappend)
		r |= UF_APPEND;
	if(f & Gnodump)
		r |= UF_NODUMP;
	return r;
}

/*
 * Changes run on the mutator thread (gefs_work.c); these carry their
 * arguments there.
 */
struct mkargs {
	Dent	*dir;
	char	*name;
	int	n;
	Gattr	*a;
	char	*target;
	int	ntarget;
	Dent	*de;
};

static void
gefs_mkjob(struct gefsmount *gm, void *v)
{
	struct mkargs *m = v;

	m->de = gefscreate(gm->gm_mnt, m->dir, m->name, m->n, m->a,
	    m->target, m->ntarget);
}

struct rmargs {
	Dent	*dir;
	char	*name;
	int	n;
	Dent	*de;
};

static void
gefs_rmjob(struct gefsmount *gm, void *v)
{
	struct rmargs *r = v;

	gefsremove(gm->gm_mnt, r->dir, r->name, r->n, r->de);
}

struct mvargs {
	Dent	*fdir;
	char	*fname;
	int	nf;
	Dent	*de;
	Dent	*tdir;
	char	*tname;
	int	n;
	Dent	*tde;
};

static void
gefs_mvjob(struct gefsmount *gm, void *v)
{
	struct mvargs *m = v;

	gefsrename(gm->gm_mnt, m->fdir, m->fname, m->nf, m->de, m->tdir,
	    m->tname, m->n, m->tde);
}

struct saargs {
	Dent	*de;
	Gattr	*a;
	int	mask;
	int	uid;
};

static void
gefs_sajob(struct gefsmount *gm, void *v)
{
	struct saargs *a = v;
	vlong olen = a->de->length;

	gefssetattr(gm->gm_mnt, a->de, a->a, a->mask, a->uid);
	if(gefs_trace && (a->mask & Asize))
		printf("gefs: truncated qid %llx in the tree: length %lld -> %lld\n",
		    (unsigned long long)a->de->qid.path, (long long)olen,
		    (long long)a->de->length);
}

struct lnargs {
	Dent	*dir;
	char	*name;
	int	n;
	Dent	*de;
};

static void
gefs_lnjob(struct gefsmount *gm, void *v)
{
	struct lnargs *l = v;

	gefslink(gm->gm_mnt, l->dir, l->name, l->n, l->de);
}

/*
 * relatime: a read sets atime when it is not after mtime, or is a day
 * old.  Lazily: the update rides along with the next commit something
 * else causes (or unmount), so reading never wakes the disk.
 */
static void
gefs_atimejob(struct gefsmount *gm, void *v)
{
	struct gefsnode *gn = v;
	Dent *de = gn->gn_de;
	Gattr a;

	gn->gn_atimeq = 0;
	if(!waserror()){
		if(!de->gone && !de->orphan){
			memset(&a, 0, sizeof(a));
			a.atime = nsec();
			gefssetattr(gm->gm_mnt, de, &a, Aatime, de->muid);
		}
		poperror();
	}
	clunkdent(gm->gm_mnt, de);
	/*
	 * Not vrele: the last reference would reclaim here, and reclaim
	 * flushes pages through this very thread.
	 */
	vrele_async(gn->gn_vp);
}

static void
gefs_readatime(struct vnode *vp)
{
	struct gefsnode *gn = VTOGN(vp);
	Dent *de = gn->gn_de;
	vlong now;

	if(vp->v_mount->mnt_flag & (MNT_NOATIME | MNT_RDONLY))
		return;
	now = nsec();
	if(de->atime > de->mtime && now - de->atime < 24LL*3600*Nsec)
		return;
	if(gn->gn_atimeq)
		return;
	gn->gn_atimeq = 1;
	vref(vp);
	aincl(&de->ref, 1);
	gefs_post(GMP(vp), gefs_atimejob, gn);
}

/* posted, not waited for: a removed file's data goes in the background */
static void
gefs_reljob(struct gefsmount *gm, void *v)
{
	Dent *de = v;

	if(waserror()){
		clunkdent(gm->gm_mnt, de);
		nexterror();
	}
	gefsrelease(gm->gm_mnt, de);
	poperror();
	clunkdent(gm->gm_mnt, de);
}

/* --- lookup --- */

static int
gefs_lookup(void *v)
{
	struct vop_lookup_v2_args *ap = v;
	struct vnode *dvp = ap->a_dvp, **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	const bool lastcn = (cnp->cn_flags & ISLASTCN) != 0;
	struct gefsmount *gm = GMP(dvp);
	Dent *dde, *volatile de;
	Gattr da, a;
	int error, found;

	*vpp = NULL;
	dde = VTODE(dvp);
	de = nil;
	error = VOP_ACCESS(dvp, VEXEC, cnp->cn_cred);
	if(error)
		return error;
	if(lastcn && (dvp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME))
		return EROFS;
	if(cache_lookup(dvp, cnp->cn_nameptr, cnp->cn_namelen,
	    cnp->cn_nameiop, cnp->cn_flags, NULL, vpp)){
		return *vpp == NULLVP ? ENOENT : 0;
	}
	if(dde->gone)
		return ENOENT;
	if(cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.'){
		if(lastcn && cnp->cn_nameiop == RENAME)
			return EISDIR;
		vref(dvp);
		*vpp = dvp;
		return 0;
	}
	if((cnp->cn_flags & ISDOTDOT) && lastcn && cnp->cn_nameiop == RENAME)
		return EINVAL;

	error = 0;
	{
		GEFS_BEGIN(gm, error, out);
		if(cnp->cn_namelen > Maxname)
			gefserr(Elength);
		de = gefslookup(gm->gm_mnt, dde, cnp->cn_nameptr, cnp->cn_namelen);
		GEFS_END();
	}
out:
	if(error)
		return error;
	found = de != nil;
	if(!found){
		if(lastcn && (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)){
			error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred);
			return error ? error : EJUSTRETURN;
		}
		if(cnp->cn_nameiop != CREATE)
			cache_enter(dvp, NULL, cnp->cn_nameptr, cnp->cn_namelen,
			    cnp->cn_flags);
		return ENOENT;
	}
	gefsgetattr(de, &a);
	if(!lastcn && a.type != Gdir && a.type != Glnk){
		error = ENOTDIR;
		goto done;
	}
	if(lastcn && (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME)){
		if((error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred)) != 0)
			goto done;
		gefsgetattr(dde, &da);
		if(da.perm & S_ISTXT){
			error = kauth_authorize_vnode(cnp->cn_cred,
			    KAUTH_VNODE_DELETE, NULL, dvp,
			    genfs_can_sticky(dvp, cnp->cn_cred, da.uid, a.uid));
			if(error){
				error = EPERM;
				goto done;
			}
		}
	}
	error = gefs_vnget(dvp->v_mount, de, vpp);
	if(error == 0 && cnp->cn_nameiop != CREATE)
		cache_enter(dvp, *vpp, cnp->cn_nameptr, cnp->cn_namelen,
		    cnp->cn_flags);
done:
	{
		int e2 = 0;
		GEFS_BEGIN(gm, e2, out2);
		clunkdent(gm->gm_mnt, de);
		GEFS_END();
	out2:	;
	}
	return error;
}

/* --- creating --- */

static int
gefs_mkobj(struct vnode *dvp, struct vnode **vpp, struct componentname *cnp,
    struct vattr *vap, char *target)
{
	struct gefsmount *gm = GMP(dvp);
	Dent *de;
	Gattr a, da;
	int error;

	*vpp = NULL;
	if(dvp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;
	memset(&a, 0, sizeof(a));
	a.type = vtype2g(vap->va_type);
	if(a.type == Gnon)
		return EINVAL;
	a.perm = vap->va_mode & ALLPERMS;
	a.uid = kauth_cred_geteuid(cnp->cn_cred);
	gefsgetattr(VTODE(dvp), &da);
	a.gid = da.gid;		/* BSD: the directory's group */
	if(a.type == Gchr || a.type == Gblk)
		a.rdev = vap->va_rdev;
	if(vap->va_flags != VNOVAL && (vap->va_flags & UF_NODUMP))
		a.flags |= Gnodump;
	{
		struct mkargs m = {
			.dir = VTODE(dvp), .name = cnp->cn_nameptr,
			.n = cnp->cn_namelen, .a = &a, .target = target,
			.ntarget = target ? strlen(target) : 0,
		};
		error = gefs_call(gm, gefs_mkjob, &m);
		de = m.de;
	}
	if(error)
		return error;
	error = gefs_vnget(dvp->v_mount, de, vpp);
	{
		int e2 = 0;
		GEFS_BEGIN(gm, e2, out2);
		clunkdent(gm->gm_mnt, de);
		GEFS_END();
	out2:	;
	}
	if(error == 0)
		cache_enter(dvp, *vpp, cnp->cn_nameptr, cnp->cn_namelen,
		    cnp->cn_flags);
	return error;
}

static int
gefs_create(void *v)
{
	struct vop_create_v3_args *ap = v;

	return gefs_mkobj(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, NULL);
}

static int
gefs_mknod(void *v)
{
	struct vop_mknod_v3_args *ap = v;
	enum vtype vt = ap->a_vap->va_type;

	if(vt != VBLK && vt != VCHR && vt != VFIFO && vt != VSOCK){
		*ap->a_vpp = NULL;
		return EINVAL;
	}
	return gefs_mkobj(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, NULL);
}

static int
gefs_mkdir(void *v)
{
	struct vop_mkdir_v3_args *ap = v;

	return gefs_mkobj(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, NULL);
}

static int
gefs_symlink(void *v)
{
	struct vop_symlink_v3_args *ap = v;

	if(strlen(ap->a_target) >= MAXPATHLEN){
		*ap->a_vpp = NULL;
		return ENAMETOOLONG;
	}
	return gefs_mkobj(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap,
	    ap->a_target);
}

/* --- open, close, access, attributes --- */

static int
gefs_open(void *v)
{
	struct vop_open_args *ap = v;
	struct vnode *vp = ap->a_vp;
	Gattr a;

	gefsgetattr(VTODE(vp), &a);
	if((a.flags & Gappend) && (ap->a_mode & (FWRITE | O_APPEND)) == FWRITE)
		return EPERM;
	if(VTODE(vp)->gone || VTODE(vp)->orphan)
		return ENOENT;
	return 0;
}

/* a page write that failed since the last report (ENOSPC, EIO) */
static int
gefs_werror(struct vnode *vp)
{
	struct gefsnode *gn = VTOGN(vp);
	int e;

	if(gn == NULL || (e = gn->gn_werror) == 0)
		return 0;
	gn->gn_werror = 0;
	return e;
}

static int
gefs_close(void *v)
{
	struct vop_close_args /* {
		struct vnode *a_vp;
		int a_fflag;
		kauth_cred_t a_cred;
	} */ *ap = v;

	return (ap->a_fflag & FWRITE) ? gefs_werror(ap->a_vp) : 0;
}

static int
gefs_access(void *v)
{
	struct vop_access_args *ap = v;
	struct vnode *vp = ap->a_vp;
	accmode_t accmode = ap->a_accmode;
	bool writing = (accmode & VWRITE) != 0;
	Gattr a;

	switch(vp->v_type){
	case VDIR:
	case VLNK:
	case VREG:
		if(writing && (vp->v_mount->mnt_flag & MNT_RDONLY))
			return EROFS;
		break;
	case VBLK:
	case VCHR:
	case VSOCK:
	case VFIFO:
		break;
	default:
		return EINVAL;
	}
	gefsgetattr(VTODE(vp), &a);
	return kauth_authorize_vnode(ap->a_cred,
	    KAUTH_ACCESS_ACTION(accmode, vp->v_type, a.perm), vp, NULL,
	    genfs_can_access(vp, ap->a_cred, a.uid, a.gid, a.perm, NULL,
	    accmode));
}

static void
ns2ts(vlong ns, struct timespec *ts)
{
	ts->tv_sec = ns / Nsec;
	ts->tv_nsec = ns % Nsec;
}

static int
gefs_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	Gattr a;

	gefsgetattr(VTODE(vp), &a);
	vattr_null(vap);
	vap->va_type = vp->v_type;
	vap->va_mode = a.perm;
	vap->va_nlink = a.nlink;
	vap->va_uid = a.uid;
	vap->va_gid = a.gid;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsidx.__fsid_val[0];
	vap->va_fileid = a.qid + 1;
	vap->va_size = (vp->v_type == VREG) ? vp->v_size : a.size;
	vap->va_blocksize = Blksz;
	ns2ts(a.atime, &vap->va_atime);
	ns2ts(a.mtime, &vap->va_mtime);
	ns2ts(a.mtime, &vap->va_ctime);		/* gefs keeps no ctime */
	ns2ts(a.mtime, &vap->va_birthtime);
	vap->va_gen = a.vers;
	vap->va_flags = g2flags(a.flags);
	vap->va_rdev = (vp->v_type == VCHR || vp->v_type == VBLK) ? a.rdev : 0;
	vap->va_bytes = (vap->va_size + Blksz-1) & ~(Blksz-1);
	vap->va_filerev = a.vers;
	vap->va_vaflags = 0;
	return 0;
}

/* is a page of vp cached at or after off? */
static bool
gefs_pagespast(struct vnode *vp, voff_t off)
{
	struct uvm_object *uobj = &vp->v_uobj;
	struct uvm_page_array a;
	struct vm_page *pg;

	rw_enter(uobj->vmobjlock, RW_READER);
	uvm_page_array_init(&a, uobj, 0);
	pg = uvm_page_array_fill_and_peek(&a, off, 0);
	uvm_page_array_fini(&a);
	rw_exit(uobj->vmobjlock);
	return pg != NULL;
}

/*
 * After uvm_vnp_setsize shrank vp: no page past the end may stay.  A
 * synchronous putpages skips busy pages ("other's activities"), and ours
 * stay busy while their I/O waits for the mutator or a reader; such a
 * page, filled or written before the truncation, would bring old data
 * back when the file grows.  Wait for the I/O, and drop them again.
 */
static void
gefs_droppast(struct vnode *vp, voff_t size)
{
	voff_t off = round_page(size);
	int tries;

	for(tries = 0; gefs_pagespast(vp, off); tries++){
		mutex_enter(vp->v_interlock);
		while(vp->v_numoutput > 0)
			cv_wait(&vp->v_cv, vp->v_interlock);
		mutex_exit(vp->v_interlock);
		if(tries > 0)
			kpause("gefstrnc", false, 1, NULL);	/* a read in flight */
		rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
		(void)VOP_PUTPAGES(vp, off, 0, PGO_FREE | PGO_SYNCIO);
		if(tries == 1000){
			printf("gefs: pages past the end of a truncated file stay\n");
			break;
		}
	}
}

static int
gefs_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	kauth_cred_t cred = ap->a_cred;
	struct gefsmount *gm = GMP(vp);
	Dent *de = VTODE(vp);
	Gattr a, n;
	int mask, error, uid, locked;

	if(vap->va_type != VNON || vap->va_nlink != VNOVAL ||
	    vap->va_fsid != VNOVAL || vap->va_fileid != VNOVAL ||
	    vap->va_blocksize != VNOVAL || vap->va_rdev != VNOVAL ||
	    vap->va_bytes != VNOVAL || vap->va_gen != VNOVAL)
		return EINVAL;
	gefsgetattr(de, &a);
	locked = 0;
	memset(&n, 0, sizeof(n));
	mask = 0;
	uid = kauth_cred_geteuid(cred);
	/*
	 * A size for a device or fifo is ignored, as on FFS: open(2) with
	 * O_TRUNC sends one, and /dev/null must open on a read-only root.
	 */
	if(vap->va_size != VNOVAL && vp->v_type != VREG && vp->v_type != VDIR)
		vap->va_size = VNOVAL;
	if((vp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (vap->va_flags != VNOVAL || vap->va_uid != (uid_t)VNOVAL ||
	    vap->va_gid != (gid_t)VNOVAL || vap->va_mode != (mode_t)VNOVAL ||
	    vap->va_size != VNOVAL || vap->va_atime.tv_sec != VNOVAL ||
	    vap->va_mtime.tv_sec != VNOVAL))
		return EROFS;

	if(vap->va_flags != VNOVAL){
		bool sysflags = (vap->va_flags & SF_SETTABLE) != 0 ||
		    (g2flags(a.flags) & SF_SETTABLE) != 0;
		if(vap->va_flags & ~(UF_APPEND | UF_NODUMP | SF_APPEND))
			return EOPNOTSUPP;
		error = kauth_authorize_vnode(cred, sysflags ?
		    KAUTH_VNODE_WRITE_SYSFLAGS : KAUTH_VNODE_WRITE_FLAGS, vp,
		    NULL, genfs_can_chflags(vp, cred, a.uid, sysflags));
		if(error)
			return error;
		n.flags = 0;
		if(vap->va_flags & (UF_APPEND | SF_APPEND))
			n.flags |= Gappend;
		if(vap->va_flags & UF_NODUMP)
			n.flags |= Gnodump;
		mask |= Aflags;
	}
	if(a.flags & Gappend){
		if(vap->va_size != VNOVAL || vap->va_mode != (mode_t)VNOVAL ||
		    vap->va_uid != (uid_t)VNOVAL || vap->va_gid != (gid_t)VNOVAL)
			return EPERM;
	}
	if(vap->va_uid != (uid_t)VNOVAL || vap->va_gid != (gid_t)VNOVAL){
		uid_t nuid = (vap->va_uid != (uid_t)VNOVAL) ? vap->va_uid : a.uid;
		gid_t ngid = (vap->va_gid != (gid_t)VNOVAL) ? vap->va_gid : a.gid;
		error = kauth_authorize_vnode(cred, KAUTH_VNODE_CHANGE_OWNERSHIP,
		    vp, NULL, genfs_can_chown(vp, cred, a.uid, a.gid, nuid, ngid));
		if(error)
			return error;
		n.uid = nuid;
		n.gid = ngid;
		mask |= Auid | Agid;
	}
	if(vap->va_mode != (mode_t)VNOVAL){
		error = kauth_authorize_vnode(cred, KAUTH_VNODE_WRITE_SECURITY,
		    vp, NULL, genfs_can_chmod(vp, cred, a.uid, a.gid,
		    vap->va_mode));
		if(error)
			return error;
		n.perm = vap->va_mode & ALLPERMS;
		mask |= Aperm;
	}
	if(vap->va_atime.tv_sec != VNOVAL || vap->va_mtime.tv_sec != VNOVAL){
		error = kauth_authorize_vnode(cred, KAUTH_VNODE_WRITE_TIMES, vp,
		    NULL, genfs_can_chtimes(vp, cred, a.uid, vap->va_vaflags));
		if(error)
			return error;
		if(vap->va_atime.tv_sec != VNOVAL){
			n.atime = (vlong)vap->va_atime.tv_sec * Nsec + vap->va_atime.tv_nsec;
			mask |= Aatime;
		}
		if(vap->va_mtime.tv_sec != VNOVAL){
			n.mtime = (vlong)vap->va_mtime.tv_sec * Nsec + vap->va_mtime.tv_nsec;
			mask |= Amtime;
		}
	}
	if(vap->va_size != VNOVAL){
		switch(vp->v_type){
		case VDIR:
			return EISDIR;
		case VREG:
			break;
		default:
			/* devices and fifos: ignored, as FFS does */
			goto attrs;
		}
		if(vap->va_size < vp->v_size){
			/* zero the rest of the last page, drop the pages after */
			off_t rem = round_page(vap->va_size) - vap->va_size;
			if(rem > 0)
				ubc_zerorange(&vp->v_uobj, vap->va_size, rem,
				    UBC_VNODE_FLAGS(vp));
		}
		if(gefs_trace)
			printf("gefs: truncate qid %llx %lld -> %lld\n",
			    (unsigned long long)de->qid.path,
			    (long long)vp->v_size, (long long)vap->va_size);
		/*
		 * As FFS: no getpages from here until the tree is cut too.
		 * Read-ahead could otherwise bring in pages past the new end
		 * (between uvm_vnp_setsize dropping them and setting v_size),
		 * filled from the tree before it was truncated, and the old
		 * data would show once the file grew again.
		 */
		genfs_node_wrlock(vp);
		uvm_vnp_setsize(vp, vap->va_size);
		gefs_droppast(vp, vap->va_size);
		n.size = vap->va_size;
		mask |= Asize;
		locked = 1;
	}
attrs:
	if(mask == 0){
		if(locked)
			genfs_node_unlock(vp);
		return 0;
	}
	{
		struct saargs sa = { .de = de, .a = &n, .mask = mask, .uid = uid };
		error = gefs_call(gm, gefs_sajob, &sa);
	}
	if(locked)
		genfs_node_unlock(vp);
	return error;
}

/* --- data --- */

static int
gefs_read(void *v)
{
	struct vop_read_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	const int ioflag = ap->a_ioflag;
	vsize_t len;
	int error;

	if(vp->v_type == VDIR)
		return EISDIR;
	if(vp->v_type != VREG || uio->uio_offset < 0)
		return EINVAL;
	if(uio->uio_resid == 0)
		return 0;
	error = 0;
	while(error == 0 && uio->uio_resid > 0){
		if(vp->v_size <= uio->uio_offset)
			break;
		len = MIN(vp->v_size - uio->uio_offset, uio->uio_resid);
		error = ubc_uiomove(&vp->v_uobj, uio, len, IO_ADV_DECODE(ioflag),
		    UBC_READ | UBC_PARTIALOK | UBC_VNODE_FLAGS(vp));
	}
	if(error == 0)
		gefs_readatime(vp);
	return error;
}

static int
gefs_write(void *v)
{
	struct vop_write_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	const int ioflag = ap->a_ioflag;
	struct gefsmount *gm = GMP(vp);
	Dent *de = VTODE(vp);
	off_t osize, nsize;
	vsize_t len;
	int error;

	if(vp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;
	if(vp->v_type != VREG || uio->uio_offset < 0)
		return EINVAL;
	if(uio->uio_resid == 0)
		return 0;
	if((error = gefs_werror(vp)) != 0)
		return error;
	/*
	 * File data takes its blocks when its pages are put, after write(2)
	 * returned; say ENOSPC here, with a margin for what is in flight,
	 * rather than lose data then.  What was freed since the last commit
	 * is free after one.
	 */
	if(gefsavail(gm->gm_fs) < uio->uio_resid + GEFS_WMARGIN){
		(void)gefs_commit(gm, 1, Clazy);
		(void)gefs_commit(gm, 1, Clazy);
		if(gefsavail(gm->gm_fs) < uio->uio_resid + GEFS_WMARGIN)
			return ENOSPC;
	}
	osize = vp->v_size;
	if(ioflag & IO_APPEND)
		uio->uio_offset = osize;
	if(uio->uio_offset + uio->uio_resid > (1LL<<62))
		return EFBIG;
	nsize = osize;
	if(uio->uio_offset + uio->uio_resid > osize){
		off_t rem;

		nsize = uio->uio_offset + uio->uio_resid;
		uvm_vnp_setwritesize(vp, nsize);
		rem = round_page(nsize) - nsize;
		if(rem > 0)
			ubc_zerorange(&vp->v_uobj, nsize, rem, UBC_VNODE_FLAGS(vp));
	}
	error = 0;
	while(error == 0 && uio->uio_resid > 0){
		off_t oldoff = uio->uio_offset;

		len = MIN(uio->uio_resid, GEFS_WBEHIND);
		error = ubc_uiomove(&vp->v_uobj, uio, len, IO_ADV_DECODE(ioflag),
		    UBC_WRITE | UBC_VNODE_FLAGS(vp));
		/*
		 * Write-behind, as FFS: whole windows this write filled go
		 * into the tree now (not to the disk: that is the commit's),
		 * so a big write does not fill memory with dirty pages that
		 * only the mutator can clean.  Small files and appends stay
		 * in memory until the syncer.
		 */
		if(error == 0 && oldoff / GEFS_WBEHIND !=
		    uio->uio_offset / GEFS_WBEHIND){
			if(gefs_trace)
				printf("gefs: write-behind qid %llx %lld..%lld\n",
				    (unsigned long long)de->qid.path,
				    (long long)rounddown(oldoff, GEFS_WBEHIND),
				    (long long)rounddown(uio->uio_offset, GEFS_WBEHIND));
			rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
			error = VOP_PUTPAGES(vp,
			    rounddown(oldoff, GEFS_WBEHIND),
			    rounddown(uio->uio_offset, GEFS_WBEHIND),
			    PGO_CLEANIT);
		}
	}
	if(error){
		uvm_vnp_setsize(vp, osize);
		return error;
	}
	if(nsize != osize){
		uvm_vnp_setsize(vp, nsize);
		wlock(de);
		de->length = nsize;
		wunlock(de);
	}
	/* in memory for stat now; the tree gets it when the pages are put */
	wlock(de);
	de->mtime = nsec();
	wunlock(de);
	if(ioflag & IO_SYNC){
		rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
		error = VOP_PUTPAGES(vp, 0, 0, PGO_ALLPAGES | PGO_CLEANIT |
		    PGO_SYNCIO);
		if(error == 0)
			error = gefs_commit(gm, 1, 0);
	}
	return error;
}

static int
gefs_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct gefsmount *gm = GMP(vp);
	int error;

	error = 0;
	if(vp->v_type == VREG){
		rw_enter(vp->v_uobj.vmobjlock, RW_WRITER);
		error = VOP_PUTPAGES(vp, 0, 0, PGO_ALLPAGES | PGO_CLEANIT |
		    ((ap->a_flags & FSYNC_WAIT) ? PGO_SYNCIO : 0));
		if(error == 0)
			error = gefs_werror(vp);
		if(error)
			return error;
	}
	/*
	 * No intent log: a durable fsync waits for a commit, one that every
	 * fsync it covers shares (gefs_durable).  Not when the vnode is being
	 * reclaimed, which flushes its pages this way too.
	 */
	if(!(ap->a_flags & FSYNC_LAZY) && !(vp->v_mount->mnt_flag & MNT_RDONLY)){
		int dying;

		mutex_enter(vp->v_interlock);
		dying = vdead_check(vp, VDEAD_NOWAIT) != 0;
		mutex_exit(vp->v_interlock);
		if(!dying)
			error = gefs_commit(gm, 1, 0);
	}
	return error;
}

/* --- removing --- */

static int
gefs_unlink(struct vnode *dvp, struct vnode *vp, struct componentname *cnp)
{
	struct gefsmount *gm = GMP(dvp);
	Gattr a;
	int error;

	gefsgetattr(VTODE(vp), &a);
	if(a.flags & Gappend)
		return EPERM;
	gefsgetattr(VTODE(dvp), &a);
	if(a.flags & Gappend)
		return EPERM;
	{
		struct rmargs r = { .dir = VTODE(dvp), .name = cnp->cn_nameptr,
		    .n = cnp->cn_namelen, .de = VTODE(vp) };
		error = gefs_call(gm, gefs_rmjob, &r);
	}
	if(error == 0){
		cache_purge(vp);
		if(vp->v_type == VDIR)
			cache_purge(dvp);
	}
	return error;
}

static int
gefs_remove(void *v)
{
	struct vop_remove_v3_args *ap = v;
	struct vnode *dvp = ap->a_dvp, *vp = ap->a_vp;
	int error;

	if(vp->v_type == VDIR)
		error = EPERM;
	else
		error = gefs_unlink(dvp, vp, ap->a_cnp);
	ap->ctx_vp_new_nlink = (error == 0) ? 0 : VTODE(vp)->nlink;
	if(dvp == vp)
		vrele(vp);
	else
		vput(vp);
	return error;
}

static int
gefs_rmdir(void *v)
{
	struct vop_rmdir_v2_args *ap = v;
	struct vnode *dvp = ap->a_dvp, *vp = ap->a_vp;
	int error;

	if(dvp == vp)
		error = EINVAL;
	else if(vp->v_type != VDIR)
		error = ENOTDIR;
	else if(vp->v_vflag & VV_ROOT)
		error = EBUSY;
	else
		error = gefs_unlink(dvp, vp, ap->a_cnp);
	vput(vp);
	return error;
}

static int
gefs_link(void *v)
{
	struct vop_link_v2_args *ap = v;
	struct vnode *dvp = ap->a_dvp, *vp = ap->a_vp;
	struct componentname *cnp = ap->a_cnp;
	struct gefsmount *gm = GMP(dvp);
	Gattr a;
	int error;

	if(vp->v_type == VDIR)
		return EPERM;
	if(dvp->v_mount != vp->v_mount)
		return EXDEV;
	if((error = vn_lock(vp, LK_EXCLUSIVE)) != 0)
		return error;
	gefsgetattr(VTODE(vp), &a);
	if(a.flags & Gappend){
		error = EPERM;
		goto out;
	}
	error = kauth_authorize_vnode(cnp->cn_cred, KAUTH_VNODE_ADD_LINK, vp,
	    dvp, 0);
	if(error)
		goto out;
	{
		struct lnargs l = { .dir = VTODE(dvp), .name = cnp->cn_nameptr,
		    .n = cnp->cn_namelen, .de = VTODE(vp) };
		error = gefs_call(gm, gefs_lnjob, &l);
	}
	if(error == 0)
		cache_enter(dvp, vp, cnp->cn_nameptr, cnp->cn_namelen,
		    cnp->cn_flags);
out:
	VOP_UNLOCK(vp);
	return error;
}

/* --- rename, through genfs's rename framework --- */

static bool
gefs_rmdired_p(struct vnode *vp)
{
	return VTODE(vp)->gone;
}

static int
gefs_gro_lock_directory(struct mount *mp, struct vnode *vp)
{
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	if(gefs_rmdired_p(vp)){
		VOP_UNLOCK(vp);
		return ENOENT;
	}
	return 0;
}

static bool
gefs_gro_directory_empty_p(struct mount *mp, kauth_cred_t cred,
    struct vnode *vp, struct vnode *dvp)
{
	struct gefsmount *gm = VFSTOGEFS(mp);
	int error, r;

	r = 0;
	error = 0;
	{
		GEFS_BEGIN(gm, error, out);
		epochstart(0);
		if(waserror()){
			epochend(0);
			nexterror();
		}
		r = gefsisempty(gm->gm_mnt, VTODE(vp));
		poperror();
		epochend(0);
		GEFS_END();
	}
out:
	return error == 0 && r;
}

static int
gefs_gro_rename_check_possible(struct mount *mp,
    struct vnode *fdvp, struct vnode *fvp,
    struct vnode *tdvp, struct vnode *tvp)
{
	Gattr fd, f, td, t;

	gefsgetattr(VTODE(fdvp), &fd);
	gefsgetattr(VTODE(fvp), &f);
	gefsgetattr(VTODE(tdvp), &td);
	if(tvp != NULL)
		gefsgetattr(VTODE(tvp), &t);
	else
		memset(&t, 0, sizeof(t));
	return genfs_ufslike_rename_check_possible(
	    g2flags(fd.flags), g2flags(f.flags), g2flags(td.flags),
	    g2flags(t.flags), tvp != NULL, IMMUTABLE, APPEND);
}

static int
gefs_gro_rename_check_permitted(struct mount *mp, kauth_cred_t cred,
    struct vnode *fdvp, struct vnode *fvp,
    struct vnode *tdvp, struct vnode *tvp)
{
	Gattr fd, f, td, t;

	gefsgetattr(VTODE(fdvp), &fd);
	gefsgetattr(VTODE(fvp), &f);
	gefsgetattr(VTODE(tdvp), &td);
	if(tvp != NULL)
		gefsgetattr(VTODE(tvp), &t);
	else
		memset(&t, 0, sizeof(t));
	return genfs_ufslike_rename_check_permitted(cred,
	    fdvp, fd.perm, fd.uid,
	    fvp, f.uid,
	    tdvp, td.perm, td.uid,
	    tvp, (tvp != NULL) ? t.uid : 0);
}

static int
gefs_gro_remove_check_possible(struct mount *mp,
    struct vnode *dvp, struct vnode *vp)
{
	Gattr d, a;

	gefsgetattr(VTODE(dvp), &d);
	gefsgetattr(VTODE(vp), &a);
	return genfs_ufslike_remove_check_possible(g2flags(d.flags),
	    g2flags(a.flags), IMMUTABLE, APPEND);
}

static int
gefs_gro_remove_check_permitted(struct mount *mp, kauth_cred_t cred,
    struct vnode *dvp, struct vnode *vp)
{
	Gattr d, a;

	gefsgetattr(VTODE(dvp), &d);
	gefsgetattr(VTODE(vp), &a);
	return genfs_ufslike_remove_check_permitted(cred, dvp, d.perm, d.uid,
	    vp, a.uid);
}

static int
gefs_gro_rename(struct mount *mp, kauth_cred_t cred,
    struct vnode *fdvp, struct componentname *fcnp,
    void *fde, struct vnode *fvp,
    struct vnode *tdvp, struct componentname *tcnp,
    void *tde, struct vnode *tvp, nlink_t *tvp_nlinkp)
{
	struct gefsmount *gm = VFSTOGEFS(mp);
	int error;

	{
		struct mvargs m = { .fdir = VTODE(fdvp),
		    .fname = fcnp->cn_nameptr, .nf = fcnp->cn_namelen,
		    .de = VTODE(fvp),
		    .tdir = VTODE(tdvp), .tname = tcnp->cn_nameptr,
		    .n = tcnp->cn_namelen,
		    .tde = tvp != NULL ? VTODE(tvp) : nil };
		error = gefs_call(gm, gefs_mvjob, &m);
	}
	if(error == 0){
		if(tvp != NULL)
			*tvp_nlinkp = 0;
		cache_purge(fvp);
		if(tvp != NULL)
			cache_purge(tvp);
	}
	return error;
}

static int
gefs_gro_remove(struct mount *mp, kauth_cred_t cred,
    struct vnode *dvp, struct componentname *cnp, void *de, struct vnode *vp,
    nlink_t *tvp_nlinkp)
{
	int error;

	error = gefs_unlink(dvp, vp, cnp);
	if(error == 0)
		*tvp_nlinkp = 0;
	return error;
}

static int
gefs_gro_lookup(struct mount *mp, struct vnode *dvp,
    struct componentname *cnp, void *de_ret, struct vnode **vp_ret)
{
	struct gefsmount *gm = VFSTOGEFS(mp);
	Dent *volatile de;
	int error;

	de = nil;
	error = 0;
	{
		GEFS_BEGIN(gm, error, out);
		de = gefslookup(gm->gm_mnt, VTODE(dvp), cnp->cn_nameptr,
		    cnp->cn_namelen);
		GEFS_END();
	}
out:
	if(error)
		return error;
	if(de == nil)
		return ENOENT;
	error = gefs_vnget(mp, de, vp_ret);
	{
		int e2 = 0;
		GEFS_BEGIN(gm, e2, out2);
		clunkdent(gm->gm_mnt, de);
		GEFS_END();
	out2:	;
	}
	return error;
}

/* the directory's parent, as a referenced vnode */
static int
gefs_parent(struct mount *mp, struct vnode *vp, struct vnode **pvp)
{
	struct gefsmount *gm = VFSTOGEFS(mp);
	Dent *volatile de;
	int error;

	de = nil;
	error = 0;
	{
		GEFS_BEGIN(gm, error, out);
		de = gefslookup(gm->gm_mnt, VTODE(vp), "..", 2);
		GEFS_END();
	}
out:
	if(error)
		return error;
	if(de == nil)
		return ENOENT;
	error = gefs_vnget(mp, de, pvp);
	{
		int e2 = 0;
		GEFS_BEGIN(gm, e2, out2);
		clunkdent(gm->gm_mnt, de);
		GEFS_END();
	out2:	;
	}
	return error;
}

static int
gefs_gro_genealogy(struct mount *mp, kauth_cred_t cred,
    struct vnode *fdvp, struct vnode *tdvp,
    struct vnode **intermediate_node_ret)
{
	struct vnode *vp, *pvp;
	int error;

	error = gefs_gro_lock_directory(mp, tdvp);
	if(error)
		return error;
	vp = tdvp;
	vref(vp);
	for(;;){
		if(vp->v_vflag & VV_ROOT){
			vput(vp);
			*intermediate_node_ret = NULL;
			return 0;
		}
		error = gefs_parent(mp, vp, &pvp);
		if(error){
			vput(vp);
			return error;
		}
		if(pvp == fdvp){
			vrele(pvp);
			VOP_UNLOCK(vp);
			*intermediate_node_ret = vp;
			return 0;
		}
		if(pvp == vp){
			vrele(pvp);
			vput(vp);
			*intermediate_node_ret = NULL;
			return 0;
		}
		vput(vp);
		vp = pvp;
		error = vn_lock(vp, LK_EXCLUSIVE);
		if(error){
			vrele(vp);
			return error;
		}
		if(gefs_rmdired_p(vp)){
			vput(vp);
			return ENOENT;
		}
	}
}

static const struct genfs_rename_ops gefs_genfs_rename_ops = {
	.gro_directory_empty_p		= gefs_gro_directory_empty_p,
	.gro_rename_check_possible	= gefs_gro_rename_check_possible,
	.gro_rename_check_permitted	= gefs_gro_rename_check_permitted,
	.gro_remove_check_possible	= gefs_gro_remove_check_possible,
	.gro_remove_check_permitted	= gefs_gro_remove_check_permitted,
	.gro_rename			= gefs_gro_rename,
	.gro_remove			= gefs_gro_remove,
	.gro_lookup			= gefs_gro_lookup,
	.gro_genealogy			= gefs_gro_genealogy,
	.gro_lock_directory		= gefs_gro_lock_directory,
};

static int
gefs_sane_rename(struct vnode *fdvp, struct componentname *fcnp,
    struct vnode *tdvp, struct componentname *tcnp,
    kauth_cred_t cred, bool posixly_correct)
{
	Dent *fde, *tde;

	return genfs_sane_rename(&gefs_genfs_rename_ops,
	    fdvp, fcnp, &fde, tdvp, tcnp, &tde, cred, posixly_correct);
}

static int
gefs_rename(void *v)
{
	return genfs_insane_rename(v, &gefs_sane_rename);
}

/* --- directories and links --- */

struct rdctx {
	char	*buf;
	size_t	len;
	size_t	off;
	off_t	n;	/* entries taken */
	int	skip;	/* entries to pass over first */
};

static int
rdfill(void *arg, Xdir *d)
{
	struct rdctx *c = arg;
	struct dirent *dp;
	size_t nl, rl;

	if(c->skip > 0){
		c->skip--;
		c->n++;
		return 0;
	}
	nl = strlen(d->name);
	rl = _DIRENT_RECLEN(dp, nl);
	if(c->off + rl > c->len)
		return -1;
	dp = (struct dirent *)(c->buf + c->off);
	memset(dp, 0, rl);
	dp->d_fileno = d->qid.path + 1;
	dp->d_reclen = rl;
	dp->d_namlen = nl;
	switch(gefsxtype(d)){
	case Gdir:	dp->d_type = DT_DIR;	break;
	case Glnk:	dp->d_type = DT_LNK;	break;
	case Gchr:	dp->d_type = DT_CHR;	break;
	case Gblk:	dp->d_type = DT_BLK;	break;
	case Gfifo:	dp->d_type = DT_FIFO;	break;
	case Gsock:	dp->d_type = DT_SOCK;	break;
	default:	dp->d_type = DT_REG;	break;
	}
	memcpy(dp->d_name, d->name, nl);
	c->off += rl;
	c->n++;
	return 0;
}

static int
dotent(struct rdctx *c, const char *name, ino_t ino)
{
	struct dirent *dp;
	size_t nl, rl;

	nl = strlen(name);
	rl = _DIRENT_RECLEN(dp, nl);
	if(c->off + rl > c->len)
		return -1;
	dp = (struct dirent *)(c->buf + c->off);
	memset(dp, 0, rl);
	dp->d_fileno = ino;
	dp->d_reclen = rl;
	dp->d_namlen = nl;
	dp->d_type = DT_DIR;
	memcpy(dp->d_name, name, nl);
	c->off += rl;
	return 0;
}

/*
 * Offsets are entry numbers: 0 is ".", 1 is "..", then the entries in
 * key (name) order.  The scan where the last call stopped is kept, so a
 * directory read in order is one pass; any other offset rescans.
 */
static int
gefs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct gefsmount *gm = GMP(vp);
	struct gefsnode *gn = VTOGN(vp);
	Dent *de = VTODE(vp);
	struct rdctx c;
	off_t off;
	int error, more;
	ino_t pino;
	Scan *s;

	if(vp->v_type != VDIR)
		return ENOTDIR;
	if(ap->a_ncookies != NULL)
		*ap->a_ncookies = 0;
	if(ap->a_cookies != NULL)
		*ap->a_cookies = NULL;
	off = uio->uio_offset;
	if(off < 0)
		return EINVAL;
	if(uio->uio_resid < _DIRENT_MINSIZE((struct dirent *)0))
		return EINVAL;
	c.len = MIN(uio->uio_resid, 64*1024);
	c.buf = kmem_alloc(c.len, KM_SLEEP);
	c.off = 0;
	c.n = off;
	c.skip = 0;
	more = 0;
	error = 0;
	if(de->gone)
		goto done;
	if(c.n == 0){
		if(dotent(&c, ".", de->qid.path + 1) == -1)
			goto done;
		c.n = 1;
	}
	if(c.n == 1){
		pino = (de->up == -1) ? de->qid.path + 1 : (ino_t)de->up + 1;
		if(dotent(&c, "..", pino) == -1)
			goto done;
		c.n = 2;
	}
	mutex_enter(&gn->gn_scanlk);
	s = gn->gn_scan;
	if(s == NULL || gn->gn_scanoff != c.n){
		/* not where the last call stopped: start over and skip */
		if(s == NULL){
			s = kmem_alloc(sizeof(Scan), KM_SLEEP);
			gn->gn_scan = s;
		}
		memset(s, 0, sizeof(*s));
		c.skip = c.n - 2;
		c.n = 2;
	}
	{
		GEFS_BEGIN(gm, error, out);
		more = gefsreaddir(gm->gm_mnt, de, s, rdfill, &c);
		GEFS_END();
	}
out:
	gn->gn_scanoff = c.n;
	mutex_exit(&gn->gn_scanlk);
done:
	if(error == 0 && c.off > 0)
		error = uiomove(c.buf, c.off, uio);
	if(error == 0)
		uio->uio_offset = c.n;
	if(ap->a_eofflag != NULL)
		*ap->a_eofflag = (error == 0 && !more && c.off < c.len);
	kmem_free(c.buf, c.len);
	return error;
}

static int
gefs_readlink(void *v)
{
	struct vop_readlink_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct gefsmount *gm = GMP(vp);
	Dent *de = VTODE(vp);
	char *buf;
	long n;
	int error;

	if(vp->v_type != VLNK)
		return EINVAL;
	if(de->length <= 0 || de->length > Blksz)
		return EIO;
	buf = kmem_alloc(de->length, KM_SLEEP);
	n = 0;
	error = 0;
	{
		GEFS_BEGIN(gm, error, out);
		n = gefsread(gm->gm_mnt, de, buf, 0, de->length);
		GEFS_END();
	}
out:
	if(error == 0)
		error = uiomove(buf, MIN(n, uio->uio_resid), uio);
	kmem_free(buf, de->length);
	return error;
}

/* --- vnode life --- */

static int
gefs_inactive(void *v)
{
	struct vop_inactive_v2_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct gefsmount *gm = GMP(vp);
	Dent *de = VTODE(vp);
	int error;

	*ap->a_recycle = false;
	if(de->orphan){
		/*
		 * The last user of a removed file: drop its pages unwritten
		 * (reclaim would flush them into an entry that is gone),
		 * then free its data.
		 */
		if(vp->v_type == VREG)
			uvm_vnp_setsize(vp, 0);
		/* the job keeps the entry; the orphanage covers a crash */
		aincl(&de->ref, 1);
		gefs_post(gm, gefs_reljob, de);
		*ap->a_recycle = true;
		USED(error);
	}
	if(de->gone)
		*ap->a_recycle = true;
	return 0;
}

static int
gefs_reclaim(void *v)
{
	struct vop_reclaim_v2_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct gefsmount *gm = GMP(vp);
	struct gefsnode *gn = VTOGN(vp);
	Dent *de = gn->gn_de;
	int error;

	VOP_UNLOCK(vp);
	genfs_node_destroy(vp);
	vp->v_data = NULL;
	if(gn->gn_scan != NULL)
		kmem_free(gn->gn_scan, sizeof(Scan));
	mutex_destroy(&gn->gn_scanlk);
	de->vdata = NULL;
	error = 0;
	{
		GEFS_BEGIN(gm, error, out);
		clunkdent(gm->gm_mnt, de);
		GEFS_END();
	}
out:
	kmem_free(gn, sizeof(*gn));
	return 0;
}

/* --- block I/O by file offset --- */

static int
gefs_bmap(void *v)
{
	struct vop_bmap_args *ap = v;

	if(ap->a_vpp != NULL)
		*ap->a_vpp = ap->a_vp;
	if(ap->a_bnp != NULL)
		*ap->a_bnp = ap->a_bn << (Lgblk - DEV_BSHIFT);
	if(ap->a_runp != NULL)
		*ap->a_runp = (MAXPHYS >> Lgblk) - 1;
	return 0;
}

/* page I/O goes to the reader and mutator threads, which biodone it */
static int
gefs_strategy(void *v)
{
	struct vop_strategy_args *ap = v;

	gefs_queuebuf(GMP(ap->a_vp), ap->a_vp, ap->a_bp);
	return 0;
}

static int
gefs_gop_alloc(struct vnode *vp, off_t off, off_t len, int flags,
    kauth_cred_t cred)
{
	/* copy on write: blocks are allocated when the data is written */
	return 0;
}

static void
gefs_gop_markupdate(struct vnode *vp, int flags)
{
}

const struct genfs_ops gefs_genfsops = {
	.gop_size = genfs_size,
	.gop_alloc = gefs_gop_alloc,
	.gop_write = genfs_gop_write,
	.gop_markupdate = gefs_gop_markupdate,
	.gop_putrange = genfs_gop_putrange,
};

/* --- the rest --- */

static int
gefs_print(void *v)
{
	struct vop_print_args *ap = v;
	Dent *de = VTODE(ap->a_vp);

	printf("tag VT_GEFS, qid %llx, length %lld%s%s\n",
	    (unsigned long long)de->qid.path, (long long)de->length,
	    de->orphan ? ", removed" : "", de->gone ? ", gone" : "");
	return 0;
}

/* administration (gefs.h): snapshots, the schedule, status */
static int
gefs_ioctl(void *v)
{
	struct vop_ioctl_args /* {
		struct vnode *a_vp;
		u_long a_command;
		void *a_data;
		int a_fflag;
		kauth_cred_t a_cred;
	} */ *ap = v;
	struct gefsmount *gm = GMP(ap->a_vp);

	switch(ap->a_command){
	case GEFSIOC_CTL:
		return gefs_admin(gm, ap->a_data, 0, ap->a_cred);
	case GEFSIOC_STATUS:
		return gefs_admin(gm, ap->a_data, 1, ap->a_cred);
	}
	return genfs_enoioctl(v);
}

static int
gefs_pathconf(void *v)
{
	struct vop_pathconf_args *ap = v;

	switch(ap->a_name){
	case _PC_LINK_MAX:
		*ap->a_retval = INT_MAX;
		return 0;
	case _PC_NAME_MAX:
		*ap->a_retval = Maxname;
		return 0;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		return 0;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		return 0;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		return 0;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		return 0;
	case _PC_SYNC_IO:
		*ap->a_retval = 1;
		return 0;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 62;
		return 0;
	default:
		return genfs_pathconf(ap);
	}
}

static int
gefs_advlock(void *v)
{
	struct vop_advlock_args *ap = v;
	struct vnode *vp = ap->a_vp;

	return lf_advlock(ap, &VTOGN(vp)->gn_lockf, vp->v_size);
}

int (**gefs_vnodeop_p)(void *);
static const struct vnodeopv_entry_desc gefs_vnodeop_entries[] = {
	{ &vop_default_desc,		vn_default_error },
	{ &vop_parsepath_desc,		genfs_parsepath },
	{ &vop_lookup_desc,		gefs_lookup },
	{ &vop_create_desc,		gefs_create },
	{ &vop_mknod_desc,		gefs_mknod },
	{ &vop_open_desc,		gefs_open },
	{ &vop_close_desc,		gefs_close },
	{ &vop_access_desc,		gefs_access },
	{ &vop_accessx_desc,		genfs_accessx },
	{ &vop_getattr_desc,		gefs_getattr },
	{ &vop_setattr_desc,		gefs_setattr },
	{ &vop_read_desc,		gefs_read },
	{ &vop_write_desc,		gefs_write },
	{ &vop_fallocate_desc,		genfs_eopnotsupp },
	{ &vop_fdiscard_desc,		genfs_eopnotsupp },
	{ &vop_ioctl_desc,		gefs_ioctl },
	{ &vop_fcntl_desc,		genfs_fcntl },
	{ &vop_poll_desc,		genfs_poll },
	{ &vop_kqfilter_desc,		genfs_kqfilter },
	{ &vop_revoke_desc,		genfs_revoke },
	{ &vop_mmap_desc,		genfs_mmap },
	{ &vop_fsync_desc,		gefs_fsync },
	{ &vop_seek_desc,		genfs_seek },
	{ &vop_remove_desc,		gefs_remove },
	{ &vop_link_desc,		gefs_link },
	{ &vop_rename_desc,		gefs_rename },
	{ &vop_mkdir_desc,		gefs_mkdir },
	{ &vop_rmdir_desc,		gefs_rmdir },
	{ &vop_symlink_desc,		gefs_symlink },
	{ &vop_readdir_desc,		gefs_readdir },
	{ &vop_readlink_desc,		gefs_readlink },
	{ &vop_abortop_desc,		genfs_abortop },
	{ &vop_inactive_desc,		gefs_inactive },
	{ &vop_reclaim_desc,		gefs_reclaim },
	{ &vop_lock_desc,		genfs_lock },
	{ &vop_unlock_desc,		genfs_unlock },
	{ &vop_bmap_desc,		gefs_bmap },
	{ &vop_strategy_desc,		gefs_strategy },
	{ &vop_print_desc,		gefs_print },
	{ &vop_pathconf_desc,		gefs_pathconf },
	{ &vop_islocked_desc,		genfs_islocked },
	{ &vop_advlock_desc,		gefs_advlock },
	{ &vop_bwrite_desc,		genfs_nullop },
	{ &vop_getpages_desc,		genfs_getpages },
	{ &vop_putpages_desc,		genfs_putpages },
	{ NULL, NULL }
};
const struct vnodeopv_desc gefs_vnodeop_opv_desc =
	{ &gefs_vnodeop_p, gefs_vnodeop_entries };

int (**gefs_specop_p)(void *);
static const struct vnodeopv_entry_desc gefs_specop_entries[] = {
	{ &vop_default_desc,		vn_default_error },
	GENFS_SPECOP_ENTRIES,
	{ &vop_close_desc,		spec_close },
	{ &vop_access_desc,		gefs_access },
	{ &vop_accessx_desc,		genfs_accessx },
	{ &vop_getattr_desc,		gefs_getattr },
	{ &vop_setattr_desc,		gefs_setattr },
	{ &vop_read_desc,		spec_read },
	{ &vop_write_desc,		spec_write },
	{ &vop_fcntl_desc,		genfs_fcntl },
	{ &vop_fsync_desc,		spec_fsync },
	{ &vop_inactive_desc,		gefs_inactive },
	{ &vop_reclaim_desc,		gefs_reclaim },
	{ &vop_lock_desc,		genfs_lock },
	{ &vop_unlock_desc,		genfs_unlock },
	{ &vop_print_desc,		gefs_print },
	{ &vop_islocked_desc,		genfs_islocked },
	{ &vop_bwrite_desc,		vn_bwrite },
	{ NULL, NULL }
};
const struct vnodeopv_desc gefs_specop_opv_desc =
	{ &gefs_specop_p, gefs_specop_entries };

int (**gefs_fifoop_p)(void *);
static const struct vnodeopv_entry_desc gefs_fifoop_entries[] = {
	{ &vop_default_desc,		vn_default_error },
	GENFS_FIFOOP_ENTRIES,
	{ &vop_close_desc,		vn_fifo_bypass },
	{ &vop_access_desc,		gefs_access },
	{ &vop_accessx_desc,		genfs_accessx },
	{ &vop_getattr_desc,		gefs_getattr },
	{ &vop_setattr_desc,		gefs_setattr },
	{ &vop_read_desc,		vn_fifo_bypass },
	{ &vop_write_desc,		vn_fifo_bypass },
	{ &vop_fcntl_desc,		genfs_fcntl },
	{ &vop_fsync_desc,		vn_fifo_bypass },
	{ &vop_inactive_desc,		gefs_inactive },
	{ &vop_reclaim_desc,		gefs_reclaim },
	{ &vop_lock_desc,		genfs_lock },
	{ &vop_unlock_desc,		genfs_unlock },
	{ &vop_print_desc,		gefs_print },
	{ &vop_islocked_desc,		genfs_islocked },
	{ &vop_bwrite_desc,		vn_bwrite },
	{ NULL, NULL }
};
const struct vnodeopv_desc gefs_fifoop_opv_desc =
	{ &gefs_fifoop_p, gefs_fifoop_entries };
