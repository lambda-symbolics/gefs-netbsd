/*
 * GEFS for NetBSD: mount arguments, and the kernel's per-mount and
 * per-vnode state.
 */
#ifndef _FS_GEFS_GEFS_H_
#define _FS_GEFS_GEFS_H_

#define MOUNT_GEFS	"gefs"

struct gefs_args {
	char	*fspec;		/* block device */
	char	snap[64];	/* snapshot label to mount, "" for "main" */
	int	cachemb;	/* block cache in MiB, 0 for the default */
	int	flags;
};

#ifdef _KERNEL

#include <miscfs/genfs/genfs_node.h>
#include <sys/queue.h>

#define GEFS_NREADERS	2

struct gefsjob;
TAILQ_HEAD(gefsjobq, gefsjob);

struct gefsmount {
	struct mount	*gm_mp;
	struct vnode	*gm_devvp;
	struct Gefs	*gm_fs;		/* the core's file system */
	struct Mount	*gm_mnt;	/* the snapshot label mounted */
	struct Dent	*gm_rootde;
	int		gm_fd;		/* device slot, p9kern.c */
	int		gm_ronly;
	char		gm_label[64];
	/* the mutator and reader threads, gefs_work.c */
	kmutex_t	gm_jlk;
	kcondvar_t	gm_mcv;		/* work for the mutator */
	kcondvar_t	gm_rcv;		/* work for the readers */
	kcondvar_t	gm_dcv;		/* a job is done, a thread left */
	struct gefsjobq	gm_mq;
	struct gefsjobq	gm_rq;
	int		gm_nthreads;
	int		gm_stop;
	volatile int	gm_lazyq;	/* a lazy commit is queued */
};

struct gefsnode {
	struct genfs_node gn_gnode;	/* first, for genfs */
	struct vnode	*gn_vp;
	struct Dent	*gn_de;		/* the vcache key is &gn_de */
	struct gefsmount *gn_gmp;
	struct lockf	*gn_lockf;
	kmutex_t	gn_scanlk;
	struct Scan	*gn_scan;	/* where the last readdir stopped */
	off_t		gn_scanoff;
	volatile int	gn_atimeq;	/* an atime update is queued */
};

#define VFSTOGEFS(mp)	((struct gefsmount *)(mp)->mnt_data)
#define VTOGN(vp)	((struct gefsnode *)(vp)->v_data)
#define VTODE(vp)	(VTOGN(vp)->gn_de)

extern int (**gefs_vnodeop_p)(void *);
extern int (**gefs_specop_p)(void *);
extern int (**gefs_fifoop_p)(void *);
extern const struct vnodeopv_desc gefs_vnodeop_opv_desc;
extern const struct vnodeopv_desc gefs_specop_opv_desc;
extern const struct vnodeopv_desc gefs_fifoop_opv_desc;
extern const struct genfs_ops gefs_genfsops;

/* p9kern.c */
int	p9kinit(void);
void	p9kfini(void);
int	gefsdevadd(struct vnode *, long long);
void	gefsdevdel(int);
void	gefsdevpend(int);
void	gefsdevstats(int, long long *, long long *, long long *);

/* gefs_work.c */
int	gefs_work_start(struct gefsmount *);
void	gefs_work_stop(struct gefsmount *);
int	gefs_call(struct gefsmount *, void (*)(struct gefsmount *, void *), void *);
void	gefs_post(struct gefsmount *, void (*)(struct gefsmount *, void *), void *);
void	gefs_queuebuf(struct gefsmount *, struct vnode *, struct buf *);

/* gefs_vfsops.c */
int	gefs_vnget(struct mount *, struct Dent *, struct vnode **);
int	gefs_commit(struct gefsmount *, int, int);
void	gefs_freefs(struct Gefs *);

/*
 * Every entry from NetBSD into the core: make the mount's file system
 * the current one for this LWP (keeping the one an outer entry set,
 * say for a page fault on a gefs file during a copy), and turn errors
 * into errno values.  No core error unwinds through a NetBSD frame.
 */
#define GEFS_BEGIN(gm, err, out)				\
	struct Gefs *_ofs = fs;					\
	p9setfs((gm)->gm_fs);					\
	if(waserror()){						\
		(err) = gefserrno(errmsg());			\
		p9setfs(_ofs);					\
		goto out;					\
	}
#define GEFS_END()						\
	do { poperror(); p9setfs(_ofs); } while(0)

#endif /* _KERNEL */
#endif /* _FS_GEFS_GEFS_H_ */
