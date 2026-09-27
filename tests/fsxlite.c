/*
 * fsxlite: random operations on one file, checked against a copy in
 * memory, after fsx (Avadis Tevanian, Apple): write, read, truncate, mmap
 * read and write, fsync, close and reopen, and on request a remount in
 * between (-r script).
 *
 *	fsxlite [-n ops] [-s seed] [-l maxlen] [-m] [-R remount-cmd] file
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char	*good, *buf;
static off_t	flen;
static long	maxlen = 4*1024*1024;
static int	fd, usemmap, verbose;
static char	*path;
static unsigned long	seed = 1;
static long	op;

static unsigned long
rnd(void)
{
	seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
	return seed >> 33;
}

/* the byte op writes at offset o: stale data tells which op wrote it */
static char
pat(long o_p, off_t o)
{
	return (char)(((o_p * 2654435761UL) >> 7) + o * 13 + (o >> 8));
}

static long
whowrote(off_t o, const char *got, int n)
{
	long p;
	int i;

	for (p = op; p >= 0; p--) {
		for (i = 0; i < n; i++)
			if (got[i] != pat(p, o + i))
				break;
		if (i == n)
			return p;
	}
	return -1;
}

static void
fail(const char *what, off_t off, long n)
{
	long i;

	warnx("op %ld: %s at %lld+%ld (file %lld)", op, what, (long long)off, n,
	    (long long)flen);
	for (i = 0; i < n; i++)
		if (buf[i] != good[off + i]) {
			warnx("first difference at %lld: got %02x want %02x; "
			    "the bytes there are op %ld's",
			    (long long)(off + i), buf[i] & 0xff,
			    good[off + i] & 0xff,
			    whowrote(off + i, buf + i, n - i < 16 ? n - i : 16));
			break;
		}
	exit(1);
}

static void
check(off_t off, long n)
{
	ssize_t r;

	if (off + n > flen)
		n = flen - off;
	if (n <= 0)
		return;
	r = pread(fd, buf, n, off);
	if (r != n)
		err(1, "op %ld: read %lld+%ld: %zd", op, (long long)off, n, r);
	if (memcmp(buf, good + off, n) != 0)
		fail("read differs", off, n);
}

static void
mcheck(off_t off, long n)
{
	char *p;
	long pg, o;

	if (off + n > flen)
		n = flen - off;
	if (n <= 0)
		return;
	pg = getpagesize();
	o = off % pg;
	p = mmap(NULL, n + o, PROT_READ, MAP_SHARED, fd, off - o);
	if (p == MAP_FAILED)
		err(1, "op %ld: mmap", op);
	memcpy(buf, p + o, n);
	munmap(p, n + o);
	if (memcmp(buf, good + off, n) != 0)
		fail("mmap read differs", off, n);
}

static void
dowrite(off_t off, long n)
{
	long i;

	if (off > flen)
		memset(good + flen, 0, off - flen);	/* a hole */
	for (i = 0; i < n; i++)
		good[off + i] = buf[i] = pat(op, off + i);
	if (pwrite(fd, buf, n, off) != n)
		err(1, "op %ld: write", op);
	if (off + n > flen)
		flen = off + n;
}

static void
domwrite(off_t off, long n)
{
	char *p;
	long pg, o, i;

	if (off + n > flen) {
		/* mmap cannot grow a file: extend first */
		if (ftruncate(fd, off + n) == -1)
			err(1, "op %ld: ftruncate for mmap", op);
		memset(good + flen, 0, off + n - flen);
		flen = off + n;
	}
	pg = getpagesize();
	o = off % pg;
	p = mmap(NULL, n + o, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off - o);
	if (p == MAP_FAILED)
		err(1, "op %ld: mmap", op);
	for (i = 0; i < n; i++)
		p[o + i] = good[off + i] = pat(op, off + i);
	if (rnd() % 4 == 0 && msync(p, n + o, MS_SYNC) == -1)
		err(1, "op %ld: msync", op);
	munmap(p, n + o);
}

static void
dotrunc(off_t len)
{
	if (ftruncate(fd, len) == -1)
		err(1, "op %ld: ftruncate", op);
	if (len > flen)
		memset(good + flen, 0, len - flen);
	flen = len;
}

int
main(int argc, char **argv)
{
	long nops, n;
	off_t off;
	struct stat st;
	char *remount;
	int c, k;

	nops = 10000;
	unsigned long seed0;
	remount = NULL;
	while ((c = getopt(argc, argv, "n:s:l:mvR:")) != -1)
		switch (c) {
		case 'n': nops = atol(optarg); break;
		case 's': seed = strtoul(optarg, NULL, 0); break;
		case 'l': maxlen = atol(optarg); break;
		case 'm': usemmap = 1; break;
		case 'v': verbose = 1; break;
		case 'R': remount = optarg; break;
		default:
			errx(2, "usage: fsxlite [-n ops] [-s seed] [-l maxlen] [-m] [-R cmd] file");
		}
	if (optind != argc - 1)
		errx(2, "a file");
	seed0 = seed;
	path = argv[optind];
	good = calloc(1, maxlen + 65536);
	buf = malloc(maxlen + 65536);
	if ((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)) == -1)
		err(1, "%s", path);
	flen = 0;
	for (op = 0; op < nops; op++) {
		k = rnd() % 100;
		off = rnd() % maxlen;
		n = 1 + rnd() % (k < 5 ? 65536 : 20000);
		if (off + n > maxlen)
			n = maxlen - off;
		if (verbose)
			printf("%ld k%d off %lld n %ld len %lld\n", op, k,
			    (long long)off, n, (long long)flen);
		if (k < 30)
			dowrite(off, n);
		else if (k < 38 && usemmap)
			domwrite(off, n);
		else if (k < 60)
			check(off, n);
		else if (k < 66 && usemmap)
			mcheck(off, n);
		else if (k < 72) {
			off_t t = rnd() % maxlen;
			if (verbose)
				printf("%ld trunc %lld\n", op, (long long)t);
			dotrunc(t);
		}
		else if (k < 75) {
			/* the length and a full compare */
			if (fstat(fd, &st) == -1)
				err(1, "fstat");
			if (st.st_size != flen)
				errx(1, "op %ld: size %lld, want %lld", op,
				    (long long)st.st_size, (long long)flen);
			check(0, flen);
		} else if (k < 78) {
			if (fsync(fd) == -1)
				err(1, "op %ld: fsync", op);
		} else if (k < 80) {
			close(fd);
			if (remount != NULL && rnd() % 8 == 0 && system(remount) != 0)
				errx(1, "op %ld: %s failed", op, remount);
			if ((fd = open(path, O_RDWR)) == -1)
				err(1, "op %ld: reopen", op);
		} else
			check(off, n);
	}
	check(0, flen);
	close(fd);
	printf("fsxlite: %ld operations, seed %lu, final length %lld: ok\n",
	    nops, seed0, (long long)flen);
	return 0;
}
