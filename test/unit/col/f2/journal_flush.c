/*
 * test/unit/col/f2/journal_flush.c - the held key-journal records of a transaction (col/f2/journal.c, sift_f2_journal_flush) when a flush fails part way, as a lock timeout under a
 * savepoint makes it: the records not yet written stay held for the commit, those written leave, and the next flush writes the rest.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "col/f2/journal.c"

sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;

static RelationData rel;
static Oid	failing = InvalidOid;
static int	opened;

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

Relation
try_index_open(Oid relationId, LOCKMODE lockmode)
{
	if (relationId == failing)
		siglongjmp(*PG_exception_stack, 1);
	opened++;
	return &rel;
}

void
index_close(Relation relation, LOCKMODE lockmode)
{
}

SiftColMetaPageData
sift_col_meta_read(Relation r)
{
	return (SiftColMetaPageData) {.jrnl_blk = InvalidBlockNumber};
}

static Held recs[3];

static void
hold_three(void)
{
	recs[0] = (Held) {.index = 10,.key = 1};
	recs[1] = (Held) {.index = 20,.key = 2};
	recs[2] = (Held) {.index = 10,.key = 3};
	held.recs = recs;
	held.n = 3;
	opened = 0;
}

static bool
flush_threw(void)
{
	bool		threw = false;

	PG_TRY();
	{
		sift_f2_journal_flush();
	}
	PG_CATCH();
	{
		threw = true;
	}
	PG_END_TRY();
	return threw;
}

/* The index whose lock times out is the first or the second of the flush: what was not written stays held, and a flush after the lock is free writes it */
static void
check_failed_flush(Oid fail, int left, const char *what)
{
	hold_three();
	failing = fail;
	SIFT_CHECKF(flush_threw(), "%s: the flush fails", what);
	SIFT_CHECKF(held.n == left && !held.flushing, "%s: %d records held (want %d), flushing %d", what, held.n, left, held.flushing);
	for (int i = 0; i < held.n; i++)
		SIFT_CHECKF(held.recs[i].index >= fail, "%s: record %d of index %u is held again", what, i, held.recs[i].index);
	failing = InvalidOid;
	SIFT_CHECKF(!flush_threw() && held.n == 0, "%s: the next flush writes the rest: %d held", what, held.n);
}

int
main(void)
{
#ifndef __SANITIZE_THREAD__
	/* ThreadSanitizer cannot follow the longjmp of an error into a PG_TRY */
	check_failed_flush(20, 1, "the second index fails");
	check_failed_flush(10, 3, "the first index fails");
	hold_three();
	SIFT_CHECKF(!flush_threw() && held.n == 0 && opened == 2, "a flush that succeeds writes every record of both indexes: %d held, %d opened", held.n, opened);
#endif
	return sift_check_summary();
}
