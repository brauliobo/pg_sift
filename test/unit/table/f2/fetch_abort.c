/*
 * test/unit/table/f2/fetch_abort.c - the fetch a transaction keeps for its rows (table/f2/fetch.c, tx_fetch_for): a fetch for another relation that fails to be made leaves no fetch whose
 * context is gone, so that the abort that follows does not read it.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "table/f2/fetch.c"

MemoryContext TopTransactionContext = (MemoryContext) 1;
static bool fail_begin;
static int	unrefs;

SiftColMetaPageData
sift_col_meta_read(Relation rel)
{
	return (SiftColMetaPageData) {.manifest_version = 1};
}

SiftF2Store *
sift_f2_store_open(Relation rel)
{
	return (SiftF2Store *) rel;
}

SiftColDeltaScan *
sift_col_delta_scan_begin(Relation rel, SiftTblVis *vis, MemoryContext cx)
{
	if (fail_begin)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
	return NULL;
}

void
sift_f2_gbuf_unref(SiftF2GranuleBuf *gb)
{
	unrefs++;
}

void
RegisterXactCallback(XactCallback cb, void *arg)
{
}

static void
fetch_other(void *arg)
{
	(void) tx_fetch_for((Relation) arg);
}

int
main(void)
{
	RelationData a = {.rd_id = 16384},
				b = {.rd_id = 16385};
	SiftTblF2Fetch *f;

	f = tx_fetch_for(&a);
	SIFT_CHECKF(f != NULL && tx_fetch == f, "the transaction keeps a fetch for its relation");
	f->gb = (SiftF2GranuleBuf *) &a;
	fail_begin = true;
	SIFT_CHECKF(!pgshim_try(fetch_other, &b), "the fetch for another relation fails to be made");
	SIFT_CHECKF(tx_fetch == NULL, "a failed fetch leaves none whose context it deleted");
	tx_fetch_xact_callback(XACT_EVENT_ABORT, NULL);
	SIFT_CHECKF(unrefs == 1, "the granule of the fetch is given back once: %d", unrefs);
	return sift_check_summary();
}
