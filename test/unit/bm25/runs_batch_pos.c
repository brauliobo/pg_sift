/*
 * test/unit/bm25/runs_batch_pos.c - the position records a batch of the merge of runs (bm25/runs/merge.c) holds: the batch goes to the writer once they
 * pass a bound, not only once it has BATCH postings (BATCH postings of documents with large records grew the array past 1 GB: "invalid memory alloc
 * request size").
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/runs/merge.c"

#define REC_LEN		(2 * 1024 * 1024)
#define NPOST		(BATCH - 1)

static int64 written;
static Size written_pos;
static Batch bt;

/* The heap of the merge is not reached; defined here so that the frontend one of libpgcommon, which brings its own palloc, is not linked */
binaryheap *
binaryheap_allocate(int capacity, binaryheap_comparator compare, void *arg)
{
	pgshim_unreached("binaryheap_allocate");
}

void
binaryheap_free(binaryheap *heap)
{
	pgshim_unreached("binaryheap_free");
}

void
binaryheap_add_unordered(binaryheap *heap, bh_node_type d)
{
	pgshim_unreached("binaryheap_add_unordered");
}

void
binaryheap_build(binaryheap *heap)
{
	pgshim_unreached("binaryheap_build");
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

bh_node_type
binaryheap_remove_first(binaryheap *heap)
{
	pgshim_unreached("binaryheap_remove_first");
}

void
binaryheap_replace_first(binaryheap *heap, bh_node_type d)
{
	pgshim_unreached("binaryheap_replace_first");
}

void
sift_postw_add(SiftPostingWriter *w, const SiftKey *keys, const uint8 *tf, const uint16 *dl, int n, const uint8 *pos, const uint32 *poslen)
{
	written += n;
	for (int i = 0; i < n; i++)
		written_pos += poslen[i];
}

static void
put_large_records(void *arg)
{
	const uint8 *rec = arg;

	for (int i = 0; i < NPOST; i++)
	{
		batch_put(&bt, i + 1, 1, 1, rec, REC_LEN);
		SIFT_CHECKF(bt.poscap <= 2 * REC_LEN + BATCH_POS_KEEP, "the batch holds %zu bytes of positions after %d postings", bt.poscap, i + 1);
		if (bt.poscap > 2 * REC_LEN + BATCH_POS_KEEP)
			return;
	}
	batch_flush(&bt);
}

int
main(void)
{
	uint8	   *rec = calloc(1, REC_LEN);

	bt.haspos = true;
	SIFT_CHECKF(pgshim_try(put_large_records, rec), "the batch failed: %s", pgshim_errmsg);
	SIFT_CHECKF(written == NPOST && written_pos == (Size) NPOST * REC_LEN, "%lld postings with %zu bytes of positions reached the writer",
				(long long) written, written_pos);
	free(rec);
	return sift_check_summary();
}
