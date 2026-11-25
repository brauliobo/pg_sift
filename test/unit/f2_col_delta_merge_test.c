/*
 * test/unit/f2_col_delta_merge_test.c - the merge of the records one transaction staged for one row (table/f2/col_delta/coalesce.c): the record it makes is one the checks of a page accept
 * (the columns in ascending order of attribute) and holds the later value of every column, whatever order the two records touched their columns in.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/unit/pg_shim.h"
#include "test/unit/harness.h"

bool		sift_test_col_delta = true;

#include "table/f2/col_delta/col_delta.c"
#include "table/f2/col_delta/coalesce.c"

enum { MAX_ATT = 24, ROUNDS = 400 };

static const SiftColDeltaTarget row = {.part_gen = 7,.granule_id = 3,.row_ordinal = 11,.tid = ((uint64) 5 << 16) | 2};

/* A record of the row that sets each attribute of the mask to base + the attribute */
static uint32
make(uint8 *buf, uint32 mask, int64 base, TransactionId xmin)
{
	SiftColDeltaCol cols[SIFT_COL_DELTA_MAX_COLS_PER_REC] = {0};
	int			n = 0;

	for (int a = 1; a < MAX_ATT; a++)
		if (mask & (1u << a))
		{
			cols[n].attnum = (int16) a;
			cols[n].kind = SIFT_CELL_INT;
			cols[n].cell.i = base + a;
			n++;
		}
	return sift_col_delta_encode(buf, SIFT_COL_DELTA_MAX_REC_BYTES, &row, xmin, 0, 0, cols, n);
}

/* The merge holds the attributes of the union, in ascending order, with the value of the second where both have one, and passes the check of a page */
static void
check_merge(uint32 m1, uint32 m2, const char *what)
{
	uint8		a[SIFT_COL_DELTA_MAX_REC_BYTES],
				b[SIFT_COL_DELTA_MAX_REC_BYTES],
				out[SIFT_COL_DELTA_MAX_REC_BYTES];
	uint32		na = make(a, m1, 100, 10),
				nb = make(b, m2, 200, 11),
				n = sift_col_delta_merge_records(out, sizeof(out), a, b);
	const char *why;
	SiftColDeltaEntryHdr h;
	const uint8 *p;
	uint32		want = m1 | m2;
	int			k = 0;

	CHECK(na > 0 && nb > 0, "%s: the records were encoded", what);
	CHECK(n > 0, "%s: the records merge", what);
	if (n == 0)
		return;
	why = sift_col_delta_rec_check(out, n);
	CHECK(why == NULL, "%s: the merged record passes the page check: %s", what, why != NULL ? why : "");
	memcpy(&h, out, sizeof(h));
	CHECK(h.ncols == __builtin_popcount(want), "%s: %d columns, wanted %d", what, h.ncols, __builtin_popcount(want));
	p = out + sizeof(h);
	for (int at = 1; at < MAX_ATT && p != NULL; at++)
	{
		SiftColDeltaPatch col;

		if (!(want & (1u << at)))
			continue;
		p = sift_col_delta_decode_col(p, out + h.rec_bytes, &col);
		CHECK(p != NULL && col.attnum == at, "%s: column %d is attribute %d", what, k, at);
		if (p != NULL)
			CHECK(col.val.i == ((m2 & (1u << at)) ? 200 : 100) + at, "%s: attribute %d holds %lld", what, at, (long long) col.val.i);
		k++;
	}
}

static void
test_merge_order(void)
{
	check_merge(1u << 5, 1u << 3, "5 then 3");
	check_merge(1u << 3, 1u << 5, "3 then 5");
	check_merge((1u << 3) | (1u << 5), (1u << 5) | (1u << 7), "3,5 then 5,7");
	check_merge((1u << 9) | (1u << 2), (1u << 4) | (1u << 1) | (1u << 9), "9,2 then 4,1,9");
	check_merge(1u << 6, 1u << 6, "6 then 6");
	for (int i = 0; i < ROUNDS; i++)
	{
		uint32		m1 = (uint32) rnd() & ((1u << MAX_ATT) - 1) & ~1u,
					m2 = (uint32) rnd() & ((1u << MAX_ATT) - 1) & ~1u;

		if (__builtin_popcount(m1) > 8 || __builtin_popcount(m2) > 8)
			continue;
		check_merge(m1, m2, "random");
	}
}

int
main(void)
{
	test_merge_order();
	printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures != 0;
}
