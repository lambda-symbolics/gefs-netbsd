/*
 * The commit protocol and the background jobs, from 9front's fs.c, for
 * the NetBSD port.  What changed (see doc/DESIGN.org):
 *
 *  - Blocks queued for writing stay queued until a commit (or memory
 *    pressure, see blk.c); wrwait() drains the queues itself and then
 *    flushes the device's write cache, since there are no sync procs.
 *  - A commit flushes the device twice, after the arena headers and after
 *    the superblocks; the arena footers wait for the next commit's flush.
 *  - commit() does nothing when nothing changed.  In the kernel the
 *    mutator thread calls it: for fsync and sync (group commit: one
 *    commit for every request it covers), when the oldest change is
 *    vfs.gefs.interval old, under dirty-memory or space pressure, and
 *    at unmount (gefs_work.c).
 *  - Commits are numbered by their cut (cmtcut), and cmtdone says which
 *    one is durable, for fsync callers to wait on.
 *  - Mount.next is Mount.mnext (it shadowed Limbo.next).
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"

static void
wrbarrier(void)
{
	tracev("barrier", agetv(&fs->qgen));
	aincv(&fs->qgen, 1);
}

/* write out everything queued so far, then make it durable if flush */
static void
wrwait(int flush)
{
	tracev("wrwait", agetv(&fs->qgen));
	aincv(&fs->qgen, 1);
	qdrainall();
	if(flush && p9devflush(fs->fd) == -1)
		broke("%s: device cache flush failed", Eio);
	aincv(&fs->qgen, 1);
	tracev("flushed", agetv(&fs->qgen));
}

static int
fssync(void)
{
	Mount *mnt;
	Arena *a;
	Dlist *sdl, ddl;
	Tree *r;
	vlong cut;
	int i, ok;

	qlock(&fs->synclk);
	if(waserror()){
		fprint(2, "gefs: failed to sync: %s\n", errmsg());
		qunlock(&fs->synclk);
		nexterror();
	}

	/*
	 * Wait for data that we're syncing to hit disk
	 */
	tracem("flush1");
	wrbarrier();
	/*
	 * pass 0: Update all open snapshots, and
	 *  pack the blocks we want to sync. Snap
	 *  while holding the write lock, and then
	 *  wait until all the blocks they point at
	 *  have hit disk; once they're on disk, we
	 *  can take a consistent snapshot.
	 */
	sdl = emalloc(sizeof(Dlist), 1);
	qlock(&fs->mutlk);
	if(waserror()){
		free(sdl);
		qunlock(&fs->mutlk);
		nexterror();
	}
	tracem("packb");

	for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->mnext){
		if(mnt->flag & Lmut){
			r = agetp(&mnt->root);
			r = updatesnap(r, mnt->name, mnt->flag);
			aswapp(&mnt->root, r);
		}
	}

	/*
	 * Now that we've updated the snaps, we can sync the
	 * dlist; the snap tree will not change from here.
	 */
	dlsync();
	*sdl = fs->snapdl;
	ddl = fs->dropdl;
	ok = sdl->hd.addr == -1 && ddl.hd.addr == -1;
	fs->snapdl.hd = Zb;
	fs->snapdl.tl = Zb;
	fs->snapdl.ins = nil;
	fs->dropdl.hd = Zb;
	fs->dropdl.tl = Zb;
	fs->dropdl.ins = nil;
	traceb("syncdl.dl", sdl->hd);
	traceb("syncdl.rb", fs->snap.bp);
	for(i = 0; i < fs->narena; i++){
		a = &fs->arenas[i];
		qlock(a);
		/*
		 * because the log uses preallocated
		 * blocks, we need to write the log
		 * block out synchronously, or it may
		 * get reused.
		 */
		logbarrier(a, agetv(&fs->qgen));
		flushlog(a);

		packarena(a->h0->data, Blksz, a);
		packarena(a->h1->data, Blksz, a);
		finalize(a->h0);
		finalize(a->h1);
		fs->arenabp[i] = a->h0->bp;
		qunlock(a);
	}
	assert(fs->snapdl.hd.addr == -1);
	traceb("packsb.rb", fs->snap.bp);
	packsb(fs->sb0->buf, Blksz, fs);
	packsb(fs->sb1->buf, Blksz, fs);
	finalize(fs->sb0);
	finalize(fs->sb1);
	fs->snap.dirty = 0;
	aswapl(&fs->lazydirty, 0);
	cut = aincv(&fs->cmtcut, 1);
	qunlock(&fs->mutlk);
	poperror();

	/*
	 * pass 1: sync block headers; if we crash here,
	 *  the block footers are consistent, and we can
	 *  use them.
	 */
	tracem("arenas0");
	for(i = 0; i < fs->narena; i++)
		enqueue(fs->arenas[i].h0);
	wrbarrier();
	wrwait(1);

	/*
	 * pass 2: sync superblock; we have a consistent
	 * set of block headers, so if we crash, we can
	 * use the loaded block headers; the footers will
	 * get synced after so that we can use them next
	 * time around.
	 */
	tracem("supers");
	enqueue(fs->sb0);
	enqueue(fs->sb1);
	wrbarrier();
	wrwait(1);
	/* NetBSD: everything up to the cut is durable now */
	aswapv(&fs->cmtdone, cut);

	/*
	 * pass 3: sync block footers; if we crash here,
	 *  the block headers are consistent, and we can
	 *  use them.  NetBSD: written, not flushed: the
	 *  next commit's first flush makes them durable.
	 */
	tracem("arenas1");
	for(i = 0; i < fs->narena; i++)
		enqueue(fs->arenas[i].h1);
	wrwait(0);

	/*
	 * Pass 4: clean up the old snap tree's deadlist.
	 * we need to wait for all the new data to hit disk
	 * before we can free anything, otherwise it gets
	 * clobbered.
	 */
	tracem("snapdl");
	freedl(&ddl, 0);
	limbo(DFdlist, sdl);
	aincv(&fs->ncommit, 1);
	qunlock(&fs->synclk);
	tracem("synced");
	poperror();
	return ok;
}

static void	sweeps(void);

/*
 * NetBSD: a full file system.  Allocations stop at the arenas' reserve
 * (1/1024th, 512 KiB to 8 MiB each), except for what frees space
 * (removal, truncation, deleting snapshots) and for commits, which the
 * freeing needs: rm and umount work on a full file system, which on
 * 9front took "usereserve 1".
 */
void
resvbegin(void)
{
	aincl(&fs->allowresv, 1);
}

void
resvend(void)
{
	aincl(&fs->allowresv, -1);
}

/* half of the reserve is gone: commit, to get back what was freed */
int
lowspace(void)
{
	vlong fr, rs;
	int i;

	fr = rs = 0;
	for(i = 0; i < fs->narena; i++){
		fr += fs->arenas[i].size - fs->arenas[i].used;
		rs += fs->arenas[i].reserve;
	}
	return fr < rs/2;
}

/* bytes a write may still take, outside the reserve (unlocked, a hint) */
vlong
gefsavail(Gefs *f)
{
	vlong n, fr;
	int i;

	n = 0;
	for(i = 0; i < f->narena; i++){
		fr = f->arenas[i].size - f->arenas[i].used - f->arenas[i].reserve;
		if(fr > 0)
			n += fr;
	}
	return n;
}

/*
 * Commit if anything changed since the last commit (or, with Clazy, if
 * lazy updates such as atime are pending).  Returns whether it wrote.
 * From 9front's AOsync, including the allocation log compression.
 */
int
commit(int flags)
{
	Bptr bp, nb, *oldhd;
	Arena *a;
	Blk *b;
	int i, ok;

	if(agetl(&fs->rdonly))
		return 0;
	qlock(&fs->cmtlk);
	/*
	 * Lazy updates (atime, and the frees a commit leaves for the next
	 * one) ride along with the next real commit; alone they go out when
	 * asked (unmount, sync(2)) or once Lazymax has passed, which bounds
	 * what a crash can leak.
	 */
	if(!fs->snap.dirty){
		if(!agetl(&fs->lazydirty)
		|| (!(flags & Clazy) && nsec() - fs->lastcommit < Lazymax)){
			aincv(&fs->nskip, 1);
			qunlock(&fs->cmtlk);
			return 0;
		}
	}
	oldhd = emalloc(fs->narena*sizeof(Bptr), 1);
	resvbegin();
	if(waserror()){
		resvend();
		free(oldhd);
		qunlock(&fs->cmtlk);
		nexterror();
	}
	/* what 9front's tasks proc did every 5 s, with this commit */
	autosnaps();
	for(i = 0; i < fs->narena; i++){
		a = &fs->arenas[i];
		oldhd[i] = Zb;
		qlock(a);
		/*
		 * arbitrary heuristic -- try compressing
		 * when the log doubles in size.  NetBSD: not in an arena that
		 * is into its reserve; the new log is written before the old
		 * one is freed.
		 */
		if(a->nlog >= 2*a->lastlogsz && a->size - a->used > a->reserve){
			oldhd[i] = a->loghd;
			epochstart(0);
			if(waserror()){
				epochend(0);
				qunlock(a);
				nexterror();
			}
			compresslog(a);
			epochend(0);
			poperror();
		}
		qunlock(a);
		epochclean();
	}

	ok = fssync();	/* oldhd blocks leaked on error() */

	for(i = 0; i < fs->narena; i++){
		for(bp = oldhd[i]; bp.addr != -1; bp = nb){
			qlock(&fs->mutlk);
			if(waserror()){
				qunlock(&fs->mutlk);
				nexterror();
			}
			b = getblk(bp, 0);
			nb = b->logp;
			freeblk(nil, b);
			dropblk(b);
			qunlock(&fs->mutlk);
			poperror();
			epochclean();
		}
	}
	/*
	 * The deadlists this commit freed become durable with the next
	 * one, a lazy one if nothing else happens; so do the blocks of
	 * the trees whose deletion this commit made durable.
	 */
	fs->lastcommit = nsec();
	if(!ok)
		aswapl(&fs->lazydirty, 1);
	sweeps();
	poperror();
	resvend();
	free(oldhd);
	qunlock(&fs->cmtlk);
	return 1;
}

/*
 * Before unmounting: we can have stragglers in the deferred lists, so
 * keep committing until they are cleared out fully.
 */
void
drainsyncq(void)
{
	int n;

	while(1){
		for(n = 0; n != nelem(fs->limbo); n++){
			while(!epochclean())
				sleep(10);
		}
		qlock(&fs->synclk);
		wrwait(1);
		qunlock(&fs->synclk);
		fs->snap.dirty = 1;
		qlock(&fs->cmtlk);
		resvbegin();
		if(waserror()){
			resvend();
			qunlock(&fs->cmtlk);
			nexterror();
		}
		n = fssync();
		sweeps();
		poperror();
		resvend();
		qunlock(&fs->cmtlk);
		if(n && fs->sweepq == nil)
			break;
	}
}

/* --- mounts: one per snapshot label in use --- */

static Mount*
mountlookup(char *name)
{
	Mount *mnt;

	for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->mnext){
		if(strcmp(name, mnt->name) == 0){
			aincl(&mnt->ref, 1);
			return mnt;
		}
	}
	return nil;
}

Mount*
getmount(char *name)
{
	Mount *mnt, *p;
	Tree *t;
	int flg;

	if(strcmp(name, "dump") == 0){
		aincl(&fs->snapmnt->ref, 1);
		return fs->snapmnt;
	}

	qlock(&fs->mountlk);
	mnt = mountlookup(name);
	qunlock(&fs->mountlk);
	if(mnt != nil)
		return mnt;

	/*
	 * tricky -- we can't hold the mountlk
	 * across opensnap, since we may end up
	 * blocking on new blocks while in an
	 * epoch, so we need to load the snap,
	 * and see if we lost the race.
	 */
	if((t = opensnap(name, &flg)) == nil)
		error(Enosnap);
	mnt = nil;
	if(waserror()){
		closesnap(t);
		if(mnt != nil)
			freemount(mnt);
		nexterror();
	}
	mnt = emalloc(sizeof(*mnt), 1);
	aswapl(&mnt->ref, 1);
	snprint(mnt->name, sizeof(mnt->name), "%s", name);
	mnt->flag = flg;
	aswapp(&mnt->root, t);
	loadautos(mnt);	/* NetBSD: as 9front's getmount */
	poperror();

	qlock(&fs->mountlk);
	if((p = mountlookup(name)) != nil){
		qunlock(&fs->mountlk);
		closesnap(t);
		freemount(mnt);
		return p;
	}
	mnt->mnext = agetp(&fs->mounts);
	aswapp(&fs->mounts, mnt);
	qunlock(&fs->mountlk);
	return mnt;
}

void
clunkmount(Mount *mnt)
{
	Mount *p, **pp;

	if(mnt == nil)
		return;
	qlock(&fs->mountlk);
	if(aincl(&mnt->ref, -1) == 0){
		pp = nil;
		for(p = agetp(&fs->mounts); p != nil; p = p->mnext){
			if(p == mnt)
				break;
			pp = &p->mnext;
		}
		assert(p != nil);
		if(pp == nil)
			aswapp(&fs->mounts, p->mnext);
		else
			*pp = p->mnext;
		limbo(DFmnt, p);
	}
	qunlock(&fs->mountlk);
}

/* --- modifying a mounted tree --- */

void
upsert(Mount *mnt, Msg *m, int nm)
{
	Tree *r;

	if(!(mnt->flag & Lmut) || agetl(&fs->rdonly))
		error(Erdonly);
	r = agetp(&mnt->root);
	if(r->nlbl != 1 || r->nref != 0) {
		r = updatesnap(r, mnt->name, mnt->flag);
		aswapp(&mnt->root, r);
	}
	btupsert(r, m, nm);
	mnt->nchange++;
	fs->snap.dirty = 1;
}

/*
 * Like upsert, for changes that need not reach the disk by themselves
 * (atime): they go out with the next commit that something else
 * causes, or at unmount.
 */
void
lazyupsert(Mount *mnt, Msg *m, int nm)
{
	Tree *r;

	if(!(mnt->flag & Lmut) || agetl(&fs->rdonly))
		error(Erdonly);
	r = agetp(&mnt->root);
	if(r->nlbl != 1 || r->nref != 0) {
		r = updatesnap(r, mnt->name, mnt->flag);
		aswapp(&mnt->root, r);
	}
	btupsert(r, m, nm);
	aswapl(&fs->lazydirty, 1);
}

/* --- snapshots, from 9front's snapfs and AOsnap --- */

static void
freetree(Bptr rb, vlong pred)
{
	Bptr bp;
	Blk *b;
	Kvp kv;
	int i;

	b = getblk(rb, 0);
	if(b->type == Tpivot){
		for(i = 0; i < b->nval; i++){
			getval(b, i, &kv);
			bp = unpackbp(kv.v, kv.nv);
			freetree(bp, pred);	/* leak b on error() */
			epochclean();
		}
	}
	if(rb.gen > pred)
		freeblk(nil, b);
	dropblk(b);
}

/*
 * Here, we clean epochs frequently, but we run outside of
 * an epoch; this is because the caller of this function
 * has already waited for an epoch to tick over, there's
 * nobody that can be accessing the tree other than us,
 * and we just need to keep the limbo list short.
 *
 * Because this is the last reference to the tree, we don't
 * need to hold the mutlk, other than when we free or kill
 * blocks via epochclean.
 */
static Tree*
sweeptree(Tree *t)
{
	char pfx[1];
	vlong gen;
	Scan s;
	Bptr bp;

	pfx[0] = Kdat;
	btnewscan(&s, pfx, 1);
	gen = (t->pred != -1) ? t->pred : t->base;
	btenter(t, &s);
	while(1){
		if(!btnext(&s, &s.kv))
			break;
		bp = unpackbp(s.kv.v, s.kv.nv);
		if(bp.gen > gen)
			freebp(nil, bp);
		epochclean();
	}
	btexit(&s);
	freetree(t->bp, gen);

	if(gen != -1 && (t = opentree(gen)) != nil){
		if(t->nlbl == 0 && t->nref == 0)
			return t;
		closesnap(t);
	}
	return nil;
}

/*
 * NetBSD: trees whose deletion the next commit makes durable, to sweep
 * after it (9front's AOsnap synced, then swept); on the mutator.
 */
typedef struct Sweep Sweep;
struct Sweep {
	Tree	*t;
	Sweep	*next;
};

void
pushsweep(Tree *t)
{
	Sweep *s;

	s = emalloc(sizeof(Sweep), 0);
	s->t = t;
	s->next = fs->sweepq;
	fs->sweepq = s;
}

/*
 * After a commit: sweep the queued trees, and delete their predecessors
 * that nothing holds any more, to sweep after the next commit.  The
 * frees reach the disk with the next commit, a lazy one at the latest.
 */
static void
sweeps(void)
{
	Sweep *s, *next;
	Tree *t, *n;
	int ok;

	s = fs->sweepq;
	fs->sweepq = nil;
	for(; s != nil; s = next){
		next = s->next;
		t = s->t;
		free(s);
		if(waserror()){
			fprint(2, "gefs: sweeping a deleted snapshot: %s\n", errmsg());
			continue;
		}
		n = sweeptree(t);	/* t leaked on error() */
		closesnap(t);
		ok = 0;
		if(n != nil && n->nref == 0 && n->nlbl == 0){
			qlock(&fs->mutlk);
			if(waserror()){
				qunlock(&fs->mutlk);
				nexterror();
			}
			ok = delsnap(n, n->succ, nil);
			qunlock(&fs->mutlk);
			poperror();
		}
		if(ok){
			pushsweep(n);
			fs->snap.dirty = 1;
		}else if(n != nil)
			closesnap(n);
		aswapl(&fs->lazydirty, 1);
		poperror();
	}
}

/*
 * Adds or removes a labelled snapshot; if the tree
 * needs to be cleaned up outside of the epoch, it
 * is returned, otherwise snapfs returns nil.
 */
Tree*
snapfs(char *old, char *new, int flag, int delete)
{
	Tree *t, *s, *r;
	Mount *mnt;

	t = nil;
	r = nil;
	for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->mnext){
		if(strcmp(old, mnt->name) != 0)
			continue;
		t = agetp(&mnt->root);
		if(mnt->flag & Lmut){
			t = updatesnap(t, mnt->name, mnt->flag);
			aswapp(&mnt->root, t);
		}
		aincl(&t->memref, 1);
		break;
	}
	if(t == nil && (t = opensnap(old, nil)) == nil)
		error(Enolabel);	/* NetBSD: was Eexist */
	if(waserror()){
		closesnap(t);
		nexterror();
	}
	if(delete){
		if(mnt != nil)
			error(Esnapu);
		if(delsnap(t, t->succ, old))
			r = t;
	}else{
		if((s = opensnap(new, nil)) != nil){
			closesnap(s);
			error(Esnapx);
		}
		tagsnap(t, new, flag);
	}
	poperror();
	if(r == nil)
		closesnap(t);
	fs->snap.dirty = 1;
	return r;
}

/*
 * Take snapshot new of label old (mutable with Lmut in flag), or delete
 * label old when new is nil, then commit.
 */
void
snapcmd(char *old, char *new, int flag)
{
	Tree *t, *n;
	int ok;

	if(agetl(&fs->rdonly))
		error(Erdonly);
	qlock(&fs->mutlk);
	resvbegin();	/* deleting frees; a snapshot takes little */
	if(waserror()){
		resvend();
		qunlock(&fs->mutlk);
		nexterror();
	}
	t = snapfs(old, new, flag, new == nil);
	resvend();
	qunlock(&fs->mutlk);
	poperror();

	commit(0);
	while(t != nil){
		n = sweeptree(t);
		closesnap(t);
		if(n == nil)
			break;
		t = n;
		ok = 0;
		if(n->nref == 0 && n->nlbl == 0){
			qlock(&fs->mutlk);
			ok = delsnap(n, n->succ, nil);
			qunlock(&fs->mutlk);
		}
		if(!ok){
			closesnap(n);
			break;
		}
		fs->snap.dirty = 1;
		commit(0);
	}
}

/*
 * Remove the data blocks of qpath from off to end, a batch per upsert;
 * 9front's AOclear, which its sweeper proc ran.
 */
void
clearblocks(Mount *mnt, vlong qpath, vlong off, vlong end)
{
	struct {
		char	buf[Kvmax];
		Msg	mb[Kvmax/Offksz];
	} *cb;		/* 2.6 KiB: not on a kernel stack */
	char *buf;
	Msg *mb;
	int nm;

	if(agetl(&fs->rdonly))
		error(Erdonly);
	cb = emalloc(sizeof(*cb), 0);
	if(waserror()){
		free(cb);
		nexterror();
	}
	buf = cb->buf;
	mb = cb->mb;
	nm = 0;
	off &= ~(Blksz-1);
	for(; off < end; off += Blksz){
		mb[nm].op = Oclearb;
		mb[nm].k = buf + Offksz * nm;
		mb[nm].nk = Offksz;
		mb[nm].k[0] = Kdat;
		PACK64(mb[nm].k+1, qpath);
		PACK64(mb[nm].k+9, off);
		mb[nm].v = nil;
		mb[nm].nv = 0;
		if(++nm >= Kvmax/Offksz || off + Blksz >= end){
			qlock(&fs->mutlk);
			resvbegin();
			if(waserror()){
				resvend();
				qunlock(&fs->mutlk);
				nexterror();
			}
			upsert(mnt, mb, nm);
			resvend();
			qunlock(&fs->mutlk);
			poperror();
			epochclean();
			nm = 0;
			/*
			 * The blocks cleared are free after a commit; a big
			 * removal on a nearly full file system commits on the
			 * way rather than run out.
			 */
			if(lowspace()){
				commit(0);
			}
		}
	}
	poperror();
	free(cb);
}
