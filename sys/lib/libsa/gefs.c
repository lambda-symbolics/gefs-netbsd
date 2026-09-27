/*	$NetBSD$	*/

/*
 * Copyright (c) 2026 Lukas Hozda.
 * gefs is by Ori Bernstein and the 9front contributors (MIT licence).
 * metrohash64 is by J. Andrew Rogers (MIT licence).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Read-only gefs, for the boot loaders.
 *
 * gefs (9front's file system, in sys/fs/gefs) is a copy-on-write
 * B-epsilon tree.  Interior nodes ("pivots") keep, next to their child
 * pointers, a buffer of messages on their way down; a lookup takes the
 * value in the leaf and applies the messages for its key from the
 * deepest pivot up to the root.  The superblock at the start of the
 * partition points at the snapshot tree, where a label ("main", or the
 * one a path starts with, "@label/...") names a tree of directory
 * entries, parent links and file blocks.  Blocks are 16 KiB, and each
 * pointer carries the metrohash64 of the block it points at.
 *
 * Numbers on the disk are big-endian; the hash reads little-endian
 * words, as the machines gefs runs on do.
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/endian.h>
#ifdef _STANDALONE
#include <lib/libkern/libkern.h>
#else
#include <string.h>
#endif

#include "stand.h"
#include "gefs.h"
#if defined(LIBSA_ENABLE_LS_OP)
#include "ls.h"
#endif

#define GBLKSZ		16384
#define GMAXHT		8		/* 16 KiB nodes: far more than any disk */
#define GNCACHE		(GMAXHT + 2)	/* blocks kept, as a rule */
#define GNPIN		(2*GMAXHT + 2)	/* and while more are pinned (ls) */
#define GKEYMAX		256
#define GINLMAX		512
#define GKVMAX		(GKEYMAX + GINLMAX)
#define GPIVHDSZ	10
#define GLEAFHDSZ	6
#define GBUFSPC		((GBLKSZ - GPIVHDSZ) / 2)
#define GPIVSPC		(GBLKSZ - GPIVHDSZ - GBUFSPC)
#define GXDIRSZ		61		/* flag qpath qvers qtype mode atime mtime len uid gid muid */
#define GTREESZ		(4*4 + 8*4 + 24)
#define GHASHSEED	0x6765

/* block types */
#define GTPIVOT		1
#define GTLEAF		2

/* key types */
#define GKDAT		0
#define GKENT		1
#define GKLABEL		3
#define GKSNAP		4
#define GKHREF		7		/* NetBSD: attributes of a linked file */

/* message types */
#define GOINSERT	1
#define GODELETE	2
#define GOCLEARB	3
#define GOCLOBBER	4
#define GOWSTAT		5
#define GORELINK	6
#define GOREPREV	7
#define GOINCREF	8

/* wstat message fields, in this order */
#define GOWSIZE		0x01
#define GOWMODE		0x02
#define GOWMTIME	0x04
#define GOWATIME	0x08
#define GOWUID		0x10
#define GOWGID		0x20
#define GOWMUID		0x40

/* Xdir: offsets in an entry's value; flags; mode bits */
#define GX_FLAG		0
#define GX_QPATH	8
#define GX_MODE		21
#define GX_MTIME	33
#define GX_LENGTH	41
#define GX_UID		49
#define GX_GID		53
#define GF_SYMLINK	0x1
#define GF_REFCNT	0x2
#define GF_DEVNOD	0x4
#define GDMDIR		0x80000000U
#define GDMSETUID	0x00080000U
#define GDMSETGID	0x00040000U
#define GDMSETVTX	0x00010000U

#define GMAXSYMLINK	8		/* links followed in one path */

struct gbp {
	uint64_t	addr;
	uint64_t	hash;
};

struct gblk {
	uint64_t	addr;		/* -1: empty */
	int		pin;
	unsigned	used;		/* LRU clock */
	char		*buf;
};

struct gtree {
	struct gbp	root;
	int		ht;
};

struct gkv {
	const char	*k;
	int		nk;
	const char	*v;
	int		nv;
	int		op;		/* messages */
};

struct gdir {
	uint64_t	qid;
	uint64_t	flag;
	uint32_t	mode;
	uint64_t	mtime;
	uint64_t	length;
	uint32_t	uid;
	uint32_t	gid;
};

struct file {
	struct gblk	cache[GNPIN];
	unsigned	clock;
	struct gtree	snap;		/* the snapshot tree */
	struct gtree	tree;		/* the label's */
	struct gdir	d;		/* the file open */
	off_t		seekp;
	char		*dbuf;		/* a data block */
	int64_t		dboff;		/* its file offset, -1: none */
};

/* --- numbers and the hash --- */

static uint64_t
metrohash64(const void *key, size_t len, uint32_t seed)
{
	static const uint64_t k0 = 0xC83A91E1ULL, k1 = 0x8648DBDBULL,
	    k2 = 0x7BDEC03BULL, k3 = 0x2F5870A5ULL;
	const uint8_t *p = key, *const end = p + len;
	uint64_t v0, v1, v2, v3, t, h;

#define ROR(v, k)	(((v) >> (k)) | ((v) << (64 - (k))))
	h = (((uint64_t)seed + k2) * k0) + len;
	if (len >= 32) {
		v0 = v1 = v2 = v3 = h;
		do {
			v0 += le64dec(p) * k0; v0 = ROR(v0, 29) + v2; p += 8;
			v1 += le64dec(p) * k1; v1 = ROR(v1, 29) + v3; p += 8;
			v2 += le64dec(p) * k2; v2 = ROR(v2, 29) + v0; p += 8;
			v3 += le64dec(p) * k3; v3 = ROR(v3, 29) + v1; p += 8;
		} while (p <= end - 32);
		t = ((v0 + v3) * k0) + v1; v2 ^= ROR(t, 33) * k1;
		t = ((v1 + v2) * k1) + v0; v3 ^= ROR(t, 33) * k0;
		t = ((v0 + v2) * k0) + v3; v0 ^= ROR(t, 33) * k1;
		t = ((v1 + v3) * k1) + v2; v1 ^= ROR(t, 33) * k0;
		h += v0 ^ v1;
	}
	if (end - p >= 16) {
		v0 = h + (le64dec(p) * k0); p += 8; v0 = ROR(v0, 33) * k1;
		v1 = h + (le64dec(p) * k1); p += 8; v1 = ROR(v1, 33) * k2;
		t = v0 * k0; v0 ^= ROR(t, 35) + v1;
		t = v1 * k3; v1 ^= ROR(t, 35) + v0;
		h += v1;
	}
	if (end - p >= 8) {
		h += le64dec(p) * k3; p += 8;
		h ^= ROR(h, 33) * k1;
	}
	if (end - p >= 4) {
		h += le32dec(p) * k3; p += 4;
		h ^= ROR(h, 15) * k1;
	}
	if (end - p >= 2) {
		h += le16dec(p) * k3; p += 2;
		h ^= ROR(h, 13) * k1;
	}
	if (end - p >= 1) {
		h += *p * k3;
		h ^= ROR(h, 25) * k1;
	}
	h ^= ROR(h, 33);
	h *= k0;
	h ^= ROR(h, 33);
#undef ROR
	return h;
}

/* --- blocks --- */

static int
readdev(struct open_file *f, uint64_t off, void *buf, size_t n)
{
	size_t rsize;
	int rc;

	rc = DEV_STRATEGY(f->f_dev)(f->f_devdata, F_READ,
	    off / GETSECSIZE(f), n, buf, &rsize);
	if (rc == 0 && rsize != n)
		rc = EIO;
	return rc;
}

/*
 * The block bp points at, checked, in the cache; pinned until unpinned.
 * A lookup pins one block a level.  The cache keeps GNCACHE blocks, and
 * grows to GNPIN only while that many are pinned (ls looks entries up
 * while it walks the tree): the EFI loader's heap is 1 MiB.
 */
static int
getblk(struct open_file *f, struct gbp bp, struct gblk **bpp)
{
	struct file *fp = f->f_fsdata;
	struct gblk *b, *victim, *empty;
	int i, rc, nalloc;

	victim = NULL;
	empty = NULL;
	nalloc = 0;
	for (i = 0; i < GNPIN; i++) {
		b = &fp->cache[i];
		if (b->buf == NULL) {
			if (empty == NULL)
				empty = b;
			continue;
		}
		nalloc++;
		if (b->addr == bp.addr) {
			b->pin++;
			b->used = ++fp->clock;
			*bpp = b;
			return 0;
		}
		if (b->pin == 0 && (victim == NULL || b->used < victim->used))
			victim = b;
	}
	if (empty != NULL && (victim == NULL || nalloc < GNCACHE)) {
		if ((empty->buf = alloc(GBLKSZ)) == NULL)
			return ENOMEM;
		victim = empty;
	}
	if (victim == NULL)
		return EIO;	/* deeper than GMAXHT */
	b = victim;
	b->addr = (uint64_t)-1;
	if ((rc = readdev(f, bp.addr, b->buf, GBLKSZ)) != 0)
		return rc;
	if (metrohash64(b->buf, GBLKSZ, GHASHSEED) != bp.hash) {
		printf("gefs: block %llx: bad hash\n",
		    (unsigned long long)bp.addr);
		return EIO;
	}
	b->addr = bp.addr;
	b->pin = 1;
	b->used = ++fp->clock;
	*bpp = b;
	return 0;
}

static void
unpin(struct gblk *b)
{
	if (b != NULL && b->pin > 0)
		b->pin--;
}

static struct gbp
unpackbp(const char *p)
{
	struct gbp bp;

	bp.addr = be64dec(p);
	bp.hash = be64dec(p + 8);
	return bp;
}

static int
btype(const struct gblk *b)
{
	return be16dec(b->buf);
}

static int
nvals(const struct gblk *b)
{
	return be16dec(b->buf + 2);
}

static int
nmsgs(const struct gblk *b)
{
	return btype(b) == GTPIVOT ? be16dec(b->buf + 6) : 0;
}

/* the i-th key and value of a node; zero if it does not parse */
static int
getval(const struct gblk *b, int i, struct gkv *kv)
{
	const char *d, *p, *e;
	int o;

	d = b->buf + (btype(b) == GTPIVOT ? GPIVHDSZ : GLEAFHDSZ);
	e = b->buf + GBLKSZ;
	o = be16dec(d + 2*i);
	p = d + o;
	if (p + 2 > e)
		return 0;
	kv->nk = be16dec(p);		p += 2;
	kv->k = p;			p += kv->nk;
	if (p + 2 > e)
		return 0;
	kv->nv = be16dec(p);		p += 2;
	kv->v = p;
	kv->op = 0;
	return p + kv->nv <= e;
}

/* the i-th message of a pivot */
static int
getmsg(const struct gblk *b, int i, struct gkv *m)
{
	const char *d, *p, *e;
	int o;

	d = b->buf + GPIVHDSZ + GPIVSPC;
	e = b->buf + GBLKSZ;
	o = be16dec(d + 2*i);
	p = d + o;
	if (p + 3 > e)
		return 0;
	m->op = *p;			p += 1;
	m->nk = be16dec(p);		p += 2;
	m->k = p;			p += m->nk;
	if (p + 2 > e)
		return 0;
	m->nv = be16dec(p);		p += 2;
	m->v = p;
	return p + m->nv <= e;
}

static int
keycmp(const char *a, int na, const char *b, int nb)
{
	int c;

	c = memcmp(a, b, na < nb ? na : nb);
	if (c != 0)
		return c < 0 ? -1 : 1;
	return (na > nb) - (na < nb);
}

/*
 * The last value whose key is at most k (the child to follow in a
 * pivot), or -1; *same if it is k.
 */
static int
blksearch(const struct gblk *b, const char *k, int nk, struct gkv *kv,
    int *same)
{
	int lo, hi, mid, ri, r;
	struct gkv c;

	ri = -1;
	lo = 0;
	hi = nvals(b) - 1;
	while (lo <= hi) {
		mid = (lo + hi) / 2;
		if (!getval(b, mid, &c))
			return -2;
		r = keycmp(k, nk, c.k, c.nk);
		if (r < 0)
			hi = mid - 1;
		else if (r == 0) {
			ri = mid;
			hi = mid - 1;
		} else
			lo = mid + 1;
	}
	*same = (ri != -1);
	if (ri == -1)
		ri = lo - 1;
	if (ri >= 0 && !getval(b, ri, kv))
		return -2;
	return ri;
}

/* the first message for k in a pivot, or -1 */
static int
bufsearch(const struct gblk *b, const char *k, int nk)
{
	int lo, hi, mid, ri, r;
	struct gkv c;

	ri = -1;
	lo = 0;
	hi = nmsgs(b) - 1;
	while (lo <= hi) {
		mid = (lo + hi) / 2;
		if (!getmsg(b, mid, &c))
			return -1;
		r = keycmp(k, nk, c.k, c.nk);
		if (r < 0)
			hi = mid - 1;
		else if (r == 0) {
			ri = mid;
			hi = mid - 1;
		} else
			lo = mid + 1;
	}
	return ri;
}

/*
 * A wstat message on an entry's value (at off: an entry, or 8 into a
 * link record): the fields the loader looks at.
 */
static void
statupdate(char *v, int nv, int off, const struct gkv *m)
{
	const char *p, *e;
	char *x;
	int op;

	if (nv < off + GXDIRSZ || m->nv < 1)
		return;
	x = v + off;
	p = m->v;
	e = m->v + m->nv;
	op = *p++;
	if ((op & GOWSIZE) && p + 8 <= e) {
		memcpy(x + GX_LENGTH, p, 8);
		p += 8;
	}
	if ((op & GOWMODE) && p + 4 <= e) {
		memcpy(x + GX_MODE, p, 4);
		p += 4;
	}
	if ((op & GOWMTIME) && p + 8 <= e) {
		memcpy(x + GX_MTIME, p, 8);
		p += 8;
	}
	if ((op & GOWATIME) && p + 8 <= e)
		p += 8;
	if ((op & GOWUID) && p + 4 <= e) {
		memcpy(x + GX_UID, p, 4);
		p += 4;
	}
	if ((op & GOWGID) && p + 4 <= e) {
		memcpy(x + GX_GID, p, 4);
		p += 4;
	}
}

/*
 * Look k up in a tree: the value (at most nbuf bytes) into buf, its
 * size into *nv.  Returns 0, ENOENT, or an I/O error.
 */
static int
lookup(struct open_file *f, const struct gtree *t, const char *k, int nk,
    char *buf, int nbuf, int *nv)
{
	struct gblk *path[GMAXHT];
	struct gkv kv, m;
	int i, j, ok, same, rc, off;

	if (t->ht < 1 || t->ht > GMAXHT)
		return EIO;
	memset(path, 0, sizeof(path));
	ok = 0;
	*nv = 0;
	if ((rc = getblk(f, t->root, &path[0])) != 0)
		goto out;
	for (i = 1; i < t->ht; i++) {
		j = blksearch(path[i-1], k, nk, &kv, &same);
		if (j == -2 || (j >= 0 && kv.nv < 24)) {
			rc = EIO;
			goto out;
		}
		if (j == -1)
			break;
		if ((rc = getblk(f, unpackbp(kv.v), &path[i])) != 0)
			goto out;
	}
	if (path[t->ht-1] != NULL) {
		if (blksearch(path[t->ht-1], k, nk, &kv, &ok) == -2) {
			rc = EIO;
			goto out;
		}
		if (ok) {
			if (kv.nv > nbuf) {
				rc = EIO;
				goto out;
			}
			memcpy(buf, kv.v, kv.nv);
			*nv = kv.nv;
		}
	}
	/* the messages, oldest (deepest) first */
	off = (nk == 9 && k[0] == GKHREF) ? 8 : 0;
	for (i = t->ht - 2; i >= 0; i--) {
		if (path[i] == NULL)
			continue;
		for (j = bufsearch(path[i], k, nk); j >= 0 && j < nmsgs(path[i]); j++) {
			if (!getmsg(path[i], j, &m)) {
				rc = EIO;
				goto out;
			}
			if (keycmp(k, nk, m.k, m.nk) != 0)
				break;
			switch (m.op) {
			case GOINSERT:
				if (m.nv > nbuf) {
					rc = EIO;
					goto out;
				}
				memcpy(buf, m.v, m.nv);
				*nv = m.nv;
				ok = 1;
				break;
			case GODELETE:
			case GOCLEARB:
			case GOCLOBBER:
				ok = 0;
				break;
			case GOWSTAT:
				if (ok)
					statupdate(buf, *nv, off, &m);
				break;
			default:
				/* snapshot links and counts: not needed here */
				break;
			}
		}
	}
	rc = ok ? 0 : ENOENT;
out:
	for (i = 0; i < GMAXHT; i++)
		unpin(path[i]);
	return rc;
}

/* --- labels, entries, blocks --- */

static int
opentree(struct open_file *f, const char *label, int n, struct gtree *t)
{
	struct file *fp = f->f_fsdata;
	char k[GKEYMAX], v[GKVMAX];
	int nv, rc;

	if (n <= 0 || n + 1 > GKEYMAX)
		return ENOENT;
	k[0] = GKLABEL;
	memcpy(k + 1, label, n);
	if ((rc = lookup(f, &fp->snap, k, n + 1, v, sizeof(v), &nv)) != 0)
		return rc;
	if (nv < 1 + 8 || v[0] != GKSNAP)
		return EIO;
	/* Klabel => Ksnap gen[8] flags[4]; Ksnap gen[8] => the tree */
	memcpy(k, v, 9);
	if ((rc = lookup(f, &fp->snap, k, 9, v, sizeof(v), &nv)) != 0)
		return rc == ENOENT ? EIO : rc;
	if (nv < GTREESZ)
		return EIO;
	t->ht = be32dec(v + 8);
	t->root = unpackbp(v + 48);
	return 0;
}

static void
xdir(const char *v, struct gdir *d)
{
	d->flag = be64dec(v + GX_FLAG);
	d->qid = be64dec(v + GX_QPATH);
	d->mode = be32dec(v + GX_MODE);
	d->mtime = be64dec(v + GX_MTIME);
	d->length = be64dec(v + GX_LENGTH);
	d->uid = be32dec(v + GX_UID);
	d->gid = be32dec(v + GX_GID);
}

/* the entry name in directory pqid (-1 and "" for the root) */
static int
getent(struct open_file *f, uint64_t pqid, const char *name, int n,
    struct gdir *d)
{
	struct file *fp = f->f_fsdata;
	char k[GKEYMAX], v[GKVMAX];
	int nv, rc;

	if (1 + 8 + 2 + n + 1 > GKEYMAX)
		return ENOENT;
	/* Kent pqid[8] len[2] name NUL */
	k[0] = GKENT;
	be64enc(k + 1, pqid);
	be16enc(k + 9, n);
	memcpy(k + 11, name, n);
	k[11 + n] = 0;
	rc = lookup(f, &fp->tree, k, 12 + n, v, sizeof(v), &nv);
	if (rc != 0)
		return rc;
	if (nv < GXDIRSZ)
		return EIO;
	xdir(v, d);
	/* NetBSD: a file with several names keeps its attributes apart */
	if (d->flag & GF_REFCNT) {
		k[0] = GKHREF;
		be64enc(k + 1, d->qid);
		rc = lookup(f, &fp->tree, k, 9, v, sizeof(v), &nv);
		if (rc == 0 && nv >= 8 + GXDIRSZ)
			xdir(v + 8, d);
		else if (rc != ENOENT)
			return rc == 0 ? EIO : rc;
	}
	return 0;
}

/* the data block of the file open at off (rounded down), or zeros */
static int
getdata(struct open_file *f, int64_t off)
{
	struct file *fp = f->f_fsdata;
	char k[17], v[64];
	struct gbp bp;
	int nv, rc;

	off &= ~(int64_t)(GBLKSZ - 1);
	if (fp->dboff == off)
		return 0;
	fp->dboff = -1;
	k[0] = GKDAT;
	be64enc(k + 1, fp->d.qid);
	be64enc(k + 9, off);
	rc = lookup(f, &fp->tree, k, 17, v, sizeof(v), &nv);
	if (rc == ENOENT) {
		memset(fp->dbuf, 0, GBLKSZ);	/* a hole */
		fp->dboff = off;
		return 0;
	}
	if (rc != 0)
		return rc;
	if (nv < 24)
		return EIO;
	bp = unpackbp(v);
	twiddle();
	if ((rc = readdev(f, bp.addr, fp->dbuf, GBLKSZ)) != 0)
		return rc;
	if (metrohash64(fp->dbuf, GBLKSZ, GHASHSEED) != bp.hash) {
		printf("gefs: data block %llx: bad hash\n",
		    (unsigned long long)bp.addr);
		return EIO;
	}
	fp->dboff = off;
	return 0;
}

static int
isdir(const struct gdir *d)
{
	return (d->mode & GDMDIR) != 0;
}

static int
islink(const struct gdir *d)
{
	return !isdir(d) && (d->flag & GF_SYMLINK) != 0;
}

/* a directory known by its qid only (a parent, going up) */
static void
dirof(struct gdir *d, uint64_t qid)
{
	memset(d, 0, sizeof(*d));
	d->qid = qid;
	d->mode = GDMDIR | 0555;
}

/*
 * Walk path from the root of the label, following symbolic links, to
 * fp->d.  A path "@label/rest" is looked up in that label, say a
 * snapshot: "@main@day.2026.09.27_00:00:00/netbsd".
 */
static int
walk(struct open_file *f, const char *path)
{
	struct file *fp = f->f_fsdata;
	uint64_t stack[64];
	struct gdir d;
	const char *p, *q, *label;
	char *link, *nbuf;
	size_t linksz, nsz;
	int depth, nlinks, rc, n, nl;

	label = "main";
	nl = 4;
	if (path[0] == '@') {
		label = path + 1;
		for (q = label; *q != 0 && *q != '/'; q++)
			continue;
		nl = q - label;
		path = q;
	}
	if ((rc = opentree(f, label, nl, &fp->tree)) != 0)
		return rc;
	if ((rc = getent(f, (uint64_t)-1, "", 0, &d)) != 0)
		return rc == ENOENT ? EIO : rc;
	depth = 0;
	stack[0] = d.qid;
	nlinks = 0;
	link = NULL;
	linksz = 0;
	p = path;
	rc = 0;
	for (;;) {
		while (*p == '/')
			p++;
		if (*p == 0)
			break;
		if (!isdir(&d)) {
			rc = ENOTDIR;
			break;
		}
		for (q = p; *q != 0 && *q != '/'; q++)
			continue;
		n = q - p;
		if (n == 1 && p[0] == '.') {
			p = q;
			continue;
		}
		if (n == 2 && p[0] == '.' && p[1] == '.') {
			if (depth > 0)
				depth--;
			dirof(&d, stack[depth]);
			p = q;
			continue;
		}
		if ((rc = getent(f, stack[depth], p, n, &d)) != 0)
			break;
		p = q;
		if (islink(&d)) {
			/* the target is the link's data; the rest of the path follows */
			if (++nlinks > GMAXSYMLINK || d.length >= GBLKSZ) {
				rc = ELOOP;
				break;
			}
			fp->d = d;
			fp->dboff = -1;
			if ((rc = getdata(f, 0)) != 0)
				break;
			fp->dboff = -1;
			n = strlen(p);
			nsz = d.length + n + 1;
			if ((nbuf = alloc(nsz)) == NULL) {
				rc = ENOMEM;
				break;
			}
			memcpy(nbuf, fp->dbuf, d.length);
			memcpy(nbuf + d.length, p, n + 1);
			if (link != NULL)
				dealloc(link, linksz);
			link = nbuf;
			linksz = nsz;
			p = link;
			if (*p == '/') {
				depth = 0;
				if ((rc = getent(f, (uint64_t)-1, "", 0, &d)) != 0)
					break;
			} else
				dirof(&d, stack[depth]);
			continue;
		}
		if (isdir(&d)) {
			if (depth + 1 >= (int)__arraycount(stack)) {
				rc = ENAMETOOLONG;
				break;
			}
			stack[++depth] = d.qid;
		}
	}
	if (link != NULL)
		dealloc(link, linksz);
	if (rc == 0)
		fp->d = d;
	return rc;
}

/* --- the file system operations --- */

static void
freefile(struct file *fp)
{
	int i;

	for (i = 0; i < GNPIN; i++)
		if (fp->cache[i].buf != NULL)
			dealloc(fp->cache[i].buf, GBLKSZ);
	if (fp->dbuf != NULL)
		dealloc(fp->dbuf, GBLKSZ);
	dealloc(fp, sizeof(*fp));
}

__compactcall int
gefs_open(const char *path, struct open_file *f)
{
	struct file *fp;
	char *sb;
	int i, rc, narena;

	fp = alloc(sizeof(*fp));
	if (fp == NULL)
		return ENOMEM;
	memset(fp, 0, sizeof(*fp));
	for (i = 0; i < GNPIN; i++)
		fp->cache[i].addr = (uint64_t)-1;
	fp->dboff = -1;
	f->f_fsdata = fp;
	if ((fp->dbuf = alloc(GBLKSZ)) == NULL) {
		rc = ENOMEM;
		goto bad;
	}

	/* the superblock: "gefs9.00", sizes, the snapshot tree, a hash */
	sb = fp->dbuf;
	if ((rc = readdev(f, 0, sb, GBLKSZ)) != 0)
		goto bad;
	if (memcmp(sb, "gefs9.00", 8) != 0 || be32dec(sb + 8) != GBLKSZ) {
		rc = EINVAL;
		goto bad;
	}
	narena = be32dec(sb + 16);
	if (narena < 1 || narena > 256 ||
	    be64dec(sb + 104 + 16*narena) !=
	    metrohash64(sb, 104 + 16*narena, GHASHSEED)) {
		printf("gefs: bad superblock\n");
		rc = EINVAL;
		goto bad;
	}
	fp->snap.ht = be32dec(sb + 20);
	fp->snap.root.addr = be64dec(sb + 24);
	fp->snap.root.hash = be64dec(sb + 32);

	if ((rc = walk(f, path)) != 0)
		goto bad;
	fp->seekp = 0;
	fp->dboff = -1;
	return 0;
bad:
	f->f_fsdata = NULL;
	freefile(fp);
	return rc;
}

#ifndef LIBSA_NO_FS_CLOSE
__compactcall int
gefs_close(struct open_file *f)
{
	struct file *fp = f->f_fsdata;

	f->f_fsdata = NULL;
	if (fp != NULL)
		freefile(fp);
	return 0;
}
#endif

__compactcall int
gefs_read(struct open_file *f, void *start, size_t size, size_t *resid)
{
	struct file *fp = f->f_fsdata;
	char *addr = start;
	size_t n, o;
	int rc;

	rc = 0;
	if (isdir(&fp->d))
		rc = EISDIR;
	while (rc == 0 && size > 0 && (uint64_t)fp->seekp < fp->d.length) {
		if ((rc = getdata(f, fp->seekp)) != 0)
			break;
		o = fp->seekp & (GBLKSZ - 1);
		n = GBLKSZ - o;
		if (n > size)
			n = size;
		if (n > fp->d.length - fp->seekp)
			n = fp->d.length - fp->seekp;
		memcpy(addr, fp->dbuf + o, n);
		fp->seekp += n;
		addr += n;
		size -= n;
	}
	if (resid)
		*resid = size;
	return rc;
}

#ifndef LIBSA_NO_FS_WRITE
__compactcall int
gefs_write(struct open_file *f, void *start, size_t size, size_t *resid)
{
	return EROFS;
}
#endif

#ifndef LIBSA_NO_FS_SEEK
__compactcall off_t
gefs_seek(struct open_file *f, off_t offset, int where)
{
	struct file *fp = f->f_fsdata;

	switch (where) {
	case SEEK_SET:
		fp->seekp = offset;
		break;
	case SEEK_CUR:
		fp->seekp += offset;
		break;
	case SEEK_END:
		fp->seekp = fp->d.length - offset;
		break;
	default:
		return -1;
	}
	return fp->seekp;
}
#endif

__compactcall int
gefs_stat(struct open_file *f, struct stat *sb)
{
	struct file *fp = f->f_fsdata;
	uint32_t m;

	memset(sb, 0, sizeof(*sb));
	m = fp->d.mode;
	sb->st_mode = m & 0777;
	if (m & GDMSETUID)
		sb->st_mode |= S_ISUID;
	if (m & GDMSETGID)
		sb->st_mode |= S_ISGID;
	if (m & GDMSETVTX)
		sb->st_mode |= S_ISVTX;
	if (isdir(&fp->d))
		sb->st_mode |= S_IFDIR;
	else if (islink(&fp->d))
		sb->st_mode |= S_IFLNK;
	else
		sb->st_mode |= S_IFREG;
	sb->st_uid = fp->d.uid;
	sb->st_gid = fp->d.gid;
	sb->st_size = fp->d.length;
	sb->st_ino = fp->d.qid + 1;
	return 0;
}

#if defined(LIBSA_ENABLE_LS_OP)

/*
 * The names in the directory open: every entry key under its prefix, in
 * the leaves and in the pivots' messages, checked with a lookup (the
 * messages may delete it).
 */
struct lsseen {
	struct lsseen	*next;
	int		n;
	char		name[1];
};

struct lsstate {
	struct open_file *f;
	const char	*pattern;
	char		pfx[9];
	lsentry_t	*names;
	struct lsseen	*seen;		/* a name can be in a leaf and a message */
	int		rc;
};

static void
lsname(struct lsstate *ls, const char *k, int nk)
{
	struct lsseen *e;
	struct gdir d;
	int n;

	if (nk < 12 || memcmp(k, ls->pfx, 9) != 0)
		return;
	n = be16dec(k + 9);
	if (n == 0 || 11 + n + 1 != nk)
		return;
	for (e = ls->seen; e != NULL; e = e->next)
		if (e->n == n && memcmp(e->name, k + 11, n) == 0)
			return;
	if ((e = alloc(sizeof(*e) + n)) != NULL) {
		e->n = n;
		memcpy(e->name, k + 11, n);
		e->next = ls->seen;
		ls->seen = e;
	}
	if (getent(ls->f, be64dec(ls->pfx + 1), k + 11, n, &d) != 0)
		return;
	lsadd(&ls->names, ls->pattern, k + 11, n, (uint32_t)(d.qid + 1),
	    isdir(&d) ? "DIR" : islink(&d) ? "LNK" : "REG");
}

static void
lsnode(struct lsstate *ls, struct gbp bp, int level, int ht)
{
	struct gblk *b;
	struct gkv kv, m;
	int i, n, same;

	if (ls->rc != 0)
		return;
	if (level >= GMAXHT || getblk(ls->f, bp, &b) != 0) {
		ls->rc = EIO;
		return;
	}
	n = nvals(b);
	if (level == ht - 1) {
		for (i = 0; i < n; i++)
			if (getval(b, i, &kv))
				lsname(ls, kv.k, kv.nk);
		unpin(b);
		return;
	}
	for (i = 0; i < nmsgs(b); i++)
		if (getmsg(b, i, &m) && m.op == GOINSERT)
			lsname(ls, m.k, m.nk);
	/*
	 * Child i holds the keys from its pivot key to the next one: from
	 * the last child starting at or below the prefix, to the first one
	 * starting above every key with the prefix.
	 */
	i = blksearch(b, ls->pfx, sizeof(ls->pfx), &kv, &same);
	for (i = (i < 0) ? 0 : i; i < n && ls->rc == 0; i++) {
		if (!getval(b, i, &kv) || kv.nv < 24) {
			ls->rc = EIO;
			break;
		}
		if (memcmp(kv.k, ls->pfx, kv.nk < 9 ? kv.nk : 9) > 0)
			break;
		lsnode(ls, unpackbp(kv.v), level + 1, ht);
	}
	unpin(b);
}

__compactcall void
gefs_ls(struct open_file *f, const char *pattern)
{
	struct file *fp = f->f_fsdata;
	struct lsstate ls;

	if (!isdir(&fp->d)) {
		printf("not a directory\n");
		return;
	}
	memset(&ls, 0, sizeof(ls));
	ls.f = f;
	ls.pattern = pattern;
	ls.pfx[0] = GKENT;
	be64enc(ls.pfx + 1, fp->d.qid);
	lsnode(&ls, fp->tree.root, 0, fp->tree.ht);
	if (ls.rc != 0)
		printf("gefs: error %d reading the directory\n", ls.rc);
	lsprint(ls.names);
	lsfree(ls.names);
	while (ls.seen != NULL) {
		struct lsseen *e = ls.seen;

		ls.seen = e->next;
		dealloc(e, sizeof(*e) + e->n);
	}
}

#endif /* LIBSA_ENABLE_LS_OP */
