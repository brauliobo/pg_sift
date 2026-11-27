/*
 * test/unit/table/f2/chain_cache_big.c - the index of a log of the chain cache (table/f2/chain_cache.c) by row holds the records of a chain of pending deletes that decodes past one palloc
 * (33 million records make a table of 2^27 slots of 8 bytes, 1 GB).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "table/f2/chain_cache.c"

#define RECS	((1 << 25) + 1)

static void
reserve(void *arg)
{
	RowIndex   *ix = arg;

	index_reserve(ix, RECS);
	index_add(ix, 0, 7);
	index_add(ix, RECS - 1, 7);
	index_add(ix, 1, 9);
}

int
main(void)
{
	RowIndex	ix = {0};
	bool		reserved = pgshim_try(reserve, &ix);

	SIFT_CHECKF(reserved, "the index of %d records is reserved: %s", RECS, pgshim_errmsg);
	SIFT_CHECKF(reserved && index_newest(&ix, 7) == RECS - 1 && ix.prev[RECS - 1] == 0 && index_newest(&ix, 9) == 1 && index_newest(&ix, 8) == -1, "the index finds the records of a key");
	pgshim_reset();
	return sift_check_summary();
}
