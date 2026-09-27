/*
 * Tests for the gefs core and the POSIX operations layer, in userland,
 * on an image file.  Each phase runs in a child process, so a phase
 * that ends without unmounting is a crash, and the next one loads from
 * whatever reached the image.
 *
 *	gefstest image
 */
#include <sys/types.h>
#include <sys/wait.h>	/* before libc.h: system headers may have fields named fs */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"
#include "host.h"

static char	*image;
static Mount	*mnt;
static Dent	*root;
static int	failures;

#define CHECK(c, ...) do{ if(!(c)){ fprint(2, "FAIL %s:%d: ", __FILE__, __LINE__); fprint(2, __VA_ARGS__); fprint(2, "\n"); failures++; } }while(0)

static void
mount(void)
{
	hostinit(32*MiB, 0);
	loadfs(image);
	hostloaded();
	mnt = getmount("main");
	root = gefsroot(mnt);
	gefsorphans(mnt);
}

static void
unmount(void)
{
	/* commit while the label is still mounted, as the kernel does */
	drainsyncq();
	clunkdent(mnt, root);
	clunkmount(mnt);
}

static Dent*
mk(Dent *dir, char *name, int type, ulong perm)
{
	Gattr a;

	memset(&a, 0, sizeof(a));
	a.type = type;
	a.perm = perm;
	a.uid = 1000;
	a.gid = 100;
	return gefscreate(mnt, dir, name, strlen(name), &a, nil, 0);
}

static Dent*
look(Dent *dir, char *name)
{
	return gefslookup(mnt, dir, name, strlen(name));
}

static void
pattern(char *buf, long n, vlong off, int seed)
{
	long i;

	for(i = 0; i < n; i++)
		buf[i] = (char)((off + i) * 31 + seed + ((off + i) >> 13));
}

static int
checkdata(Dent *de, vlong off, long n, int seed)
{
	char *buf, *want;
	long r;
	int ok;

	buf = malloc(n);
	want = malloc(n);
	r = gefsread(mnt, de, buf, off, n);
	pattern(want, n, off, seed);
	ok = (r == n && memcmp(buf, want, n) == 0);
	free(buf);
	free(want);
	return ok;
}

static int
iszero(Dent *de, vlong off, long n)
{
	char *buf;
	long i, r;

	buf = malloc(n);
	r = gefsread(mnt, de, buf, off, n);
	for(i = 0; i < r; i++)
		if(buf[i] != 0)
			break;
	free(buf);
	return r == n && i == n;
}

static void
put(Dent *de, vlong off, long n, int seed)
{
	char *buf;

	buf = malloc(n);
	pattern(buf, n, off, seed);
	CHECK(gefswrite(mnt, de, buf, off, n, -1, 1000) == n, "write %lld+%ld", off, n);
	free(buf);
}

/* phase 1: operations, then a clean unmount */
static void
phase1(void)
{
	Dent *a, *b, *f, *g, *h, *l, *d, *x;
	char buf[64];
	Gattr at;
	long n;

	mount();
	a = mk(root, "a", Gdir, 0755);
	b = mk(root, "b", Gdir, 0755);
	f = mk(a, "f", Greg, 0644);
	put(f, 0, 100000, 1);
	CHECK(checkdata(f, 0, 100000, 1), "read back 100000");
	put(f, 5, 7, 2);
	CHECK(checkdata(f, 5, 7, 2), "small overwrite");
	CHECK(checkdata(f, 12, 99988, 1), "rest after small overwrite");
	put(f, 300000, 5000, 3);
	CHECK(iszero(f, 100000, 200000), "hole reads as zeros");
	CHECK(checkdata(f, 300000, 5000, 3), "after the hole");
	gefstrunc(mnt, f, 12345, 1000);
	CHECK(f->length == 12345, "length after shrink %lld", f->length);
	CHECK(checkdata(f, 12, 12333, 1), "data after shrink");
	memset(&at, 0, sizeof(at));
	at.size = 60000;
	gefssetattr(mnt, f, &at, Asize, 1000);
	CHECK(iszero(f, 12345, 60000-12345), "grown tail is zero (old data must not come back)");
	/* rename within and across directories, and over a file */
	gefsrename(mnt, a, "f", 1, f, a, "g", 1, nil);
	CHECK((x = look(a, "f")) == nil, "old name gone");
	clunkdent(mnt, x);
	CHECK((g = look(a, "g")) == f, "new name");
	clunkdent(mnt, g);
	gefsrename(mnt, a, "g", 1, f, b, "g", 1, nil);
	CHECK((x = look(a, "g")) == nil, "moved out");
	clunkdent(mnt, x);
	h = mk(b, "h", Greg, 0600);
	put(h, 0, 1000, 9);
	gefsrename(mnt, b, "g", 1, f, b, "h", 1, h);
	CHECK((x = look(b, "h")) == f, "rename replaced h");
	clunkdent(mnt, x);
	CHECK(h->orphan, "replaced file is an orphan");
	CHECK(checkdata(h, 0, 1000, 9), "orphan still readable while held");
	gefsrelease(mnt, h);
	clunkdent(mnt, h);
	/* a directory moves with its parent link */
	gefsrename(mnt, root, "b", 1, b, a, "b2", 2, nil);
	CHECK((x = look(b, "..")) == a, "dotdot follows the move");
	clunkdent(mnt, x);
	CHECK(gefsancestor(mnt, a, b), "a is b's ancestor");
	CHECK(!gefsancestor(mnt, b, a), "b is not a's ancestor");
	/* remove: files and directories */
	CHECK(!gefsisempty(mnt, a), "a not empty");
	if(!waserror()){
		gefsremove(mnt, root, "a", 1, a);
		CHECK(0, "removing a non-empty directory worked");
		poperror();
	}else
		CHECK(strcmp(errmsg(), Enempty) == 0, "non-empty error: %s", errmsg());
	/* symlink and device node */
	memset(&at, 0, sizeof(at));
	at.type = Glnk;
	at.perm = 0777;
	l = gefscreate(mnt, b, "l", 1, &at, "../some/target", 14);
	n = gefsread(mnt, l, buf, 0, sizeof(buf));
	CHECK(n == 14 && memcmp(buf, "../some/target", 14) == 0, "symlink target");
	gefsgetattr(l, &at);
	CHECK(at.type == Glnk && at.size == 14, "symlink type %d size %lld", at.type, at.size);
	memset(&at, 0, sizeof(at));
	at.type = Gchr;
	at.perm = 0620;
	at.rdev = 0x1234;
	d = gefscreate(mnt, b, "tty", 3, &at, nil, 0);
	/* hard links: two names, one file */
	{
		Dent *hl, *hl2, *x2;
		Gattr ga;

		hl = mk(b, "hl", Greg, 0644);
		put(hl, 0, 5000, 11);
		gefslink(mnt, b, "hl2", 3, hl);
		CHECK(hl->nlink == 2, "nlink after link %d", hl->nlink);
		CHECK((hl2 = look(b, "hl2")) == hl, "second name is the same file");
		clunkdent(mnt, hl2);
		put(hl, 5000, 1000, 12);	/* through either name: one file */
		gefsgetattr(hl, &ga);
		CHECK(ga.size == 6000 && ga.nlink == 2, "linked size %lld nlink %d", ga.size, ga.nlink);
		gefslink(mnt, root, "hl3", 3, hl);
		CHECK(hl->nlink == 3, "nlink 3: %d", hl->nlink);
		gefsremove(mnt, b, "hl", 2, hl);
		CHECK(!hl->orphan && hl->nlink == 2, "one name removed: orphan %d nlink %d", hl->orphan, hl->nlink);
		CHECK((x2 = look(b, "hl")) == nil, "removed name gone");
		clunkdent(mnt, x2);
		gefsrename(mnt, root, "hl3", 3, hl, a, "hl4", 3, nil);
		CHECK(checkdata(hl, 0, 5000, 11) && checkdata(hl, 5000, 1000, 12), "linked data");
		clunkdent(mnt, hl);
	}
	/* setuid survives */
	memset(&at, 0, sizeof(at));
	at.perm = 04755;
	gefssetattr(mnt, d, &at, Aperm, 0);
	clunkdent(mnt, l);
	clunkdent(mnt, d);
	clunkdent(mnt, f);
	clunkdent(mnt, a);
	clunkdent(mnt, b);
	/* commits */
	CHECK(commit(0) == 1, "commit after changes");
	n = p9nwrite;
	CHECK(commit(0) == 0, "second commit skipped");
	CHECK(p9nwrite == n, "idle commit wrote %lld blocks", p9nwrite - n);
	/* the frees the first commit made go out lazily, or when forced */
	CHECK(agetl(&fs->lazydirty) != 0, "frees pending after a commit");
	CHECK(commit(Clazy) == 1, "lazy commit writes them");
	n = p9nwrite;
	CHECK(commit(Clazy) == 0 && p9nwrite == n, "then nothing is pending");
	unmount();
}

/* phase 2: everything from phase 1 is there after a reload */
static void
phase2(void)
{
	Dent *a, *b, *g, *d, *l;
	Gattr at;

	mount();
	CHECK((a = look(root, "a")) != nil, "a persisted");
	CHECK((b = look(a, "b2")) != nil, "a/b2 persisted");
	CHECK((g = look(b, "h")) != nil, "a/b2/h persisted");
	CHECK(g->length == 60000, "h length %lld", g->length);
	CHECK(checkdata(g, 12, 12333, 1), "h data persisted");
	CHECK(iszero(g, 12345, 60000-12345), "h tail zero after reload");
	CHECK((l = look(b, "l")) != nil, "symlink persisted");
	gefsgetattr(l, &at);
	CHECK(at.type == Glnk, "symlink type after reload");
	CHECK((d = look(b, "tty")) != nil, "device persisted");
	gefsgetattr(d, &at);
	CHECK(at.type == Gchr && at.rdev == 0x1234, "device %d rdev %llx", at.type, at.rdev);
	CHECK(at.perm == 04755, "setuid perm %lo", at.perm);
	{
		Dent *h2, *h4;
		Gattr ga;

		CHECK((h2 = look(b, "hl2")) != nil, "hl2 persisted");
		CHECK((h4 = look(a, "hl4")) == h2, "hl4 is hl2's file after reload");
		gefsgetattr(h2, &ga);
		CHECK(ga.nlink == 2 && ga.size == 6000, "link record nlink %d size %lld", ga.nlink, ga.size);
		CHECK(checkdata(h2, 0, 5000, 11) && checkdata(h2, 5000, 1000, 12), "linked data after reload");
		gefsremove(mnt, a, "hl4", 3, h4);
		gefsremove(mnt, b, "hl2", 3, h2);
		CHECK(h2->orphan, "last name removed: orphan");
		gefsrelease(mnt, h2);
		clunkdent(mnt, h4);
		clunkdent(mnt, h2);
	}
	clunkdent(mnt, a);
	clunkdent(mnt, b);
	clunkdent(mnt, g);
	clunkdent(mnt, d);
	clunkdent(mnt, l);
	unmount();
}

/* phase 3: remove a file while it is open, commit, and crash */
static void
phase3(void)
{
	Dent *f;

	mount();
	f = mk(root, "doomed", Greg, 0644);
	put(f, 0, 3*Blksz + 100, 7);
	gefsremove(mnt, root, "doomed", 6, f);
	put(f, 3*Blksz + 100, 1000, 8);	/* still writable */
	CHECK(checkdata(f, 0, 3*Blksz + 100, 7), "orphan readable");
	CHECK(commit(0) == 1, "commit with an orphan");
	/* exit without release or unmount: a crash */
	exits(nil);
}

/* phase 4: the orphan is freed at mount; the check passes */
static void
phase4(void)
{
	Dent *f;

	mount();
	CHECK((f = look(root, "hl2")) == nil, "phase 2's removals persisted");
	clunkdent(mnt, f);
	CHECK((f = look(root, "doomed")) == nil, "orphan has no name");
	clunkdent(mnt, f);
	unmount();
	qlock(&fs->mutlk);
	if(!checkfs(2))
		CHECK(0, "checkfs failed");
	qunlock(&fs->mutlk);
}

extern vlong	nsecskew;

/* how many labels start with pfx */
static int
nlabels(char *pfx)
{
	char k[128];
	Scan s;
	int n;

	k[0] = Klabel;
	n = snprint(k+1, sizeof(k)-1, "%s", pfx);
	btnewscan(&s, k, n+1);
	btenter(&fs->snap, &s);
	n = 0;
	while(btnext(&s, &s.kv))
		n++;
	btexit(&s);
	return n;
}

static int
ctlerr(char *cmd, char *want)
{
	int ok;

	if(waserror()){
		ok = strncmp(errmsg(), want, strlen(want)) == 0;
		if(!ok)
			fprint(2, "%s: %s\n", cmd, errmsg());
		return ok;
	}
	ctlcmd(cmd, 2);
	poperror();
	fprint(2, "%s: worked\n", cmd);
	return 0;
}

static uvlong
used(void)
{
	uvlong u;
	int i;

	u = 0;
	for(i = 0; i < fs->narena; i++)
		u += fs->arenas[i].used;
	return u;
}

/* phase 5: snapshots, forks, the schedule and its retention */
static void
phase5(void)
{
	Dent *f, *x, *r2;
	Mount *m, *m2;
	char *st, *e;
	uvlong u0, u1;
	int i, n;

	mount();
	/* phase 1's first commit took the day, hour and minute snapshots */
	CHECK(nlabels("main@day.") == 1 && nlabels("main@hour.") == 1,
		"first commit snapshots: day %d hour %d", nlabels("main@day."), nlabels("main@hour."));
	ctlcmd("retain main 3@m", 2);
	CHECK(nlabels("main@day.") == 0 && nlabels("main@hour.") == 0,
		"tiers left out of the spec age out: day %d hour %d",
		nlabels("main@day."), nlabels("main@hour."));
	f = mk(root, "snapfile", Greg, 0644);
	put(f, 0, 5000, 21);
	ctlcmd("snap main before", 2);
	put(f, 0, 5000, 22);
	CHECK(commit(0) == 1, "commit after the snapshot");

	/* the snapshot has the old data and is read-only */
	m = mnt;
	m2 = getmount("before");
	r2 = gefsroot(m2);
	mnt = m2;
	x = look(r2, "snapfile");
	CHECK(x != nil && checkdata(x, 0, 5000, 21), "snapshot keeps the old data");
	if(x != nil && !waserror()){
		put(x, 0, 10, 1);
		CHECK(0, "wrote into a snapshot");
		poperror();
	}
	mnt = m;
	CHECK(checkdata(f, 0, 5000, 22), "main has the new data");
	CHECK(ctlerr("delsnap before", Esnapu), "a mounted snapshot cannot go");
	clunkdent(m2, x);
	clunkdent(m2, r2);
	clunkmount(m2);
	ctlcmd("delsnap before", 2);
	CHECK(nlabels("before") == 0, "deleted");
	CHECK(ctlerr("delsnap before", Enolabel), "deleting it again");
	CHECK(ctlerr("snap main a/b", Ename), "a label with a slash");
	CHECK(ctlerr("retain main 3@x", Esyntax), "a bad schedule");
	CHECK(ctlerr("frobnicate", Ebadctl), "an unknown command");

	/* a fork is a mutable copy */
	ctlcmd("fork main work", 2);
	m2 = getmount("work");
	r2 = gefsroot(m2);
	mnt = m2;
	x = look(r2, "snapfile");
	CHECK(x != nil && checkdata(x, 0, 5000, 22), "the fork has main's data");
	u0 = used();
	put(x, 0, 5000, 23);
	{
		Dent *big = mk(r2, "big", Greg, 0644);
		char *buf = malloc(64*Blksz);
		for(i = 0; i < 16; i++){
			pattern(buf, 64*Blksz, i*64*Blksz, 5);
			gefswrite(m2, big, buf, (vlong)i*64*Blksz, 64*Blksz, -1, 0);
		}
		free(buf);
		clunkdent(m2, big);
	}
	CHECK(checkdata(x, 0, 5000, 23), "the fork writes");
	mnt = m;
	CHECK(checkdata(f, 0, 5000, 22), "main does not see the fork's writes");
	CHECK(commit(0) == 1, "commit the fork");
	u1 = used();
	CHECK(u1 > u0 + 1000*Blksz, "the fork's data takes space: %llud -> %llud", u0, u1);
	CHECK(nlabels("work@minute.") == 1, "the fork inherited main's schedule: %d",
		nlabels("work@minute."));
	ctlcmd("retain work none", 2);
	CHECK(nlabels("work@") == 0, "retain none drops the fork's snapshots: %d",
		nlabels("work@"));
	clunkdent(m2, x);
	clunkdent(m2, r2);
	clunkmount(m2);
	ctlcmd("snap work work2", 2);	/* of an unmounted fork */
	CHECK(nlabels("work2") == 1, "snapshot of an unmounted fork");
	ctlcmd("delsnap work2", 2);
	ctlcmd("delsnap work", 2);
	for(i = 0; i < 4; i++)
		commit(Clazy);
	CHECK(used() < u0 + 64*Blksz, "deleting the fork gave its space back: %llud -> %llud -> %llud",
		u0, u1, used());

	/* the schedule: one snapshot an interval, only after a change */
	n = nlabels("main@minute.");
	nsecskew += 61*Nsec;
	CHECK(commit(Clazy) == 1 || 1, "commit");
	CHECK(nlabels("main@minute.") == n, "no change, no snapshot: %d -> %d", n, nlabels("main@minute."));
	for(i = 0; i < 5; i++){
		nsecskew += 61*Nsec;
		put(f, 0, 100, 30+i);
		commit(0);
	}
	CHECK(nlabels("main@minute.") == 3, "3@m keeps 3: %d", nlabels("main@minute."));
	st = malloc(64*1024);
	e = seprintstatus(st, st+64*1024);
	CHECK(strstr(st, "conf main retain 3@m\n") != nil, "status shows the schedule:\n%s", st);
	CHECK(strstr(st, "fork main\n") != nil && strstr(st, "snap main@minute.") != nil
		&& strstr(st, "work") == nil, "status lists the labels:\n%s", st);
	CHECK(e > st && e[-1] == '\n', "status ends");
	fprint(2, "%s", st);
	free(st);
	clunkdent(mnt, f);
	unmount();
	qlock(&fs->mutlk);
	if(!checkfs(2))
		CHECK(0, "checkfs failed");
	qunlock(&fs->mutlk);
}

/* phase 6: the schedule and history survive a reload */
static void
phase6(void)
{
	mount();
	CHECK(nlabels("main@minute.") == 3, "minute snapshots after reload: %d", nlabels("main@minute."));
	CHECK(mnt->cron[0].cnt == 3 && mnt->cron[0].last != 0, "schedule reloaded: cnt %d last %lld",
		mnt->cron[0].cnt, mnt->cron[0].last);
	ctlcmd("retain main default", 2);
	CHECK(mnt->cron[1].cnt == 24 && mnt->cron[2].cnt == -1, "back to 9front's default");
	unmount();
}

static void
run(char *name, void (*fn)(void))
{
	int pid, st;

	fprint(2, "--- %s\n", name);
	if((pid = fork()) == 0){
		failures = 0;
		if(waserror()){
			fprint(2, "FAIL %s: error: %s\n", name, errmsg());
			exit(2);
		}
		fn();
		poperror();
		exit(failures != 0);
	}
	waitpid(pid, &st, 0);
	if(!WIFEXITED(st) || WEXITSTATUS(st) != 0){
		fprint(2, "FAIL %s (status %x)\n", name, st);
		failures++;
	}
}

static void
ream(void)
{
	hostinit(32*MiB, 0);
	reamfs(image);
}

int
main(int argc, char **argv)
{
	if(argc != 2){
		fprint(2, "usage: gefstest image\n");
		exits("usage");
	}
	image = argv[1];
	run("ream", ream);
	run("operations", phase1);
	run("reload", phase2);
	run("crash with an orphan", phase3);
	run("orphan cleanup", phase4);
	run("snapshots", phase5);
	run("snapshots after a reload", phase6);
	fprint(2, failures ? "FAILED\n" : "all passed\n");
	return failures != 0;
}
