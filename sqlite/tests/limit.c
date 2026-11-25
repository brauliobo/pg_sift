/*
 * tests/limit.c - the rows a ranked scan has to produce for LIMIT and OFFSET (cursor.c sq_rank_rows): the sum never overflows, a bounded query stays
 * bounded, LIMIT 0 is no rows (0) and a negative LIMIT is every row (-1).
 */
#include <stdio.h>

#include "vtab_int.h"

static int	failed;

#define CHECK(c) do { if (!(c)) { failed++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define BIG		INT64_MAX
#define BOUNDED(r)	((r) >= 1000000 && (r) <= SQ_RANK_ROWS_MAX)

int
main(void)
{
	CHECK(sq_rank_rows(10, 0) == 10);
	CHECK(sq_rank_rows(10, 20) == 30);
	CHECK(sq_rank_rows(10, -3) == 10);
	CHECK(sq_rank_rows(0, 0) == 0);
	CHECK(sq_rank_rows(0, 5) == 0);
	CHECK(sq_rank_rows(-1, 0) == -1);
	CHECK(sq_rank_rows(-1, 5) == -1);
	CHECK(sq_rank_rows(INT64_MIN, 0) == -1);
	CHECK(sq_rank_rows(INT64_MIN, BIG) == -1);
	CHECK(BOUNDED(sq_rank_rows(10, BIG)));
	CHECK(BOUNDED(sq_rank_rows(BIG, 1)));
	CHECK(BOUNDED(sq_rank_rows(BIG, BIG)));
	CHECK(BOUNDED(sq_rank_rows(BIG, 0)));
	CHECK(BOUNDED(sq_rank_rows(SQ_RANK_ROWS_MAX, 1)));
	printf("limit_test: %s\n", failed ? "FAILED" : "ok");
	return failed != 0;
}
