/*
 * sagefs: the boot loaders' gefs reader (sys/lib/libsa/gefs.c), in
 * userland, on a device or image.
 *
 *	sagefs dev cat path	the file, to stdout
 *	sagefs dev stat path	mode, size, uid, gid, ino
 *	sagefs dev ls path	the names in a directory
 *	sagefs dev sum path...	the size and a checksum of each file
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "stand.h"
#include "ls.h"
#include "../../sys/lib/libsa/gefs.h"

static int	devfd;
static long long nreads, nalloc, maxalloc;

static int
strategy(void *devdata, int rw, daddr_t blk, size_t size, void *buf, size_t *rsize)
{
	ssize_t n;

	(void)devdata;
	(void)rw;
	nreads++;
	n = pread(devfd, buf, size, (off_t)blk * 512);
	if (n < 0)
		return EIO;
	*rsize = n;
	return 0;
}

static const struct devsw dev = { "img", strategy };

void *
alloc(size_t n)
{
	nalloc += n;
	if (nalloc > maxalloc)
		maxalloc = nalloc;
	return malloc(n);
}

void
dealloc(void *p, size_t n)
{
	nalloc -= n;
	free(p);
}

void
twiddle(void)
{
}

void
lsadd(lsentry_t **names, const char *pattern, const char *name, size_t n,
    uint32_t ino, const char *type)
{
	lsentry_t *e, **np;

	(void)pattern;
	e = malloc(sizeof(*e) + n);
	e->e_ino = ino;
	e->e_type = type;
	memcpy(e->e_name, name, n);
	e->e_name[n] = 0;
	for (np = names; *np; np = &(*np)->e_next)
		if (strcmp(e->e_name, (*np)->e_name) < 0)
			break;
	e->e_next = *np;
	*np = e;
}

void
lsprint(lsentry_t *e)
{
	for (; e != NULL; e = e->e_next)
		printf("%u: %s (%s)\n", e->e_ino, e->e_name, e->e_type);
}

void
lsfree(lsentry_t *e)
{
	lsentry_t *n;

	for (; e != NULL; e = n) {
		n = e->e_next;
		free(e);
	}
}

/* FNV-1a, a stand-in for cksum */
static uint64_t
fnv(uint64_t h, const unsigned char *p, size_t n)
{
	while (n-- > 0)
		h = (h ^ *p++) * 0x100000001b3ULL;
	return h;
}

static int
openit(struct open_file *f, const char *path)
{
	int rc;

	memset(f, 0, sizeof(*f));
	f->f_dev = &dev;
	f->f_flags = F_READ;
	if ((rc = gefs_open(path, f)) != 0)
		fprintf(stderr, "sagefs: %s: %s\n", path, strerror(rc));
	return rc;
}

int
main(int argc, char **argv)
{
	struct open_file f;
	static char buf[1 << 20];
	struct stat st;
	size_t resid;
	uint64_t h, total;
	int i, rc, bad;

	if (argc < 4) {
		fprintf(stderr, "usage: sagefs dev cat|stat|ls|sum path...\n");
		return 2;
	}
	if ((devfd = open(argv[1], O_RDONLY)) < 0) {
		perror(argv[1]);
		return 1;
	}
	bad = 0;
	for (i = 3; i < argc; i++) {
		if (openit(&f, argv[i]) != 0) {
			bad = 1;
			continue;
		}
		if (strcmp(argv[2], "ls") == 0)
			gefs_ls(&f, NULL);
		else if (strcmp(argv[2], "stat") == 0) {
			gefs_stat(&f, &st);
			printf("%s: mode %o size %lld uid %d gid %d ino %llu\n", argv[i],
			    (unsigned)st.st_mode, (long long)st.st_size, (int)st.st_uid,
			    (int)st.st_gid, (unsigned long long)st.st_ino);
		} else {
			h = 0xcbf29ce484222325ULL;
			total = 0;
			for (;;) {
				rc = gefs_read(&f, buf, sizeof(buf), &resid);
				if (rc != 0) {
					fprintf(stderr, "sagefs: %s: read: %s\n", argv[i], strerror(rc));
					bad = 1;
					break;
				}
				if (resid == sizeof(buf))
					break;
				if (strcmp(argv[2], "cat") == 0)
					fwrite(buf, 1, sizeof(buf) - resid, stdout);
				h = fnv(h, (unsigned char *)buf, sizeof(buf) - resid);
				total += sizeof(buf) - resid;
			}
			if (strcmp(argv[2], "sum") == 0)
				printf("%016llx %llu %s\n", (unsigned long long)h,
				    (unsigned long long)total, argv[i]);
		}
		gefs_close(&f);
	}
	fprintf(stderr, "sagefs: %lld device reads, at most %lld KiB allocated\n",
	    nreads, maxalloc / 1024);
	return bad;
}
