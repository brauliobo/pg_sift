/*
 * test/unit/table/f2/gcache_unkept.c - the row pack block the cache keeps for nobody (table/f2/gcache.c, block_unkept): a read of the next block that fails (a cancel, an I/O error) leaves
 * no pointer to the block it freed, so the next read or a DISCARD ALL does not free it a second time.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "test/common/pgshim.h"

#include "table/f2/gcache.c"

sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

SiftRowPackBlk *
sift_row_pack_read_block(Relation rel, const SiftPart *part, uint32 blk)
{
	ereport(ERROR, (errcode(ERRCODE_QUERY_CANCELED), errmsg("canceling statement due to user request")));
	return NULL;
}

int
main(void)
{
	SiftF2PartPin pin = {0};
	volatile bool failed = false;

	pgshim_error_hook = pg_re_throw;
	bunkept = palloc(64);
	PG_TRY();
	{
		(void) block_unkept(NULL, &pin, 1);
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	SIFT_CHECKF(failed, "the read of the block did not fail");
	SIFT_CHECKF(bunkept == NULL, "a failed read of the next block left the freed one as the block kept for nobody");
	return sift_check_summary();
}
