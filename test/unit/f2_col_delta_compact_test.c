/*
 * test/unit/f2_col_delta_compact_test.c - the plan of a garbage collection of a column delta chain (table/f2/f2_col_delta_compact.c): a record every attribute of which a later record of the
 * row writes again is dropped when both writers committed before the horizon; one the later record does not cover, and one whose later writer is still in the horizon's reach, stays.
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

#include "table/f2/f2_col_delta.c"
#include "table/f2/f2_col_delta_compact.c"

enum { PART = 7, RUNNING_FROM = 100 };

static SiftHorizonStatus
status_of(void *ctx, TransactionId xid, FullTransactionId *fxid)
{
	*fxid = FullTransactionIdFromEpochAndXid(0, xid);
	return xid < RUNNING_FROM ? SIFT_H_COMMITTED_OLD : SIFT_H_RUNNING;
}

typedef struct Chain
{
	char	   *page;
	uint32		used;
	int			n;
} Chain;

/* A record of row ordinal `row` writing the attributes of the mask */
static void
put(Chain *c, uint16 row, TransactionId xmin, uint32 mask)
{
	SiftColDeltaCol cols[SIFT_COL_DELTA_MAX_COLS_PER_REC] = {0};
	SiftColDeltaTarget t = {.part_gen = PART,.granule_id = 0,.row_ordinal = row,.tid = ((uint64) 1 << 16) | (row + 1)};
	int			n = 0;
	uint32		bytes;

	for (int a = 1; a < 16; a++)
		if (mask & (1u << a))
		{
			cols[n].attnum = (int16) a;
			cols[n].kind = SIFT_CELL_INT;
			cols[n].cell.i = xmin + a;
			n++;
		}
	bytes = sift_col_delta_encode((uint8 *) c->page + SIFT_PAGE_DATA_OFFSET + c->used, SIFT_COL_DELTA_MAX_REC_BYTES, &t, xmin, 0, 0, cols, n);
	CHECK(bytes > 0, "the record encodes");
	c->used += bytes;
	c->n++;
	SiftPageSetUsed(c->page, c->used);
}

static void
test_superseded(void)
{
	Chain		c = {.page = calloc(1, BLCKSZ),.used = sizeof(SiftColDeltaPageData)};
	SiftColDeltaChain chain = {.npages = 1,.arena = c.page};
	SiftColDeltaHorizon hz = {.ctx = NULL,.status = status_of};
	SiftColDeltaCompactPlan plan;
	uint64		live[1] = {PART};

	SiftPageSetUsed(c.page, c.used);
	put(&c, 1, 10, 1u << 3);
	put(&c, 1, 11, 1u << 3);
	put(&c, 1, 12, (1u << 3) | (1u << 4));
	put(&c, 2, 10, 1u << 5);
	put(&c, 2, 150, 1u << 5);
	put(&c, 3, 10, (1u << 3) | (1u << 4));
	put(&c, 3, 11, 1u << 3);
	put(&c, 4, 10, 1u << 3);
	put(&c, 4, 11, 1u << 4);
	put(&c, 4, 12, (1u << 3) | (1u << 4));
	sift_col_delta_compact_plan(&chain, NULL, 0, live, 1, &hz, UINT32_MAX, &plan);
	CHECK(plan.nrecords == c.n, "%d records planned, %d put", plan.nrecords, c.n);
	CHECK(plan.ndropped == 4, "the 4 superseded records go: %d", plan.ndropped);
	CHECK(plan.ncarry == 6, "the other 6 stay: %d", plan.ncarry);
	sift_col_delta_compact_plan_free(&plan);
	free(c.page);
}

int
main(void)
{
	test_superseded();
	printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures != 0;
}
