/*
 * test/unit/bm25/keyed_huge.c - the rows of a round of a scan ordered by keys and score (bm25/cscan/keyed.c) when work_mem holds more than 32 M ties of
 * the last key: the rows take the huge allocation (a plain one failed with "invalid memory alloc request size" under a work_mem the budget of the round
 * allowed), and the scoring of the rows answers a cancel.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/cscan/keyed.c"

#define NTIE	((int64) (MaxAllocSize / sizeof(KeyRow) + 1))

volatile sig_atomic_t InterruptPending = false;
static int	nprocessed;

void
ProcessInterrupts(void)
{
	nprocessed++;
	InterruptPending = false;
}

/* The heap is built by hand; defined here so that the frontend one of libpgcommon, which brings its own palloc, is not linked */
binaryheap *
binaryheap_allocate(int capacity, binaryheap_comparator compare, void *arg)
{
	pgshim_unreached("binaryheap_allocate");
}

void
binaryheap_add(binaryheap *heap, bh_node_type d)
{
	pgshim_unreached("binaryheap_add");
}

bh_node_type
binaryheap_first(binaryheap *heap)
{
	pgshim_unreached("binaryheap_first");
}

void
binaryheap_replace_first(binaryheap *heap, bh_node_type d)
{
	pgshim_unreached("binaryheap_replace_first");
}

int32
ItemPointerCompare(ItemPointer a, ItemPointer b)
{
	uint64		x = (uint64) ItemPointerGetBlockNumberNoCheck(a) << 16 | ItemPointerGetOffsetNumberNoCheck(a),
				y = (uint64) ItemPointerGetBlockNumberNoCheck(b) << 16 | ItemPointerGetOffsetNumberNoCheck(b);

	return x < y ? -1 : x > y;
}

static KeyRow **rows;
static int64 nrows,
			ranked;

SiftFilter *
sift_filter_create(MemoryContext mcxt)
{
	return NULL;
}

void
sift_filter_add_page(SiftFilter *f, BlockNumber blk, const uint64 *offmask)
{
}

void
sift_plan_set_filter(SiftPlan *plan, const SiftFilter *filter)
{
}

SiftRanker *
sift_rank_begin(SiftPlan *plan, int64 k, SiftVerifyFn verify, void *arg)
{
	ranked = 0;
	return NULL;
}

bool
sift_rank_next(SiftRanker *r, ItemPointer tid, float4 *score)
{
	if (ranked == nrows)
		return false;
	*tid = rows[ranked++]->tid;
	*score = 1;
	return true;
}

void
sift_rank_end(SiftRanker *r)
{
}

/* One row in the heap and NTIE ties of its keys, in order of entry */
static KeyTop t;

static void
rows_of(void *arg)
{
	rows = keytop_rows(&t, &nrows);
}

static void
check_rows(void)
{
	static KeyRow worst;
	binaryheap *heap = palloc(offsetof(binaryheap, bh_nodes) + sizeof(bh_node_type));

	ItemPointerSet(&worst.tid, 0, 1);
	*heap = (binaryheap) {.bh_size = 1,.bh_space = 1};
	heap->bh_nodes[0] = PointerGetDatum(&worst);
	t = (KeyTop) {.heap = heap,.tie = MemoryContextAllocHuge(CurrentMemoryContext, NTIE * sizeof(ItemPointerData)),.ntie = NTIE,.captie = NTIE};
	for (int64 i = 0; i < NTIE; i++)
		ItemPointerSet(&t.tie[i], 1 + i / MaxHeapTuplesPerPage, 1 + i % MaxHeapTuplesPerPage);
	SIFT_CHECKF(pgshim_try(rows_of, NULL), "the rows of %lld ties failed: %s", (long long) NTIE, pgshim_errmsg);
	SIFT_CHECKF(nrows == NTIE + 1, "%lld rows", (long long) nrows);
}

static void
check_cancel(void)
{
	static KeyRow r[3];
	static KeyRow *p[3] = {&r[0], &r[1], &r[2]};
	SiftScanState ss = {0};

	for (int i = 0; i < 3; i++)
		ItemPointerSet(&r[i].tid, i, 1);
	rows = p;
	nrows = 3;
	nprocessed = 0;
	InterruptPending = true;
	keyed_score(&ss, rows, nrows);
	SIFT_CHECKF(nprocessed > 0, "the rows of a round were scored with a cancel pending");
}

int
main(void)
{
	check_cancel();
	check_rows();
	return sift_check_summary();
}
