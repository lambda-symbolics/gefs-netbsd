/*
 * readdir-regress DIR: directory enumeration checks for a gefs mount
 * (2026-10-18: a directory once listed while empty kept listing empty, so
 * git clone could not find its own pack and a download into an empty
 * ~/Desktop was invisible to ls while openable by name).
 *
 * cc -O2 -Wall -Wextra -o readdir-regress readdir-regress.c
 * Exit status: number of failed checks.  Cleanup uses known names only, so
 * an enumeration bug cannot hide the test's own files from it.
 */
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NLONG	2048

static int fails;

static void
check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		fails++;
}

/* Times name appears in a fresh listing of dir. */
static int
seen(const char *dir, const char *name)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (strcmp(e->d_name, name) == 0)
			n++;
	closedir(d);
	return n;
}

static int
entries(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			n++;
	closedir(d);
	return n;
}

static void
touch(const char *path)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);

	if (fd >= 0) {
		(void)write(fd, "x", 1);
		close(fd);
	}
}

int
main(int argc, char **argv)
{
	char base[1024], a[1100], b[1100], p[2048], q[2048];
	struct statvfs sv;
	struct stat st;
	DIR *d;
	struct dirent *e;
	int i, n, *count;

	if (argc != 2) {
		fprintf(stderr, "usage: readdir-regress DIR\n");
		return 2;
	}
	snprintf(base, sizeof(base), "%s/rdr.%ld", argv[1], (long)getpid());
	if (mkdir(base, 0755) != 0) {
		perror(base);
		return 2;
	}

	/* 1. empty, listed, then a file created: a fresh listing shows it */
	snprintf(a, sizeof(a), "%s/empty1", base);
	mkdir(a, 0755);
	check(entries(a) == 0, "new directory lists empty");
	snprintf(p, sizeof(p), "%s/file", a);
	touch(p);
	check(seen(a, "file") == 1, "file created after an empty listing appears");
	check(access(p, R_OK) == 0, "and opens by its path");

	/* 2. rename into a directory listed while empty */
	snprintf(b, sizeof(b), "%s/empty2", base);
	mkdir(b, 0755);
	check(entries(b) == 0, "second directory lists empty");
	snprintf(p, sizeof(p), "%s/moved.tmp", base);
	touch(p);
	snprintf(q, sizeof(q), "%s/moved", b);
	check(rename(p, q) == 0, "rename into it");
	check(seen(b, "moved") == 1, "renamed file appears in a fresh listing");
	check(seen(base, "moved.tmp") == 0, "and is gone from the source");

	/* 3. rewinddir on an open stream after a create */
	snprintf(a, sizeof(a), "%s/rewind", base);
	mkdir(a, 0755);
	d = opendir(a);
	while (d != NULL && readdir(d) != NULL)
		;
	snprintf(p, sizeof(p), "%s/late", a);
	touch(p);
	n = 0;
	if (d != NULL) {
		rewinddir(d);
		while ((e = readdir(d)) != NULL)
			if (strcmp(e->d_name, "late") == 0)
				n++;
		closedir(d);
	}
	check(n == 1, "rewinddir shows a file created after the first pass");

	/* 4. many long names over several readdir buffers, each exactly once */
	snprintf(a, sizeof(a), "%s/long", base);
	mkdir(a, 0755);
	check(entries(a) == 0, "long-name directory lists empty first");
	for (i = 0; i < NLONG; i++) {
		snprintf(p, sizeof(p), "%s/%04d-%0200d", a, i, 0);
		touch(p);
	}
	count = calloc(NLONG, sizeof(int));
	d = opendir(a);
	while (d != NULL && (e = readdir(d)) != NULL) {
		i = atoi(e->d_name);
		if (e->d_name[0] != '.' && i >= 0 && i < NLONG)
			count[i]++;
	}
	if (d != NULL)
		closedir(d);
	for (n = 0, i = 0; i < NLONG; i++)
		n += count[i] == 1;
	printf("      %d of %d long names seen exactly once\n", n, NLONG);
	check(n == NLONG, "every long name listed exactly once");
	free(count);

	/* 5. hard links: unlinking one name leaves the other with nlink 1 */
	snprintf(p, sizeof(p), "%s/h1", base);
	snprintf(q, sizeof(q), "%s/h2", base);
	touch(p);
	check(link(p, q) == 0, "link");
	check(unlink(p) == 0 && stat(q, &st) == 0 && st.st_nlink == 1,
	    "unlink of one name leaves nlink 1 on the other");

	/* 6. statvfs: used inodes = files - ffree must not be negative */
	check(statvfs(argv[1], &sv) == 0 && sv.f_files >= sv.f_ffree,
	    "statvfs f_files >= f_ffree");

	/* cleanup by known names; rmdir must find every directory empty */
	snprintf(p, sizeof(p), "%s/empty1/file", base); unlink(p);
	snprintf(p, sizeof(p), "%s/empty2/moved", base); unlink(p);
	snprintf(p, sizeof(p), "%s/rewind/late", base); unlink(p);
	snprintf(p, sizeof(p), "%s/h2", base); unlink(p);
	for (i = 0; i < NLONG; i++) {
		snprintf(p, sizeof(p), "%s/long/%04d-%0200d", base, i, 0);
		unlink(p);
	}
	n = 0;
	snprintf(p, sizeof(p), "%s/empty1", base); n += rmdir(p) == 0;
	snprintf(p, sizeof(p), "%s/empty2", base); n += rmdir(p) == 0;
	snprintf(p, sizeof(p), "%s/rewind", base); n += rmdir(p) == 0;
	snprintf(p, sizeof(p), "%s/long", base); n += rmdir(p) == 0;
	n += rmdir(base) == 0;
	check(n == 5, "every test directory removes cleanly");
	printf("%d check(s) failed\n", fails);
	return fails;
}
