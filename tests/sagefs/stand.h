/*
 * Just enough of libsa's stand.h to build sys/lib/libsa/gefs.c in
 * userland, on NetBSD or Linux, for sagefs(1).
 */
#ifndef SAGEFS_STAND_H
#define SAGEFS_STAND_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/endian.h>
#ifndef __arraycount
#define __arraycount(a)	(sizeof(a) / sizeof((a)[0]))
#endif

#define __compactcall
#define LIBSA_ENABLE_LS_OP	1

struct open_file;
struct devsw {
	const char *dv_name;
	int (*dv_strategy)(void *, int, daddr_t, size_t, void *, size_t *);
};
struct open_file {
	int		f_flags;
	const struct devsw *f_dev;
	void		*f_devdata;
	void		*f_fsdata;
};

#define F_READ		0x0001
#define DEV_STRATEGY(d)	((d)->dv_strategy)
#define GETSECSIZE(f)	512

#define FS_DEF(fs) \
	int fs##_open(const char *, struct open_file *); \
	int fs##_close(struct open_file *); \
	int fs##_read(struct open_file *, void *, size_t, size_t *); \
	int fs##_write(struct open_file *, void *, size_t, size_t *); \
	off_t fs##_seek(struct open_file *, off_t, int); \
	int fs##_stat(struct open_file *, struct stat *); \
	void fs##_ls(struct open_file *, const char *)

void	*alloc(size_t);
void	dealloc(void *, size_t);
void	twiddle(void);

#endif
