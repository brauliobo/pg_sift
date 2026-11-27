/*
 * test/unit/col/f2/track_cells.c - the key cells of the command of a track_changes trigger (col/f2/track.c, ctx_make): an allocation of more cells that fails (out of memory, then a
 * ROLLBACK TO SAVEPOINT, which leaves the transaction and its cells) leaves no pointer to the cells it freed, so the next command of the transaction does not free them a second time.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "test/common/pgshim.h"

MemoryContext TopTransactionContext = (MemoryContext) 1;

#include "col/f2/track.c"

sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;

static FormData_pg_index index_form = {.indisready = true};
static RelationData index_rel = {.rd_index = &index_form};
static SiftF2Rel wide = {.ncols = PG_INT32_MAX};

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

Oid
sift_col_track_trigger_index(Oid trigger)
{
	return 42;
}

uint32
GetSysCacheHashValue(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{
	return 1;
}

bool
sift_col_track_index_live(Oid index)
{
	return true;
}

Relation
index_open(Oid relationId, LOCKMODE lockmode)
{
	return &index_rel;
}

bool
sift_quarantine_skips(Relation index, SiftQuarSkip kind)
{
	return false;
}

SiftF2Rel *
sift_f2_rel(Relation index)
{
	return &wide;
}

SiftTxnStage *
sift_stage_get(Relation rel, const SiftPartSpec *spec)
{
	return NULL;
}

bool
sift_f2_journal_on(Relation index)
{
	return false;
}

int
main(void)
{
	Trigger		trigger = {.tgoid = 7};
	TriggerData td = {.tg_trigger = &trigger};
	volatile bool failed = false;

	pgshim_error_hook = pg_re_throw;
	ctx.hooked = true;
	ctx.diff_cx = TopTransactionContext;
	ctx.cells = palloc0(sizeof(SiftCell));
	ctx.cells_cap = 1;
	PG_TRY();
	{
		(void) ctx_make(&td);
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	SIFT_CHECKF(failed, "the cells of %d columns were allocated", wide.ncols);
	SIFT_CHECKF(ctx.cells == NULL && ctx.cells_cap == 0, "a failed allocation of the cells left the freed ones as the cells of the command (%d of them)", ctx.cells_cap);
	return sift_check_summary();
}
