/*
 * test/unit/col/skip/build.c - the skip index of a part (col/skip/build.c) reads no string column past the rows of its part, and the trigrams of a wide string take a cancel.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "col/skip/build.c"

#define WIDE_BYTES	(1 << 20)
#define WIDE_ROWS	SIFT_SKIP_FILTER_MIN_ROWS
#define CANCEL_EVERY	65536

volatile sig_atomic_t InterruptPending = 0;
static int	interrupts;

void
ProcessInterrupts(void)
{
	interrupts++;
}

void
appendStringInfoSpaces(StringInfo s, int n)
{
	while (n-- > 0)
		appendStringInfoChar(s, ' ');
}

SiftColIn
sift_colin_slice(const SiftColIn *c, int from)
{
	SiftColIn	s = *c;

	s.s = c->s + from;
	s.slen = c->slen + from;
	return s;
}

static SiftAuxBudget aux = {.cap = UINT64_MAX,.rem = UINT64_MAX,.d_total = UINT64_MAX / 2,.d_comp = UINT64_MAX / 2,.nrows = SIFT_AUX_SKIP_MIN_ROWS};

static bool
build(int nrows, const char *const *s, const uint32 *slen)
{
	SiftColIn	col = {.kind = SIFT_CELL_STR,.s = s,.slen = slen,.hint = SIFT_CHINT_AUTO};
	SiftRows	rows = {.nrows = nrows,.ncols = 1,.cols = &col};
	int			gstart[2] = {0, nrows};
	StringInfoData out;

	initStringInfo(&out);
	return sift_skip_build(&rows, gstart, 1, NULL, NULL, &aux, &out);
}

/* A part of 16 rows: the average length of its strings is taken from those 16, in arrays of their size (AddressSanitizer reports a read past them) */
static void
test_avg_len_stays_in_the_part(void)
{
	enum { ROWS = 16 };
	const char **s = palloc(ROWS * sizeof(char *));
	uint32	   *slen = palloc(ROWS * sizeof(uint32));
	char		v[ROWS][32];

	for (int r = 0; r < ROWS; r++)
	{
		snprintf(v[r], sizeof(v[r]), "a value of row %08d", r);
		s[r] = v[r];
		slen[r] = strlen(v[r]);
	}
	SIFT_CHECKF(build(ROWS, s, slen), "a part of %d rows keeps the exact set of its strings, read within them", ROWS);
}

/* One string of 1 MB in a granule of the fewest rows a filter is built for: its trigrams are hashed with a look for a cancel every 64 kB */
static void
test_wide_string_takes_a_cancel(void)
{
	const char **s = palloc(WIDE_ROWS * sizeof(char *));
	uint32	   *slen = palloc(WIDE_ROWS * sizeof(uint32));
	char	   *wide = palloc(WIDE_BYTES);
	char		v[WIDE_ROWS][24];

	memset(wide, 'a', WIDE_BYTES);
	for (int r = 0; r < WIDE_ROWS; r++)
	{
		snprintf(v[r], sizeof(v[r]), "row%012d", r);
		s[r] = v[r];
		slen[r] = strlen(v[r]);
	}
	s[0] = wide;
	slen[0] = WIDE_BYTES;
	interrupts = 0;
	InterruptPending = 1;
	build(WIDE_ROWS, s, slen);
	InterruptPending = 0;
	SIFT_CHECKF(interrupts >= WIDE_BYTES / CANCEL_EVERY, "the trigrams of a string of %d bytes looked for a cancel %d times", WIDE_BYTES, interrupts);
}

int
main(void)
{
	test_avg_len_stays_in_the_part();
	test_wide_string_takes_a_cancel();
	return sift_check_summary();
}
