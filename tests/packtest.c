/*
 * Boundary tests for disk metadata decoders.
 * Run: make -C sbin/gefs -f Makefile.host check-boundaries
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>
#include "dat.h"
#include "fns.h"
#include "host.h"

static int failures;

#define CHECK(c, ...) do { \
	if(!(c)){ fprint(2, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprint(2, __VA_ARGS__); fprint(2, "\n"); failures++; } \
} while(0)

static int
badstr(void)
{
	char b[3] = { 0, 4, 0 }, *s;
	unpackstr(b, b+sizeof b, &s);
	return 0;
}

static int
badbp(void)
{
	char b[Ptrsz-1];
	unpackbp(b, sizeof b);
	return 0;
}

static int
baddval(void)
{
	Xdir d;
	char b[1];
	dval2dir(b, -7, &d);
	return 0;
}

static int
badcount(void)
{
	char b[Blksz];
	Gefs f;
	memset(b, 0, sizeof b);
	memset(&f, 0, sizeof f);
	memcpy(b, "gefs9.00", 8);
	PACK32(b+8, Blksz);
	PACK32(b+12, Bufspc);
	PACK32(b+16, (Blksz-112)/16+1);
	unpacksb(&f, b, sizeof b);
	return 0;
}

static int
traps(int (*fn)(void))
{
	if(waserror())
		return 1;
	fn();
	poperror();
	return 0;
}

int
main(void)
{
	hostinit(32*MiB, 1);
	CHECK(traps(badstr), "short/unterminated string was accepted");
	CHECK(traps(badbp), "short block pointer was accepted");
	CHECK(traps(baddval), "negative directory value size was accepted");
	CHECK(traps(badcount), "superblock arena count was accepted");
	return failures != 0;
}
