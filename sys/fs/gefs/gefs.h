/*
 * GEFS for NetBSD: mount arguments, administration ioctls, and the
 * kernel's per-device, per-mount and per-vnode state.
 */
#ifndef _FS_GEFS_GEFS_H_
#define _FS_GEFS_GEFS_H_

#if defined(_KERNEL) || !defined(__linux__)
#include <sys/ioccom.h>
#else
#include <sys/ioctl.h>	/* the userland tools also build on Linux */
#endif

#define MOUNT_GEFS	"gefs"

struct gefs_args {
	char	*fspec;		/* block device */
	char	snap[64];	/* label to mount (a fork or a snapshot), "" for "main" */
	int	cachemb;	/* block cache in MiB, 0 for the default */
	int	flags;
};

/*
 * Administration, as 9front's adm/ctl and adm/status: an ioctl on any
 * file or directory of a mounted gefs.  GEFSIOC_CTL runs one command:
 *
 *	snap label new		snapshot label as new (read-only)
 *	fork label new		the same, mutable
 *	delsnap label		delete a label
 *	retain label spec...	automatic snapshots of label ("-": default),
 *				e.g. "60@m 24@h 14@d", "none", "default"
 *	sync			commit everything now
 *	check			check the whole file system
 *	usereserve 0|1		allow the reserve, to delete when full
 *
 * If it fails and gc_buf is set, the reason goes there.  GEFSIOC_STATUS
 * copies out the status text (usage, retention, labels), and sets gc_len
 * to its length, which may exceed the buffer.
 */
struct gefs_ctl {
	char	gc_cmd[256];
	char	*gc_buf;
	size_t	gc_len;
};
#define GEFSIOC_CTL	_IOWR('G', 1, struct gefs_ctl)
#define GEFSIOC_STATUS	_IOWR('G', 2, struct gefs_ctl)

#ifdef _KERNEL

#include <miscfs/genfs/genfs_node.h>
#include <sys/queue.h>

#define GEFS_NREADERS	2
#define GEFS_WBEHIND	(1024*1024)	/* write-behind window */
#define GEFS_MAXWQ	64		/* page writes queued: 4 MiB of pager VA */
#define GEFS_WMARGIN	(64*1024*1024)	/* write(2) says ENOSPC this much early */

/* why the mutator commits (gd_nwhy) */
enum {
	GEFS_WSYNC,	/* fsync, O_SYNC, sync(2), shutdown */
	GEFS_WAGE,	/* a change is vfs.gefs.interval old */
	GEFS_WDIRTY,	/* queued blocks passed vfs.gefs.dirtypct of the cache */
	GEFS_WSPACE,	/* little space left: commit to free what was freed */
	GEFS_WLAZY,	/* only lazy updates (atime, frees), Lazymax old */
	GEFS_NWHY
};

struct gefsjob;
struct gefsmount;
TAILQ_HEAD(gefsjobq, gefsjob);

/*
 * One per device: the core's file system and the threads that change
 * it.  Every label mounted from the device shares it (a fork on
 * /usr/obj, a snapshot on /mnt, next to main on /), as 9front serves
 * all labels from one file server.
 */
struct gefsdev {
	LIST_ENTRY(gefsdev) gd_link;
	dev_t		gd_dev;
	struct vnode	*gd_devvp;
	struct Gefs	*gd_fs;		/* the core's file system */
	int		gd_fd;		/* device slot, p9kern.c */
	int		gd_openmode;	/* FREAD, or FREAD|FWRITE */
	int		gd_nrw;		/* read-write mounts; on the mutator */
	int		gd_busy;	/* a mount or unmount in progress */
	LIST_HEAD(, gefsmount) gd_mounts;
	/* the mutator and reader threads, gefs_work.c */
	kmutex_t	gd_jlk;
	kcondvar_t	gd_mcv;		/* work for the mutator */
	kcondvar_t	gd_rcv;		/* work for the readers */
	kcondvar_t	gd_dcv;		/* a job is done, a thread left */
	struct gefsjobq	gd_mq;
	struct gefsjobq	gd_rq;
	int		gd_nthreads;
	int		gd_stop;
	int		gd_nwq;		/* page writes queued or running */
	kcondvar_t	gd_wqcv;	/* room in the write queue */
	struct lwp	*gd_mutlwp;	/* the mutator */
	volatile int	gd_lazyq;	/* a lazy commit is queued */
	/*
	 * Commits, gefs_work.c: the mutator runs them, for requests (group
	 * commit) and by the background policy.  With gd_jlk.
	 */
	kcondvar_t	gd_cmtcv;	/* a commit finished, or failed */
	uint64_t	gd_jenq;	/* mutator jobs queued, ever */
	uint64_t	gd_jtaken;	/* mutator jobs taken, ever */
	int		gd_cmtwant;	/* a commit is requested ... */
	int		gd_cmtflags;	/* ... with these commit() flags ... */
	uint64_t	gd_cmtticket;	/* ... after the jobs queued before it ... */
	uint64_t	gd_cmtdue;	/* ... and not before this uptime, ns */
	int64_t		gd_cmtneed;	/* the latest commit a waiter needs */
	uint64_t	gd_nfail;	/* commits that failed waiters */
	int		gd_cmterr;	/* the last failure's errno */
	uint64_t	gd_dirtyat;	/* uptime the tree got dirty, 0: clean */
	uint64_t	gd_lastcmt;	/* uptime of the last commit written */
	int64_t		gd_ncmtseen;	/* the core's ncommit at the last look */
	uint64_t	gd_spaceat;	/* uptime of the last commit for space */
	/* counters, for vfs.gefs */
	uint64_t	gd_nreq;	/* durability requests (fsync, O_SYNC, sync) */
	uint64_t	gd_njoin;	/* ... that started no commit of their own */
	uint64_t	gd_nwhy[GEFS_NWHY];	/* commits run, by reason */
};

/* One per NetBSD mount: a label of a device. */
struct gefsmount {
	LIST_ENTRY(gefsmount) gm_link;	/* on gd_mounts */
	struct mount	*gm_mp;
	struct gefsdev	*gm_gd;
	struct Gefs	*gm_fs;		/* gm_gd->gd_fs */
	struct Mount	*gm_mnt;	/* the label mounted */
	struct Dent	*gm_rootde;
	int		gm_ronly;
	char		gm_label[64];
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
	volatile int	gn_werror;	/* a page write failed: for write, fsync, close */
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
int	p9capture(char *, int);

/* gefs_work.c */
int	gefs_work_start(struct gefsdev *);
void	gefs_work_stop(struct gefsdev *);
int	gefs_call(struct gefsmount *, void (*)(struct gefsmount *, void *), void *);
void	gefs_post(struct gefsmount *, void (*)(struct gefsmount *, void *), void *);
void	gefs_queuebuf(struct gefsmount *, struct vnode *, struct buf *);
int	gefs_durable(struct gefsmount *, int);
void	gefs_cmtrequest(struct gefsmount *, int);
void	gefs_kick(struct gefsdev *);
extern int	gefs_interval, gefs_groupwait, gefs_dirtypct, gefs_lazymax;

/* gefs_vfsops.c */
extern int	gefs_trace;
int	gefs_vnget(struct mount *, struct Dent *, struct vnode **);
int	gefs_commit(struct gefsmount *, int, int);
int	gefs_putall(struct mount *, int);
int	gefs_admin(struct gefsmount *, struct gefs_ctl *, int, kauth_cred_t);
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
