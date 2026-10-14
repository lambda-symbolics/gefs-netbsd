/* Boundary tests for snapshot retention parsing. */
#include "../sys/fs/gefs/snapadm.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprint(2, "FAIL %s:%d: ", __FILE__, __LINE__); fprint(2, __VA_ARGS__); fprint(2, "\n"); fails++; } } while (0)

static void
accepted(char *s)
{
	Cron c[3];
	memset(c, 0, sizeof c);
	CHECK(parseretain(s, c), "parseretain(%s) rejected valid input", s);
}

static void
rejected(char *s)
{
	Cron c[3];
	memset(c, 0, sizeof c);
	CHECK(!parseretain(s, c), "parseretain(%s) accepted invalid input", s);
}

int
main(void)
{
	Cron c[3];
	accepted("0@m");
	accepted("@m");
	memset(c, 0, sizeof c);
	CHECK(parseretain("2@m", c), "minute schedule rejected");
	CHECK(c[0].cnt == 2 && c[0].div == 60, "minute count/interval wrong");
	memset(c, 0, sizeof c);
	CHECK(parseretain("1@d 2@h", c), "mixed schedule rejected");
	CHECK(c[2].cnt == 1 && c[2].div == 86400, "day count/interval wrong");
	CHECK(c[1].cnt == 2 && c[1].div == 3600, "hour count/interval wrong");
	memset(c, 0, sizeof c);
	CHECK(parseretain("65536@m", c), "maximum count rejected");
	CHECK(c[0].cnt == 65536 && c[0].div == 60, "maximum count wrong");
	rejected("65537@m");
	rejected("9223372036854775808@m");
	rejected("1@9223372036854775808m");
	rejected("1@153722867280912930h");
	rejected("1@x");
	rejected("1@m trailing");
	return fails != 0;
}
