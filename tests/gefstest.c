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
	clunkdent(mnt, root);
	clunkmount(mnt);
	drainsyncq();
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
	gefsrename(mnt, a, f, a, "g", 1, nil);
	CHECK((x = look(a, "f")) == nil, "old name gone");
	clunkdent(mnt, x);
	CHECK((g = look(a, "g")) == f, "new name");
	clunkdent(mnt, g);
	gefsrename(mnt, a, f, b, "g", 1, nil);
	CHECK((x = look(a, "g")) == nil, "moved out");
	clunkdent(mnt, x);
	h = mk(b, "h", Greg, 0600);
	put(h, 0, 1000, 9);
	gefsrename(mnt, b, f, b, "h", 1, h);
	CHECK((x = look(b, "h")) == f, "rename replaced h");
	clunkdent(mnt, x);
	CHECK(h->orphan, "replaced file is an orphan");
	CHECK(checkdata(h, 0, 1000, 9), "orphan still readable while held");
	gefsrelease(mnt, h);
	clunkdent(mnt, h);
	/* a directory moves with its parent link */
	gefsrename(mnt, root, b, a, "b2", 2, nil);
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
	CHECK((f = look(root, "doomed")) == nil, "orphan has no name");
	clunkdent(mnt, f);
	unmount();
	qlock(&fs->mutlk);
	if(!checkfs(2))
		CHECK(0, "checkfs failed");
	qunlock(&fs->mutlk);
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
	fprint(2, failures ? "FAILED\n" : "all passed\n");
	return failures != 0;
}
