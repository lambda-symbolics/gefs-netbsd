/*
 * fsyncgrp: threads that each write a record and fsync it, in a loop,
 * to see how many commits their fsyncs share, and to check after a
 * crash that every record whose fsync returned is on the disk.
 *
 *	fsyncgrp [-t threads] [-n fsyncs | -s seconds] [-b recsize] [-a] dir
 *	fsyncgrp -v acklog [-b recsize] dir
 *
 * Thread t writes record i (recsize bytes of a pattern of t and i) at
 * offset i*recsize of dir/f.t, then fsyncs.  With -a it prints "ack t i"
 * on stdout once that fsync has returned.  -v reads such a log (from a
 * run that was cut off by a crash) and checks every acknowledged record;
 * records after the last acknowledged one may be there or not.
 *
 * On NetBSD it reports vfs.gefs.{commits,flushes,writes} around the run
 * and the fsync latencies.
 */
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __NetBSD__
#include <sys/sysctl.h>
#endif
#include <err.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXT	256

static int	nthr = 4, ack;
static long	nfs = 100, secs, recsz = 4096;
static char	*dir;
static pthread_barrier_t	bar;
static pthread_mutex_t	outlk = PTHREAD_MUTEX_INITIALIZER;
static volatile int	stop;

struct thr {
	pthread_t	tid;
	int		t;
	int		fd;
	long		n;
	double		*lat;
	long		nlat;
};

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
fillrec(char *b, int t, long i)
{
	char c[32];
	long k;

	snprintf(c, sizeof(c), "%03d:%010ld|", t % 1000, i % 10000000000L);
	for (k = 0; k < recsz; k++)
		b[k] = c[k % 15];
	b[recsz - 1] = '\n';
}

static long long
gstat(const char *name)
{
#ifdef __NetBSD__
	char buf[64];
	long long v;
	size_t len = sizeof(v);

	snprintf(buf, sizeof(buf), "vfs.gefs.%s", name);
	if (sysctlbyname(buf, &v, &len, NULL, 0) == -1)
		return -1;
	return v;
#else
	(void)name;
	return -1;
#endif
}

static void *
run(void *arg)
{
	struct thr *th = arg;
	char *b;
	double t0;
	long i;

	b = malloc(recsz);
	pthread_barrier_wait(&bar);
	for (i = 0; secs ? !stop : i < nfs; i++) {
		fillrec(b, th->t, i);
		if (pwrite(th->fd, b, recsz, i * recsz) != recsz)
			err(1, "pwrite");
		t0 = now();
		if (fsync(th->fd) == -1)
			err(1, "fsync");
		if (th->nlat < nfs)
			th->lat[th->nlat++] = now() - t0;
		if (ack) {
			pthread_mutex_lock(&outlk);
			printf("ack %d %ld\n", th->t, i);
			fflush(stdout);
			pthread_mutex_unlock(&outlk);
		}
	}
	th->n = i;
	free(b);
	return NULL;
}

static int
dcmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static int
verify(const char *log)
{
	long last[MAXT], i, n, good, extra, bad;
	char path[1024], *b, *g;
	FILE *f;
	int t, fd;

	for (t = 0; t < MAXT; t++)
		last[t] = -1;
	if ((f = fopen(log, "r")) == NULL)
		err(1, "%s", log);
	while (fscanf(f, "ack %d %ld\n", &t, &i) == 2)
		if (t >= 0 && t < MAXT && i > last[t])
			last[t] = i;
	fclose(f);
	b = malloc(recsz);
	g = malloc(recsz);
	good = extra = bad = 0;
	for (t = 0; t < MAXT; t++) {
		if (last[t] < 0)
			continue;
		snprintf(path, sizeof(path), "%s/f.%d", dir, t);
		if ((fd = open(path, O_RDONLY)) == -1) {
			warn("%s (acknowledged up to %ld)", path, last[t]);
			bad += last[t] + 1;
			continue;
		}
		for (i = 0; ; i++) {
			n = pread(fd, b, recsz, i * recsz);
			fillrec(g, t, i);
			if (n == recsz && memcmp(b, g, recsz) == 0) {
				if (i <= last[t])
					good++;
				else
					extra++;
				continue;
			}
			if (i <= last[t]) {
				warnx("%s: record %ld lost (acknowledged up to %ld)",
				    path, i, last[t]);
				bad++;
				continue;
			}
			break;
		}
		close(fd);
	}
	printf("verify: %ld acknowledged records intact, %ld lost; "
	    "%ld more not acknowledged but there\n", good, bad, extra);
	return bad != 0;
}

int
main(int argc, char **argv)
{
	struct thr th[MAXT];
	long long c0, c1, f0, f1, w0, w1;
	char path[1024], *vlog;
	double t0, t1, *all;
	long tot, nall, i;
	int ch, t;

	vlog = NULL;
	while ((ch = getopt(argc, argv, "ab:n:s:t:v:")) != -1) {
		switch (ch) {
		case 'a': ack = 1; break;
		case 'b': recsz = strtol(optarg, NULL, 0); break;
		case 'n': nfs = strtol(optarg, NULL, 0); break;
		case 's': secs = strtol(optarg, NULL, 0); break;
		case 't': nthr = atoi(optarg); break;
		case 'v': vlog = optarg; break;
		default:
		usage:
			fprintf(stderr, "usage: fsyncgrp [-t threads] [-n fsyncs | -s seconds] "
			    "[-b recsize] [-a] dir\n       fsyncgrp -v acklog [-b recsize] dir\n");
			return 2;
		}
	}
	if (argc - optind != 1 || nthr < 1 || nthr > MAXT || recsz < 16)
		goto usage;
	dir = argv[optind];
	if (vlog != NULL)
		return verify(vlog);
	if (secs)
		nfs = 1000000;

	for (t = 0; t < nthr; t++) {
		th[t].t = t;
		th[t].n = th[t].nlat = 0;
		th[t].lat = calloc(nfs, sizeof(double));
		snprintf(path, sizeof(path), "%s/f.%d", dir, t);
		if ((th[t].fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)) == -1)
			err(1, "%s", path);
	}
	sync();
	sleep(1);
	pthread_barrier_init(&bar, NULL, nthr + 1);
	for (t = 0; t < nthr; t++)
		pthread_create(&th[t].tid, NULL, run, &th[t]);
	c0 = gstat("commits");
	f0 = gstat("flushes");
	w0 = gstat("writes");
	pthread_barrier_wait(&bar);
	t0 = now();
	if (secs) {
		sleep(secs);
		stop = 1;
	}
	tot = 0;
	for (t = 0; t < nthr; t++) {
		pthread_join(th[t].tid, NULL);
		tot += th[t].n;
	}
	t1 = now();
	c1 = gstat("commits");
	f1 = gstat("flushes");
	w1 = gstat("writes");

	all = calloc(nthr * nfs, sizeof(double));
	nall = 0;
	for (t = 0; t < nthr; t++)
		for (i = 0; i < th[t].nlat; i++)
			all[nall++] = th[t].lat[i];
	qsort(all, nall, sizeof(double), dcmp);
	fprintf(ack ? stderr : stdout,
	    "threads %d fsyncs %ld in %.2f s (%.0f/s); commits %lld flushes %lld "
	    "writes %lld; fsyncs per commit %.2f; latency ms p50 %.2f p99 %.2f max %.2f\n",
	    nthr, tot, t1 - t0, tot / (t1 - t0), c1 - c0, f1 - f0, w1 - w0,
	    c1 > c0 ? (double)tot / (c1 - c0) : 0.0,
	    nall ? all[nall / 2] * 1e3 : 0, nall ? all[nall * 99 / 100] * 1e3 : 0,
	    nall ? all[nall - 1] * 1e3 : 0);
	return 0;
}
