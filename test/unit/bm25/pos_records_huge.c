/*
 * test/unit/bm25/pos_records_huge.c - the position record of one posting past 512 MB: the chunk the inverted buffer copies it to (bm25/inv.c) and the
 * record a run reader assembles (bm25/runs/reader.c) grow with the huge allocation (a doubling past 1 GB failed with "invalid memory alloc request
 * size", and the build or the merge of a large doc failed for good).
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/inv.c"
#include "bm25/runs/reader.c"

#define FULL	(MaxAllocSize / 2 + 1)

static PosChunk pc;
static RunReader rd;

static void
chunk(void *arg)
{
	chunk_put(&pc, 7);
}

static void
record(void *arg)
{
	record_put(&rd, 7);
}

int
main(void)
{
	pc = (PosChunk) {.buf = palloc(FULL),.len = FULL,.cap = FULL};
	SIFT_CHECKF(pgshim_try(chunk, NULL), "a chunk of %zu bytes failed to grow: %s", (size_t) FULL, pgshim_errmsg);
	SIFT_CHECKF(pc.len == FULL + 1 && pc.buf[FULL] == 7, "the chunk holds %zu bytes", pc.len);
	pfree(pc.buf);
	rd = (RunReader) {.prec = palloc(FULL),.prlen = FULL,.prcap = FULL};
	SIFT_CHECKF(pgshim_try(record, NULL), "a record of %zu bytes failed to grow: %s", (size_t) FULL, pgshim_errmsg);
	SIFT_CHECKF(rd.prlen == FULL + 1 && rd.prec[FULL] == 7, "the record holds %u bytes", rd.prlen);
	return sift_check_summary();
}
