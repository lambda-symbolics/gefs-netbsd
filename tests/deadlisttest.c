/*
 * Host-only deadlist metadata benchmark. Each source deadlist owns a real
 * on-disk deadlist block, so reclaim tests both metadata batching and the
 * reachability of the retained successor chain.
 */

/* Expose the static reclaim path while CORE_NOSNAP supplies the other core. */
#include "../../sys/fs/gefs/snap.c"
#include "host.h"
int commit(int);
void drainsyncq(void);

static vlong
used(void)
{
	vlong n;
	int i;

	n = 0;
	for(i = 0; i < fs->narena; i++)
		n += fs->arenas[i].used;
	return n;
}

static void
adddlist(vlong gen, vlong bgen)
{
	Dlist d;
	Blk *b;

	memset(&d, 0, sizeof(d));
	d.gen = gen;
	d.bgen = bgen;
	d.hd = Zb;
	/* dlflush recognizes this as the tail of a one-block list. */
	b = newblk(&fs->snap, Tdlist);
	d.tl = b->bp;
	d.ins = b;
	dlflush(&d);
}

static int
countdl(vlong gen)
{
	char pfx[9];
	Scan s;
	int n;

	pfx[0] = Kdlist;
	PACK64(pfx+1, gen);
	n = 0;
	btnewscan(&s, pfx, sizeof(pfx));
	btenter(&fs->snap, &s);
	while(btnext(&s, &s.kv))
		n++;
	btexit(&s);
	return n;
}

static void
verify_reload(char *image, int n, int src, int dst, int old)
{
	int ns, nd, ok;

	hostinit(64*MiB, 1);
	loadfs(image);
	hostloaded();
	qlock(&fs->mutlk);
	ok = checkfs(1);
	ns = countdl(src);
	nd = countdl(dst);
	qunlock(&fs->mutlk);
	if(!ok || (old ? ns != n || nd != 0 : ns != 0 || nd != n))
		sysfatal("reload verification failed: check=%d source=%d target=%d", ok, ns, nd);
	print("reload %s: source=%d target=%d check=ok\n", old ? "old" : "new", ns, nd);
}

int
main(int argc, char **argv)
{
	char *image, *mode;
	vlong before, after, w0, w1;
	int i, n, src, dst, old;

	if(argc < 2 || argc > 4)
		sysfatal("usage: deadlisttest image [deadlists|source|reclaim|check|oldcheck] [count]");
	image = argv[1];
	mode = argc >= 3 ? argv[2] : nil;
	src = 100;
	dst = 200;
	old = mode != nil && strcmp(mode, "oldcheck") == 0;
	if(mode != nil && (strcmp(mode, "check") == 0 || old)){
		n = argc == 4 ? atoi(argv[3]) : 128;
		if(n < 2 || n > 4096)
			sysfatal("deadlists must be between 2 and 4096");
		verify_reload(image, n, src, dst, old);
		return 0;
	}
	if(mode != nil && (strcmp(mode, "source") == 0 || strcmp(mode, "reclaim") == 0))
		n = argc == 4 ? atoi(argv[3]) : 128;
	else
		n = argc == 3 ? atoi(argv[2]) : 128;
	if(n < 2 || n > 4096)
		sysfatal("deadlists must be between 2 and 4096");

	hostinit(64*MiB, 0);
	loadfs(image);
	hostloaded();
	before = used();
	if(mode == nil || strcmp(mode, "source") == 0){
		qlock(&fs->mutlk);
		for(i = 0; i < n; i++)
			adddlist(src, 1000+i);
		fs->snap.dirty = 1;
		qunlock(&fs->mutlk);
		if(commit(0) != 1)
			sysfatal("source commit did not commit");
	}
	if(mode != nil && strcmp(mode, "source") == 0)
		return 0;
	if(countdl(src) != n)
		sysfatal("source setup count mismatch");

	w0 = p9nwrite;
	qlock(&fs->mutlk);
	if(waserror()){
		qunlock(&fs->mutlk);
		nexterror();
	}
	/* Source removal and successor publication are one tree update. */
	reclaimblocks(src, dst, 2000);
	fs->snap.dirty = 1;
	qunlock(&fs->mutlk);
	poperror();
	if(commit(0) != 1)
		sysfatal("reclaim commit did not commit");
	w1 = p9nwrite;
	after = used();
	if(countdl(src) != 0 || countdl(dst) != n)
		sysfatal("reclaim counts source=%d target=%d", countdl(src), countdl(dst));
	qlock(&fs->mutlk);
	if(!checkfs(1))
		sysfatal("in-memory fsck failed after reclaim");
	qunlock(&fs->mutlk);
	print("deadlists=%d writes=%lld retained-before=%lld retained-after=%lld check=ok\n",
		n, w1-w0, before, after);

	drainsyncq();
	return 0;
}
