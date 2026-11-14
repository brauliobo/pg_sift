/*
 * bm25/maint_sql.c - sift.promote() and sift.merge() of the indexes that have the generic maintenance of maint.c (sift, sift_col): one call, the caller's own
 * transaction, the maintenance lock held for it
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/rel.h"

#include "bm25/maint.h"
#include "bm25/maint_int.h"
#include "core/errctx.h"

Datum
sift_mops_sql_promote(const SiftMaintOps *ops, PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(ops, PG_GETARG_OID(0));
	SiftErrCtx	ctx;
	int			n;

	sift_maint_lock(index, true);
	sift_errctx_push(&ctx, RelationGetRelid(index));
	n = promote_locked(ops, index, true, true);
	sift_errctx_pop(&ctx);
	sift_maint_unlock(index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

/* Runs merge steps until the policy asks for none; force merges all immutable segments into one (and, if the kind says so, those of the open write buffer too) */
Datum
sift_mops_sql_merge(const SiftMaintOps *ops, PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(ops, PG_GETARG_OID(0));
	bool		force = PG_NARGS() > 1 && PG_GETARG_BOOL(1);
	SiftErrCtx	ctx;
	uint64		busy = 0;
	int			n = 0;

	sift_maint_lock(index, true);
	sift_errctx_push(&ctx, RelationGetRelid(index));
	promote_locked(ops, index, force && ops->force_seals, true);
	while (ops->merge_next(index, force && n == 0, SIFT_GOAL_ANY, true, &busy) == SIFT_STEP_DONE)
	{
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	sift_errctx_pop(&ctx);
	sift_maint_unlock(index);
	/* also when no merge was due: the end a merge left is cut by the first call that finds the lock free, however many calls the busy ones were */
	sift_mops_cut_free_end(ops, index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}
