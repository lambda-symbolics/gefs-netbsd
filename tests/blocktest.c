/* Keep readblk/getarena in this translation unit so static decoder paths are covered. */
#include "../sys/fs/gefs/blk.c"
#include "../sbin/gefs/host.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprint(2, "FAIL %s:%d: ", __FILE__, __LINE__); fprint(2, __VA_ARGS__); fprint(2, "\n"); fails++; } } while (0)

static int
caught(void (*fn)(void *), void *arg)
{
	if (waserror()) { fprint(2, "caught: %s\n", errmsg()); return 1; }
	fn(arg);
	poperror();
	return 0;
}

static void
readone(void *arg)
{
	Blk b;
	memset(&b, 0, sizeof b);
	b.magic = Magic;
	readblk(&b, *(Bptr *)arg, GBnochk, 0);
}

static int fd;
static char image[] = "/tmp/gefs-blocktest-XXXXXX";

static void
putblock(vlong off, void (*fill)(char *))
{
	char b[Blksz];
	memset(b, 0, sizeof b);
	fill(b);
	CHECK(pwrite(fd, b, sizeof b, off) == sizeof b, "write block");
}

static void
badlog65535(char *b)
{
	PACK16(b, Tlog); PACK16(b+2, 65535);
}
static void
trunlog(char *b)
{
	PACK16(b, Tlog); PACK16(b+2, 8);
	PACK64(b+Loghdsz, (uvlong)LogAlloc); /* missing length word */
}
static void
validlog(char *b)
{
	PACK16(b, Tlog); PACK16(b+2, 8);
	PACK64(b+Loghdsz, 0x4000ULL | LogAlloc1);
}
static void
validdlist(char *b)
{
	PACK16(b, Tdlist); PACK16(b+2, 8);
	PACK64(b+Loghdsz, 0x4000ULL);
}
static void
badleaf(char *b)
{
	PACK16(b, Tleaf); PACK16(b+2, 1); PACK16(b+4, 4);
	PACK16(b+Leafhdsz, 0); /* offset points into the offset table */
}
static void
validleaf(char *b)
{
	Blk t;
	Kvp kv;
	memset(&t, 0, sizeof(t));
	t.type = Tleaf;
	t.data = t.buf + Leafhdsz;
	kv.k = "a"; kv.nk = 1; kv.v = "value"; kv.nv = 5;
	setval(&t, &kv);
	kv.k = "b"; kv.v = "other";
	setval(&t, &kv);
	PACK16(t.buf, Tleaf);
	PACK16(t.buf+2, t.nval); PACK16(t.buf+4, t.valsz);
	memcpy(b, t.buf, Blksz);
}
static void
validpivot(char *b)
{
	int o = Bufspc - 6;
	int vo = Pivspc - (5+Pptrsz);
	PACK16(b, Tpivot); PACK16(b+2, 1); PACK16(b+4, 5+Pptrsz);
	PACK16(b+6, 1); PACK16(b+8, 6);
	PACK16(b+Pivhdsz, vo);
	PACK16(b+Pivhdsz+vo, 1); b[Pivhdsz+vo+2] = 'k';
	PACK16(b+Pivhdsz+vo+3, Pptrsz);
	PACK64(b+Pivhdsz+vo+5, Blksz);
	PACK16(b+Pivhdsz+Pivspc, o);
	b[Pivhdsz+Pivspc+o+0] = Oinsert;
	PACK16(b+Pivhdsz+Pivspc+o+1, 1); b[Pivhdsz+Pivspc+o+3] = 'k';
	PACK16(b+Pivhdsz+Pivspc+o+4, 0);
}

static void
run_read_tests(void)
{
	Bptr bp = { 0, -1, -1 };
	Blk b;

	putblock(0, badlog65535); CHECK(caught(readone, &bp), "65535 log size accepted");
	putblock(0, trunlog); CHECK(caught(readone, &bp), "truncated log accepted");
	putblock(0, validlog); CHECK(!caught(readone, &bp), "valid log rejected");
	putblock(0, validdlist); CHECK(!caught(readone, &bp), "valid deadlist rejected");
	putblock(0, badleaf); CHECK(caught(readone, &bp), "bad leaf offset accepted");
	putblock(0, validleaf); CHECK(!caught(readone, &bp), "valid leaf rejected");
	putblock(0, validpivot); CHECK(!caught(readone, &bp), "valid pivot rejected");
	bp.addr = 1; CHECK(caught(readone, &bp), "unaligned address accepted");
	bp.addr = -Blksz; CHECK(caught(readone, &bp), "negative address accepted");
	bp.addr = Blksz; CHECK(caught(readone, &bp), "short/OOB block read accepted");
	USED(b);
}

static void
run_getarena(void *unused)
{
	USED(unused);
	getarena(*(vlong *)unused);
}

int
main(void)
{
	Gefs *g;
	Arena arena;
	Blk sb0, sb1, h0, h1;
	vlong below, above;
	fd = mkstemp(image);
	CHECK(fd >= 0, "mkstemp: %r");
	if (fd < 0) return 1;
	CHECK(ftruncate(fd, Blksz) == 0, "ftruncate: %r");
	hostinit(8*MiB, 0);
	g = fs;
	g->fd = fd;
	run_read_tests();
	/* The binary-search path must reject addresses outside valid arena ranges. */
	memset(&arena, 0, sizeof arena);
	memset(&sb0, 0, sizeof sb0); memset(&sb1, 0, sizeof sb1);
	memset(&h0, 0, sizeof h0); memset(&h1, 0, sizeof h1);
	g->sb0 = &sb0; g->sb1 = &sb1; g->narena = 1; g->arenas = &arena;
	sb0.bp.addr = 0; sb1.bp.addr = 10*Blksz;
	arena.h0 = &h0; arena.h1 = &h1; arena.size = 2*Blksz;
	h0.bp.addr = 2*Blksz; h1.bp.addr = 4*Blksz;
	below = Blksz; above = 12*Blksz;
	CHECK(caught(run_getarena, &below), "getarena accepted address below arena");
	CHECK(caught(run_getarena, &above), "getarena accepted address above arena");
	close(fd);
	remove(image);
	return fails != 0;
}
