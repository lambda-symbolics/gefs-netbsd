/*
 * POSIX operations on gefs for the NetBSD port: the parts of 9front's
 * fs.c that are not 9P (walk, create, remove, wstat, read, write), made
 * to POSIX rules.  See ops.h and doc/DESIGN.org.
 *
 * Locking follows 9front: tree reads run in an epoch, changes run with
 * fs->mutlk held, epochclean() runs outside both.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"

/* --- directory entries in memory, from fs.c --- */

Dent*
getdent(Mount *mnt, vlong pqid, Xdir *d)
{
	Dent *de;
	char *e;
	u32int h;

	h = ihash(d->qid.path) % Ndtab;
	lock(&mnt->dtablk);
	for(de = mnt->dtab[h]; de != nil; de = de->next){
		if(de->qid.path == d->qid.path){
			aincl(&de->ref, 1);
			goto Out;
		}
	}
	if(waserror()){
		unlock(&mnt->dtablk);
		nexterror();
	}
	de = emalloc(sizeof(Dent), 1);
	de->Xdir = *d;
	aswapl(&de->ref, 1);
	de->up = pqid;
	de->qid = d->qid;
	de->length = d->length;
	de->truncrz.l = &fs->mutlk;
	de->nlink = 1;
	e = packdkey(de->buf, sizeof(de->buf), pqid, d->name);
	de->k = de->buf;
	de->nk = e - de->buf;
	de->name = de->buf + 11;
	de->next = mnt->dtab[h];
	mnt->dtab[h] = de;
	poperror();
Out:
	unlock(&mnt->dtablk);
	return de;
}

void
clunkdent(Mount *mnt, Dent *de)
{
	Dent *e, **pe;
	u32int h;

	if(de == nil)
		return;
	lock(&mnt->dtablk);
	if(aincl(&de->ref, -1) != 0)
		goto Out;
	h = ihash(de->qid.path) % Ndtab;
	pe = &mnt->dtab[h];
	for(e = mnt->dtab[h]; e != nil; e = e->next){
		if(e == de)
			break;
		pe = &e->next;
	}
	assert(e != nil);
	*pe = e->next;
	rwlockfini(&de->RWLock);
	rendezfini(&de->truncrz);
	free(de);
Out:
	unlock(&mnt->dtablk);
}

/* --- keys --- */

static void
dkey(Key *k, vlong up, char *name, char *buf, int nbuf)
{
	char *p;

	p = packdkey(buf, nbuf, up, name);
	k->k = buf;
	k->nk = p - buf;
}

static void
xkey(Key *k, vlong qid, char *name, char *buf, int nbuf)
{
	int n;

	n = strlen(name);
	assert(nbuf >= 9 + n);
	buf[0] = Kxattr;
	PACK64(buf+1, qid);
	memcpy(buf+9, name, n);
	k->k = buf;
	k->nk = 9 + n;
}

/* the name as a C string, checked the POSIX way */
static char*
cname(char *buf, char *name, int n)
{
	int i;

	if(n <= 0)
		error(Ename);
	if(n > Maxname)
		error(Elength);
	for(i = 0; i < n; i++)
		if(name[i] == 0 || name[i] == '/')
			error(Ename);
	memmove(buf, name, n);
	buf[n] = 0;
	if(strcmp(buf, ".") == 0 || strcmp(buf, "..") == 0)
		error(Ename);
	return buf;
}

/*
 * The directory entry's attributes: the device of a device node, the
 * shared record of a file with several names.
 */
static void
loadextra(Tree *t, Dent *de)
{
	char kbuf[Keymax], vbuf[Kvmax];
	Kvp kv;
	Key k;

	if((de->Xdir.flag & GEFS_FDEVNOD) && de->devt == 0){
		xkey(&k, de->qid.path, Xrdev, kbuf, sizeof(kbuf));
		if(btlookup(t, &k, &kv, vbuf, sizeof(vbuf)) && kv.nv == 12){
			de->devt = UNPACK32(kv.v);
			de->rdev = UNPACK64(kv.v+4);
		}
	}
}

static Dent*
entdent(Mount *mnt, Tree *t, vlong up, Kvp *kv)
{
	Dent *de;
	Xdir d;

	kv2dir(kv, &d);
	de = getdent(mnt, up, &d);
	if(waserror()){
		clunkdent(mnt, de);
		nexterror();
	}
	loadextra(t, de);
	poperror();
	return de;
}

/* --- lookups (in an epoch) --- */

Dent*
gefsroot(Mount *mnt)
{
	char kbuf[Keymax], kvbuf[Kvmax];
	Dent *de;
	Tree *t;
	Kvp kv;
	Key k;

	epochstart(0);
	if(waserror()){
		epochend(0);
		nexterror();
	}
	t = agetp(&mnt->root);
	dkey(&k, -1, "", kbuf, sizeof(kbuf));
	if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf)))
		error(Esrch);
	de = entdent(mnt, t, -1, &kv);
	poperror();
	epochend(0);
	return de;
}

static Dent*
lookup1(Mount *mnt, Tree *t, vlong up, char *name)
{
	char kbuf[Keymax], kvbuf[Kvmax];
	Kvp kv;
	Key k;

	dkey(&k, up, name, kbuf, sizeof(kbuf));
	if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf)))
		return nil;
	return entdent(mnt, t, up, &kv);
}

/* the parent of a directory, through its Kup key */
static Dent*
parent(Mount *mnt, Tree *t, Dent *dir)
{
	char kbuf[Keymax], kvbuf[Kvmax], *name;
	vlong up;
	Kvp kv;
	Key k;

	if(dir->up == -1){
		aincl(&dir->ref, 1);
		return dir;
	}
	/* Kup(dir->up) holds the key of the parent's own entry */
	kbuf[0] = Kup;
	PACK64(kbuf+1, dir->up);
	k.k = kbuf;
	k.nk = Upksz;
	if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf)))
		error(Esrch);
	name = unpackdkey(kv.v, kv.nv, &up);
	return lookup1(mnt, t, up, name);
}

Dent*
gefslookup(Mount *mnt, Dent *dir, char *name, int n)
{
	char buf[Maxname+1];
	Dent *de;
	Tree *t;

	epochstart(0);
	if(waserror()){
		epochend(0);
		nexterror();
	}
	t = agetp(&mnt->root);
	if(n == 2 && name[0] == '.' && name[1] == '.')
		de = parent(mnt, t, dir);
	else
		de = lookup1(mnt, t, dir->qid.path, cname(buf, name, n));
	poperror();
	epochend(0);
	return de;
}

/* is there anything in directory dir? */
int
gefsisempty(Mount *mnt, Dent *dir)
{
	char pfx[Dpfxsz];
	Tree *t;
	Scan s;
	int e;

	t = agetp(&mnt->root);
	packdkey(pfx, sizeof(pfx), dir->qid.path, nil);
	btnewscan(&s, pfx, sizeof(pfx));
	btenter(t, &s);
	e = !btnext(&s, &s.kv);
	btexit(&s);
	return e;
}

/*
 * Is a an ancestor of (or the same directory as) d?  For rename, which
 * must not move a directory below itself.
 */
int
gefsancestor(Mount *mnt, Dent *a, Dent *d)
{
	char kbuf[Keymax], kvbuf[Kvmax];
	vlong q;
	Tree *t;
	Kvp kv;
	Key k;
	int r;

	epochstart(0);
	if(waserror()){
		epochend(0);
		nexterror();
	}
	t = agetp(&mnt->root);
	r = 0;
	q = d->qid.path;
	while(1){
		if(q == a->qid.path){
			r = 1;
			break;
		}
		kbuf[0] = Kup;
		PACK64(kbuf+1, q);
		k.k = kbuf;
		k.nk = Upksz;
		if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf)))
			break;
		unpackdkey(kv.v, kv.nv, &q);
		if(q == -1)
			break;
	}
	poperror();
	epochend(0);
	return r;
}

/* --- attributes --- */

int
gefsxtype(Xdir *d)
{
	if(d->mode & DMDIR)
		return Gdir;
	if(d->flag & GEFS_FSYMLINK)
		return Glnk;
	if(d->flag & GEFS_FDEVNOD){
		switch(d->devt){
		case GEFS_DEVBLK:	return Gblk;
		case GEFS_DEVCHR:	return Gchr;
		case GEFS_DEVSOCK:	return Gsock;
		case GEFS_DEVFIFO:	return Gfifo;
		}
		return Greg;
	}
	return Greg;
}

static ulong
mode2perm(ulong mode)
{
	ulong p;

	p = mode & 0777;
	if(mode & GEFS_DMSETUID)
		p |= 04000;
	if(mode & GEFS_DMSETGID)
		p |= 02000;
	if(mode & GEFS_DMSETVTX)
		p |= 01000;
	return p;
}

static ulong
perm2mode(ulong perm)
{
	ulong m;

	m = perm & 0777;
	if(perm & 04000)
		m |= GEFS_DMSETUID;
	if(perm & 02000)
		m |= GEFS_DMSETGID;
	if(perm & 01000)
		m |= GEFS_DMSETVTX;
	return m;
}

void
gefsgetattr(Dent *de, Gattr *a)
{
	rlock(de);
	memset(a, 0, sizeof(*a));
	a->type = gefsxtype(&de->Xdir);
	a->perm = mode2perm(de->mode);
	if(de->mode & DMAPPEND)
		a->flags |= Gappend;
	if(de->mode & DMTMP)
		a->flags |= Gnodump;
	a->uid = de->uid;
	a->gid = de->gid;
	a->size = (a->type == Gdir) ? 0 : de->length;
	a->atime = de->atime;
	a->mtime = de->mtime;
	a->qid = de->qid.path;
	a->vers = de->qid.vers;
	a->nlink = (a->type == Gdir) ? 1 : de->nlink;
	a->devt = de->devt;
	a->rdev = de->rdev;
	runlock(de);
}

/* a directory's contents changed: new version and mtime */
static void
touch(Dent *de, Msg *msg, char *buf, vlong now)
{
	wlock(de);
	de->qid.vers++;
	de->mtime = now;
	buf[0] = Owmtime;
	PACK64(buf+1, now);
	msg->op = Owstat;
	msg->k = de->k;
	msg->nk = de->nk;
	msg->v = buf;
	msg->nv = 9;
	wunlock(de);
}

/*
 * Set the attributes in mask; Asize truncates or extends.  uid is the
 * caller, recorded as muid.
 */
void
gefssetattr(Mount *mnt, Dent *de, Gattr *a, int mask, int uid)
{
	char buf[Kvmax], *p;
	Msg m;
	int op;
	ulong mode;

	if(mask & Asize)
		gefstrunc(mnt, de, a->size, uid);
	mask &= ~Asize;
	if(mask == 0)
		return;
	qlock(&fs->mutlk);
	wlock(de);
	if(waserror()){
		wunlock(de);
		qunlock(&fs->mutlk);
		nexterror();
	}
	p = buf+1;
	op = 0;
	mode = de->mode;
	if(mask & Aperm)
		mode = (mode & ~(0777|GEFS_DMSETUID|GEFS_DMSETGID|GEFS_DMSETVTX))
			| perm2mode(a->perm);
	if(mask & Aflags){
		mode &= ~(DMAPPEND|DMTMP);
		if(a->flags & Gappend)
			mode |= DMAPPEND;
		if(a->flags & Gnodump)
			mode |= DMTMP;
	}
	if(mode != de->mode){
		op |= Owmode;
		PACK32(p, mode);
		p += 4;
	}
	if(mask & Amtime){
		op |= Owmtime;
		PACK64(p, a->mtime);
		p += 8;
	}
	if(mask & Aatime){
		op |= Owatime;
		PACK64(p, a->atime);
		p += 8;
	}
	if(mask & Auid){
		op |= Owuid;
		PACK32(p, a->uid);
		p += 4;
	}
	if(mask & Agid){
		op |= Owgid;
		PACK32(p, a->gid);
		p += 4;
	}
	if(op != 0){
		op |= Owmuid;
		PACK32(p, uid);
		p += 4;
		buf[0] = op;
		m.op = Owstat;
		m.k = de->k;
		m.nk = de->nk;
		m.v = buf;
		m.nv = p - buf;
		/* atime alone goes out with the next real commit */
		if(op == (Owatime|Owmuid))
			lazyupsert(mnt, &m, 1);
		else
			upsert(mnt, &m, 1);
		de->qid.vers++;
		if(op & Owmode){
			de->mode = mode;
			de->qid.type = mode>>24;
		}
		if(op & Owmtime)
			de->mtime = a->mtime;
		if(op & Owatime)
			de->atime = a->atime;
		if(op & Owuid)
			de->uid = a->uid;
		if(op & Owgid)
			de->gid = a->gid;
		de->muid = uid;
	}
	poperror();
	wunlock(de);
	qunlock(&fs->mutlk);
	epochclean();
}

/* --- file data, from fs.c's readb and writeb --- */

static int
readb(Tree *t, vlong qpath, char *d, vlong o, vlong n, vlong sz)
{
	char buf[Offksz], kvbuf[Offksz+32];
	vlong fb, fo;
	Bptr bp;
	Blk *b;
	Key k;
	Kvp kv;

	if(o >= sz)
		return 0;

	fb = o & ~(Blksz-1);
	fo = o & (Blksz-1);
	if(fo+n > Blksz)
		n = Blksz-fo;
	if(o+n > sz)
		n = sz-o;

	k.k = buf;
	k.nk = sizeof(buf);
	k.k[0] = Kdat;
	PACK64(k.k+1, qpath);
	PACK64(k.k+9, fb);

	if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf))){
		memset(d, 0, n);
		return n;
	}

	bp = unpackbp(kv.v, kv.nv);
	b = getblk(bp, GBraw);
	memcpy(d, b->buf+fo, n);
	dropblk(b);
	return n;
}

/*
 * One data block at file offset o: a new block with the old contents
 * where the write does not cover it, queued for writing; m is set up to
 * insert its pointer.  Returns the bytes taken from s.
 */
static int
writeb(Mount *mnt, vlong qpath, Msg *m, Bptr *ret, char *s, vlong o, vlong n, vlong sz)
{
	char buf[Kvmax];
	vlong fb, fo;
	Blk *b, *t;
	int seq;
	Tree *r;
	Bptr bp;
	Kvp kv;

	fb = o & ~(Blksz-1);
	fo = o & (Blksz-1);

	m->k[0] = Kdat;
	PACK64(m->k+1, qpath);
	PACK64(m->k+9, fb);

	if(fo+n >= Blksz)
		seq = 1;
	else
		seq = 0;
	r = agetp(&mnt->root);
	b = newdblk(r, qpath, seq);
	if(waserror()){
		freeblk(r, b);
		dropblk(b);
		nexterror();
	}
	t = nil;
	r = agetp(&mnt->root);
	if(btlookup(r, m, &kv, buf, sizeof(buf))){
		bp = unpackbp(kv.v, kv.nv);
		if(fb < sz && (fo != 0 || n < Blksz)){
			t = getblk(bp, GBraw);
			memcpy(b->buf, t->buf, Blksz);
			dropblk(t);
		}
	}
	if(fo+n > Blksz)
		n = Blksz-fo;
	memcpy(b->buf+fo, s, n);
	if(t == nil){
		if(fo > 0)
			memset(b->buf, 0, fo);
		if(fo+n < Blksz)
			memset(b->buf+fo+n, 0, Blksz-fo-n);
	}
	enqueue(b);
	poperror();

	packbp(m->v, m->nv, &b->bp);
	*ret = b->bp;
	dropblk(b);

	return n;
}

long
gefsread(Mount *mnt, Dent *de, char *buf, vlong off, long n)
{
	vlong len, qpath;
	long c, r;
	Tree *t;

	if(off < 0)
		error(Eoffset);
	rlock(de);
	len = de->length;
	qpath = de->qid.path;
	runlock(de);
	if(off >= len)
		return 0;
	if(off + n > len)
		n = len - off;
	epochstart(0);
	if(waserror()){
		epochend(0);
		nexterror();
	}
	t = agetp(&mnt->root);
	for(c = 0; c < n; c += r){
		r = readb(t, qpath, buf+c, off+c, n-c, len);
		if(r <= 0)
			break;
	}
	poperror();
	epochend(0);
	return c;
}

enum {
	Wbatch	= 32,	/* data blocks per upsert: 512 KiB */
};

/*
 * Write n bytes at off, in batches of Wbatch blocks: the new data
 * pointers, the size and mtime go into the tree together.  With newsize
 * >= 0 the file's size becomes newsize instead of growing to cover the
 * write (for the page cache, which knows the size better).
 */
long
gefswrite(Mount *mnt, Dent *de, char *s, vlong off, long n, vlong newsize, int uid)
{
	char sbuf[Wstatmax], kbuf[Wbatch][Offksz], vbuf[Wbatch][Ptrsz];
	Bptr bp[Wbatch];
	Msg kv[Wbatch+1];
	vlong o, len;
	long c, w, x;
	int i, j;
	char *p;
	Tree *t;

	if(off < 0 || off >= (1LL<<62) - n)
		error(Ewstatl);
	if(agetl(&fs->rdonly))
		error(Erdonly);
	w = 0;
	o = off;
	c = n;
	while(c > 0 || (w == 0 && newsize >= 0)){
		qlock(&fs->mutlk);
		wlock(de);
		if(waserror()){
			wunlock(de);
			qunlock(&fs->mutlk);
			nexterror();
		}
		if(de->mode & DMAPPEND)
			o = de->length;
		t = agetp(&mnt->root);
		len = de->length;
		for(i = 0; c > 0 && i < Wbatch; i++){
			kv[i].op = Oinsert;
			kv[i].k = kbuf[i];
			kv[i].nk = sizeof(kbuf[i]);
			kv[i].v = vbuf[i];
			kv[i].nv = sizeof(vbuf[i]);
			if(waserror()){
				if(!agetl(&fs->rdonly))
					for(j = 0; j < i; j++)
						freebp(t, bp[j]);
				nexterror();
			}
			x = writeb(mnt, de->qid.path, &kv[i], &bp[i], s+w, o, c, len);
			poperror();
			w += x;
			o += x;
			c -= x;
		}
		p = sbuf;
		kv[i].op = Owstat;
		kv[i].k = de->k;
		kv[i].nk = de->nk;
		*p++ = 0;
		if(newsize >= 0 && c == 0){
			if(newsize != de->length){
				sbuf[0] |= Owsize;
				PACK64(p, newsize);
				p += 8;
				de->length = newsize;
			}
		}else if(o > de->length){
			sbuf[0] |= Owsize;
			PACK64(p, o);
			p += 8;
			de->length = o;
		}
		sbuf[0] |= Owmtime;
		de->mtime = nsec();
		PACK64(p, de->mtime);
		p += 8;
		sbuf[0] |= Owmuid;
		PACK32(p, uid);
		p += 4;
		de->muid = uid;
		de->qid.vers++;
		kv[i].v = sbuf;
		kv[i].nv = p - sbuf;
		upsert(mnt, kv, i+1);
		poperror();
		wunlock(de);
		qunlock(&fs->mutlk);
		epochclean();
		if(c == 0)
			break;
	}
	return w;
}

/*
 * Change the size.  Shrinking zeroes the tail of the last block and
 * frees the blocks after it.  (9front's wstat queues Oclearb messages at
 * the unaligned new length, which match no key: the blocks after it
 * stayed, and came back when the file grew.)
 */
void
gefstrunc(Mount *mnt, Dent *de, vlong len, int uid)
{
	char sbuf[Wstatmax], kbuf[Offksz], vbuf[Ptrsz], *p, *zero;
	vlong old, tail;
	Msg m[2];
	Bptr bp;
	Tree *t;
	int nm;

	if(len < 0)
		error(Ewstatl);
	if(de->mode & DMDIR)
		error(Ewstatl);
	qlock(&fs->mutlk);
	wlock(de);
	if(waserror()){
		wunlock(de);
		qunlock(&fs->mutlk);
		nexterror();
	}
	old = de->length;
	nm = 0;
	if(len < old && (len & (Blksz-1)) != 0){
		/* rewrite the new last block with zeros after the end */
		tail = Blksz - (len & (Blksz-1));
		zero = emalloc(tail, 1);
		if(waserror()){
			free(zero);
			nexterror();
		}
		m[nm].op = Oinsert;
		m[nm].k = kbuf;
		m[nm].nk = sizeof(kbuf);
		m[nm].v = vbuf;
		m[nm].nv = sizeof(vbuf);
		t = agetp(&mnt->root);
		USED(t);
		writeb(mnt, de->qid.path, &m[nm], &bp, zero, len, tail, old);
		nm++;
		poperror();
		free(zero);
	}
	p = sbuf;
	*p++ = Owsize|Owmtime|Owmuid;
	PACK64(p, len);		p += 8;
	de->mtime = nsec();
	PACK64(p, de->mtime);	p += 8;
	PACK32(p, uid);		p += 4;
	m[nm].op = Owstat;
	m[nm].k = de->k;
	m[nm].nk = de->nk;
	m[nm].v = sbuf;
	m[nm].nv = p - sbuf;
	nm++;
	upsert(mnt, m, nm);
	de->length = len;
	de->qid.vers++;
	de->muid = uid;
	poperror();
	wunlock(de);
	qunlock(&fs->mutlk);
	epochclean();
	if(len < old)
		clearblocks(mnt, de->qid.path, (len + Blksz-1) & ~(Blksz-1), old);
}

/* --- creating and removing --- */

/*
 * Create name in dir, with the type, permissions and owner in a; link
 * holds a symlink's target (nlink bytes).
 */
Dent*
gefscreate(Mount *mnt, Dent *dir, char *name, int n, Gattr *a, char *link, int nlink)
{
	char nbuf[Maxname+1], buf[Kvmax], upkbuf[Keymax], upvbuf[Inlmax];
	char tbuf[16], xkbuf[Keymax], xvbuf[12], dkbuf[Offksz], dvbuf[Ptrsz];
	Msg mb[6];
	Dent *de;
	Bptr bp;
	Xdir d;
	Tree *t;
	int nm;
	char *p;
	vlong now;

	cname(nbuf, name, n);
	if(a->type == Glnk && (nlink <= 0 || nlink > Blksz))
		error(Elength);
	if(agetl(&fs->rdonly))
		error(Erdonly);
	epochstart(0);
	qlock(&fs->mutlk);
	if(waserror()){
		qunlock(&fs->mutlk);
		epochend(0);
		nexterror();
	}
	t = agetp(&mnt->root);
	if((de = lookup1(mnt, t, dir->qid.path, nbuf)) != nil){
		clunkdent(mnt, de);
		error(Eexist);
	}
	rlock(dir);
	if(dir->gone || dir->orphan){
		runlock(dir);
		error(Ephase);
	}
	if((dir->mode & DMDIR) == 0){
		runlock(dir);
		error(Ecdir);
	}
	runlock(dir);
	if(fs->nextqid >= Qdump)
		error(Enoqid);

	now = nsec();
	memset(&d, 0, sizeof(d));
	d.flag = 0;
	d.qid.type = QTFILE;
	d.mode = perm2mode(a->perm);
	switch(a->type){
	case Gdir:
		d.qid.type = QTDIR;
		d.mode |= DMDIR;
		break;
	case Glnk:
		d.flag |= GEFS_FSYMLINK;
		d.length = nlink;
		break;
	case Gchr:
	case Gblk:
	case Gfifo:
	case Gsock:
		d.flag |= GEFS_FDEVNOD|GEFS_FXATTR;
		d.devt = (a->type == Gchr) ? GEFS_DEVCHR
			: (a->type == Gblk) ? GEFS_DEVBLK
			: (a->type == Gfifo) ? GEFS_DEVFIFO : GEFS_DEVSOCK;
		d.rdev = a->rdev;
		break;
	case Greg:
		break;
	default:
		error(Emode);
	}
	if(a->flags & Gappend){
		d.mode |= DMAPPEND;
		d.qid.type |= QTAPPEND;
	}
	if(a->flags & Gnodump){
		d.mode |= DMTMP;
		d.qid.type |= QTTMP;
	}
	d.qid.path = fs->nextqid++;
	d.qid.vers = 0;
	d.name = nbuf;
	d.atime = now;
	d.mtime = now;
	d.uid = a->uid;
	d.gid = a->gid;
	d.muid = a->uid;

	nm = 0;
	mb[nm].op = Oinsert;
	dir2kv(dir->qid.path, &d, &mb[nm], buf, sizeof(buf));
	nm++;
	if(d.mode & DMDIR){
		mb[nm].op = Oinsert;
		p = packsuper(upkbuf, sizeof(upkbuf), d.qid.path);
		mb[nm].k = upkbuf;
		mb[nm].nk = p - upkbuf;
		p = packdkey(upvbuf, sizeof(upvbuf), dir->qid.path, d.name);
		mb[nm].v = upvbuf;
		mb[nm].nv = p - upvbuf;
		nm++;
	}
	if(d.flag & GEFS_FDEVNOD){
		mb[nm].op = Oinsert;
		xkey(&mb[nm], d.qid.path, Xrdev, xkbuf, sizeof(xkbuf));
		PACK32(xvbuf, d.devt);
		PACK64(xvbuf+4, d.rdev);
		mb[nm].v = xvbuf;
		mb[nm].nv = 12;
		nm++;
	}
	if(d.flag & GEFS_FSYMLINK){
		mb[nm].op = Oinsert;
		mb[nm].k = dkbuf;
		mb[nm].nk = sizeof(dkbuf);
		mb[nm].v = dvbuf;
		mb[nm].nv = sizeof(dvbuf);
		writeb(mnt, d.qid.path, &mb[nm], &bp, link, 0, nlink, 0);
		nm++;
	}
	touch(dir, &mb[nm++], tbuf, now);
	assert(nm <= nelem(mb));
	upsert(mnt, mb, nm);

	de = getdent(mnt, dir->qid.path, &d);
	de->devt = d.devt;
	de->rdev = d.rdev;
	poperror();
	qunlock(&fs->mutlk);
	epochend(0);
	epochclean();
	return de;
}

/*
 * Remove name from dir; de is what it names.  A directory must be
 * empty and goes at once.  A file's entry moves to the orphanage
 * (Qorphan), because POSIX lets it stay open: gefsrelease frees its data
 * when the last user is done, and a mount after a crash frees whatever
 * is still there.
 */
void
gefsremove(Mount *mnt, Dent *dir, char *name, int n, Dent *de)
{
	char nbuf[Maxname+1], kbuf[Keymax], kvbuf[Kvmax], upbuf[Upksz], tbuf[16];
	char obuf[Kvmax];
	Msg mb[4];
	Tree *t;
	Kvp kv;
	Key k;
	Xdir d;
	int nm;

	cname(nbuf, name, n);
	if(agetl(&fs->rdonly))
		error(Erdonly);
	epochstart(0);
	qlock(&fs->mutlk);
	wlock(de);
	if(waserror()){
		wunlock(de);
		qunlock(&fs->mutlk);
		epochend(0);
		nexterror();
	}
	if(de->gone || de->orphan)
		error(Ephase);
	t = agetp(&mnt->root);
	dkey(&k, dir->qid.path, nbuf, kbuf, sizeof(kbuf));
	if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf)))
		error(Esrch);
	kv2dir(&kv, &d);
	if(d.qid.path != de->qid.path)
		error(Ephase);
	if((de->mode & DMDIR) && !gefsisempty(mnt, de))
		error(Enempty);

	nm = 0;
	mb[nm].op = Odelete;
	mb[nm].k = k.k;
	mb[nm].nk = k.nk;
	mb[nm].v = nil;
	mb[nm].nv = 0;
	nm++;
	if(de->mode & DMDIR){
		packsuper(upbuf, sizeof(upbuf), de->qid.path);
		mb[nm].op = Oclobber;
		mb[nm].k = upbuf;
		mb[nm].nk = Upksz;
		mb[nm].v = nil;
		mb[nm].nv = 0;
		nm++;
	}else{
		snprint(de->oname, sizeof(de->oname), "%016llux", de->qid.path);
		d = de->Xdir;
		d.name = de->oname;
		mb[nm].op = Oinsert;
		dir2kv(Qorphan, &d, &mb[nm], obuf, sizeof(obuf));
		nm++;
	}
	touch(dir, &mb[nm++], tbuf, nsec());
	assert(nm <= nelem(mb));
	upsert(mnt, mb, nm);
	if(de->mode & DMDIR)
		de->gone = 1;
	else{
		/* the entry now lives in the orphanage; follow it there */
		de->orphan = 1;
		de->nlink = 0;
		de->up = Qorphan;
		dkey(&k, Qorphan, de->oname, kbuf, sizeof(kbuf));
		cpkey(de, &k, de->buf, sizeof(de->buf));
		de->name = de->buf + 11;
	}
	poperror();
	wunlock(de);
	qunlock(&fs->mutlk);
	epochend(0);
	epochclean();
}

/* the last reference to a removed file is gone: free its data */
void
gefsrelease(Mount *mnt, Dent *de)
{
	char kbuf[Keymax], xkbuf[Keymax];
	Msg mb[2];
	Key k;
	int nm;

	if(!de->orphan || agetl(&fs->rdonly))
		return;
	clearblocks(mnt, de->qid.path, 0, de->length);
	qlock(&fs->mutlk);
	if(waserror()){
		qunlock(&fs->mutlk);
		nexterror();
	}
	nm = 0;
	dkey(&k, Qorphan, de->oname, kbuf, sizeof(kbuf));
	mb[nm].op = Odelete;
	mb[nm].k = k.k;
	mb[nm].nk = k.nk;
	mb[nm].v = nil;
	mb[nm].nv = 0;
	nm++;
	if(de->Xdir.flag & GEFS_FDEVNOD){
		mb[nm].op = Oclobber;
		xkey(&mb[nm], de->qid.path, Xrdev, xkbuf, sizeof(xkbuf));
		mb[nm].v = nil;
		mb[nm].nv = 0;
		nm++;
	}
	upsert(mnt, mb, nm);
	de->orphan = 0;
	de->gone = 1;
	poperror();
	qunlock(&fs->mutlk);
	epochclean();
}

/* at mount: finish removals that a crash interrupted */
void
gefsorphans(Mount *mnt)
{
	char pfx[Dpfxsz];
	Dent *de;
	Tree *t;
	Scan s;
	Xdir d;
	int n;

	if(agetl(&fs->rdonly) || !(mnt->flag & Lmut))
		return;
	for(n = 0;; n++){
		epochstart(0);
		if(waserror()){
			epochend(0);
			nexterror();
		}
		t = agetp(&mnt->root);
		packdkey(pfx, sizeof(pfx), Qorphan, nil);
		btnewscan(&s, pfx, sizeof(pfx));
		btenter(t, &s);
		de = nil;
		if(btnext(&s, &s.kv)){
			kv2dir(&s.kv, &d);
			de = getdent(mnt, Qorphan, &d);
			snprint(de->oname, sizeof(de->oname), "%s", d.name);
			de->orphan = 1;
		}
		btexit(&s);
		poperror();
		epochend(0);
		if(de == nil)
			break;
		if(waserror()){
			clunkdent(mnt, de);
			nexterror();
		}
		gefsrelease(mnt, de);
		poperror();
		clunkdent(mnt, de);
	}
	if(n > 0)
		fprint(2, "gefs: freed %d files removed before a crash\n", n);
}

/*
 * Rename de, named fname in fdir, to tname in tdir.  If tname exists it
 * is tde, and is replaced: a file goes to the orphanage as in
 * gefsremove, a directory must be empty.  All of it is one upsert.
 */
void
gefsrename(Mount *mnt, Dent *fdir, Dent *de, Dent *tdir, char *tname, int n, Dent *tde)
{
	char nbuf[Maxname+1], rnbuf[Kvmax], upbuf[Upksz], tupbuf[Upksz], obuf[Kvmax];
	char t1[16], t2[16], okbuf[Keymax], kbuf[Keymax];
	Msg mb[7];
	Xdir nd, od;
	vlong now;
	Key k;
	int nm;

	cname(nbuf, tname, n);
	if(agetl(&fs->rdonly))
		error(Erdonly);
	epochstart(0);
	qlock(&fs->mutlk);
	wlock(de);
	if(tde != nil && tde != de)
		wlock(tde);
	if(waserror()){
		if(tde != nil && tde != de)
			wunlock(tde);
		wunlock(de);
		qunlock(&fs->mutlk);
		epochend(0);
		nexterror();
	}
	if(de->gone || de->orphan)
		error(Ephase);
	if(tde == de)
		goto Done;
	if(tde != nil){
		if(tde->gone || tde->orphan)
			error(Ephase);
		if(tde->mode & DMDIR){
			if(!(de->mode & DMDIR))
				error(Edir);
			if(!gefsisempty(mnt, tde))
				error(Enempty);
		}else if(de->mode & DMDIR)
			error(Edir);
	}
	now = nsec();
	nm = 0;
	/* the old name */
	mb[nm].op = Oclobber;
	mb[nm].Key = de->Key;
	mb[nm].v = nil;
	mb[nm].nv = 0;
	nm++;
	/* the new one, replacing tde's entry if there is one */
	nd = de->Xdir;
	nd.name = nbuf;
	nd.qid.vers++;
	mb[nm].op = Oinsert;
	dir2kv(tdir->qid.path, &nd, &mb[nm], rnbuf, sizeof(rnbuf));
	k = mb[nm].Key;
	nm++;
	if(de->mode & DMDIR){
		packsuper(upbuf, sizeof(upbuf), de->qid.path);
		mb[nm].op = Oinsert;
		mb[nm].k = upbuf;
		mb[nm].nk = Upksz;
		mb[nm].v = k.k;
		mb[nm].nv = k.nk;
		nm++;
	}
	if(tde != nil){
		if(tde->mode & DMDIR){
			packsuper(tupbuf, sizeof(tupbuf), tde->qid.path);
			mb[nm].op = Oclobber;
			mb[nm].k = tupbuf;
			mb[nm].nk = Upksz;
			mb[nm].v = nil;
			mb[nm].nv = 0;
			nm++;
		}else{
			snprint(tde->oname, sizeof(tde->oname), "%016llux", tde->qid.path);
			od = tde->Xdir;
			od.name = tde->oname;
			mb[nm].op = Oinsert;
			dir2kv(Qorphan, &od, &mb[nm], obuf, sizeof(obuf));
			nm++;
		}
	}
	touch(fdir, &mb[nm++], t1, now);
	if(tdir != fdir)
		touch(tdir, &mb[nm++], t2, now);
	assert(nm <= nelem(mb));
	upsert(mnt, mb, nm);

	/* the entries in memory */
	de->Xdir = nd;
	de->up = tdir->qid.path;
	cpkey(de, &k, de->buf, sizeof(de->buf));
	de->name = de->buf + 11;
	if(tde != nil){
		if(tde->mode & DMDIR)
			tde->gone = 1;
		else{
			tde->orphan = 1;
			tde->nlink = 0;
			tde->up = Qorphan;
			dkey(&k, Qorphan, tde->oname, okbuf, sizeof(okbuf));
			cpkey(tde, &k, tde->buf, sizeof(tde->buf));
			tde->name = tde->buf + 11;
		}
	}
	USED(kbuf);
Done:
	poperror();
	if(tde != nil && tde != de)
		wunlock(tde);
	wunlock(de);
	qunlock(&fs->mutlk);
	epochend(0);
	epochclean();
}

/* --- directories --- */

/*
 * Call fn for the entries of dir after where scan s stopped, until fn
 * returns -1 (no room: that entry is given again next time) or the
 * directory ends (returns 0; 1 when stopped early).  A new scan must
 * have s->first set, as btnewscan leaves it.
 */
int
gefsreaddir(Mount *mnt, Dent *dir, Scan *s, int (*fn)(void*, Xdir*), void *arg)
{
	char pfx[Dpfxsz];
	int stopped;
	Tree *t;
	Xdir d;

	if(s->donescan)
		return 0;
	if(s->pfx.nk == 0){	/* a zeroed Scan: start one */
		packdkey(pfx, sizeof(pfx), dir->qid.path, nil);
		btnewscan(s, pfx, sizeof(pfx));
	}
	stopped = 0;
	epochstart(0);
	if(waserror()){
		epochend(0);
		nexterror();
	}
	if(s->overflow){
		kv2dir(&s->kv, &d);
		if(fn(arg, &d) == -1){
			poperror();
			epochend(0);
			return 1;
		}
		s->overflow = 0;
	}
	t = agetp(&mnt->root);
	btenter(t, s);
	if(waserror()){
		btexit(s);
		nexterror();
	}
	while(btnext(s, &s->kv)){
		kv2dir(&s->kv, &d);
		if(fn(arg, &d) == -1){
			s->overflow = 1;
			stopped = 1;
			break;
		}
	}
	poperror();
	btexit(s);
	poperror();
	epochend(0);
	return stopped;
}

/* --- errors --- */

static struct {
	char	*msg;
	int	err;
} errtab[] = {
	{Esrch,		ENOENT},
	{Eexist,	EEXIST},
	{Enempty,	ENOTEMPTY},
	{Efull,		ENOSPC},
	{Estuffed,	ENOSPC},
	{Erdonly,	EROFS},
	{Elength,	ENAMETOOLONG},
	{Ename,		EINVAL},
	{Eperm,		EPERM},
	{Ecdir,		ENOTDIR},
	{Edir,		EISDIR},
	{Ephase,	ENOENT},	/* it went away */
	{Enomem,	ENOMEM},
	{Eio,		EIO},
	{Ecorrupt,	EIO},
	{Enosnap,	ENOENT},
	{Esnapx,	EEXIST},
	{Esnapu,	EBUSY},
	{Ewstatl,	EFBIG},
	{Eoffset,	EINVAL},
	{Enoqid,	ENOSPC},
	{Emode,		EINVAL},
	{Eimpl,		EOPNOTSUPP},
};

/* the errno for a gefs error string (NetBSD's values) */
int
gefserrno(char *e)
{
	int i, n;

	for(i = 0; i < (int)nelem(errtab); i++){
		n = strlen(errtab[i].msg);
		if(strncmp(e, errtab[i].msg, n) == 0)
			return errtab[i].err;
	}
	return EIO;
}
