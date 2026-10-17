/*
 * POSIX-shaped operations on a mounted gefs, shared by the NetBSD vnode
 * operations and the userland tools.  Each runs with the file system set
 * (p9setfs) and under the caller's waserror(); errors are raised with
 * error() and turned into errno values by gefserrno().
 *
 * On-disk extensions (doc/DESIGN.org): 9front's entries are left exactly
 * as they are, so a 9front reader sees plain files and directories.
 */

enum {
	Khref	= Kconf+1,	/* 7, Ori's: qid[8] => refs[8] xdir[]: linked files */
	Kxattr	= Kconf+2,	/* 8: qid[8] name[] => value[]: extended attributes */
};

/* Xdir.flag */
#define GEFS_FSYMLINK	0x1ULL	/* Ori: the target is the file's data */
#define GEFS_FREFCNT	0x2ULL	/* Ori: more than one name, see Khref */
#define GEFS_FDEVNOD	0x4ULL	/* Ori: device, fifo or socket */
#define GEFS_FXATTR	0x8ULL	/* has Kxattr keys */

/* POSIX mode bits beyond 0777, at their 9P2000.u positions */
#define GEFS_DMSETUID	0x00080000
#define GEFS_DMSETGID	0x00040000
#define GEFS_DMSETVTX	0x00010000

/* device kinds, as in Ori's port */
enum {
	GEFS_DEVBLK = 1,
	GEFS_DEVCHR,
	GEFS_DEVSOCK,
	GEFS_DEVFIFO,
};

/* parent qid of files removed while still open; see gefsremove */
#define Qorphan		(Qmagic|0x100)
#define Xrdev		"gefs.rdev"

/* commit() flags */
enum {
	Clazy	= 1<<0,		/* also when only lazy updates are pending */
};
extern int gefs_lazymax;	/* vfs.gefs.lazymax, seconds */
#define Lazymax	((vlong)gefs_lazymax*Nsec)	/* lazy updates wait at most this long */

/* file types */
enum {
	Gnon,
	Greg,
	Gdir,
	Glnk,
	Gchr,
	Gblk,
	Gfifo,
	Gsock,
};

/* Gattr.flags */
enum {
	Gappend	= 1<<0,		/* DMAPPEND */
	Gnodump	= 1<<1,		/* DMTMP */
};

/* gefssetattr() mask */
enum {
	Aperm	= 1<<0,
	Auid	= 1<<1,
	Agid	= 1<<2,
	Asize	= 1<<3,
	Aatime	= 1<<4,
	Amtime	= 1<<5,
	Aflags	= 1<<6,
};

typedef struct Gattr Gattr;
struct Gattr {
	int	type;
	ulong	perm;		/* 07777 */
	ulong	flags;
	int	uid;
	int	gid;
	vlong	size;
	vlong	atime;		/* ns */
	vlong	mtime;		/* ns */
	uvlong	qid;
	ulong	vers;
	int	nlink;
	int	devt;
	uvlong	rdev;
};

/* sync.c */
int	commit(int);
void	drainsyncq(void);
void	upsert(Mount*, Msg*, int);
void	lazyupsert(Mount*, Msg*, int);
Tree*	snapfs(char*, char*, int, int);
void	snapcmd(char*, char*, int);
void	pushsweep(Tree*);
void	clearblocks(Mount*, vlong, vlong, vlong);

/* NetBSD: freeing and committing may use the reserve (a full fs can be emptied) */
void	resvbegin(void);
void	resvend(void);
int	lowspace(void);
vlong	gefsavail(Gefs*);

/* snapadm.c */
void	ctlcmd(char*, int);
int	ctltokenize(char*, char**, int);
char*	seprintstatus(char*, char*);
void	loadautos(Mount*);
void	autosnaps(void);
void	autodel(char*);
char*	seprintsnaptime(char*, char*, vlong);
int	parsesnaptime(char*, vlong*);

/* ops.c */
Dent*	getdent(Mount*, vlong, Xdir*);
void	clunkdent(Mount*, Dent*);
Dent*	gefsroot(Mount*);
Dent*	gefslookup(Mount*, Dent*, char*, int);
Dent*	gefscreate(Mount*, Dent*, char*, int, Gattr*, char*, int);
void	gefsremove(Mount*, Dent*, char*, int, Dent*);
void	gefsrename(Mount*, Dent*, char*, int, Dent*, Dent*, char*, int, Dent*);
void	gefslink(Mount*, Dent*, char*, int, Dent*);
int	gefsancestor(Mount*, Dent*, Dent*);
void	gefsgetattr(Dent*, Gattr*);
void	gefssetattr(Mount*, Dent*, Gattr*, int, int);
long	gefsread(Mount*, Dent*, char*, vlong, long);
long	gefswrite(Mount*, Dent*, char*, vlong, long, vlong, int);
void	gefstrunc(Mount*, Dent*, vlong, int);
void	gefsrelease(Mount*, Dent*);
void	gefsorphans(Mount*);
int	gefsreaddir(Mount*, Dent*, Scan*, int (*)(void*, Xdir*), void*);
int	gefsisempty(Mount*, Dent*);
int	gefserrno(char*);
_Noreturn void	gefserr(char*, ...);
int	gefsxtype(Xdir*);
int	gefsdevtype(Mount*, vlong);

/* the environment */
int	p9devflush(int);
