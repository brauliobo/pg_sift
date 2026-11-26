/*
 * core/maint/step.c - the step the workers of the queue (core/maint/sched.h) run for a claim: the relation is opened under the lock order of VACUUM and the step runs in a transaction of its
 * own. The kind says what the step is: the mark of a quarantine flags the metapage of the index, the merge of a BM25 index promotes and merges one step of it, and the others are the steps of an
 * index of format major 2 or of a sift_table of format major 2 (col/f2/vacuum/run.c). An index that is out of service is left alone, whatever the kind but the mark that says so.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/f2.h"
#include "col/f2/vacuum/run.h"
#include "core/maint/maint.h"
#include "core/maint/pacer.h"
#include "core/maint/sched.h"
#include "core/meta_peek.h"
#include "core/quarantine/quarantine.h"
#include "table/dispatch.h"

static bool
is_f2_table(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_RELATION && sift_tbl_is_table(rel) && sift_meta_resolve_major(rel) == SIFT_FORMAT2_MAJOR;
}

static bool
is_f2_index(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX && sift_meta_peek_format(rel).status == SIFT_FMT_V2;
}

static LOCKMODE
step_lock(SiftMaintKind kind)
{
	return kind == SIFT_MK_QUARANTINE_MARK || kind == SIFT_MK_BM25_MERGE ? RowExclusiveLock : ShareUpdateExclusiveLock;
}

/* One promotion, else one merge, of the policy of the index: more is asked for while a step did something */
static SiftMaintStep
bm25_step(Relation index)
{
	const SiftMaintOps *ops = sift_mops_of(index);

	if (ops == NULL || !ops->is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not an index that is merged in the background", RelationGetRelationName(index))));
	return sift_mops_promote(ops, index, false) > 0 || sift_mops_merge_step(ops, index, false) ? SIFT_MSTEP_MORE : SIFT_MSTEP_DONE;
}

static SiftMaintStep
step_of(Relation rel, const SiftMaintClaim *claim)
{
	if (claim->kind == SIFT_MK_QUARANTINE_MARK)
	{
		sift_quarantine_mark(rel);
		return SIFT_MSTEP_DONE;
	}
	if (sift_index_quarantined(rel))
		return SIFT_MSTEP_DONE;
	if (claim->kind == SIFT_MK_BM25_MERGE)
		return bm25_step(rel);
	if (!is_f2_index(rel) && !is_f2_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not an index of format major 2 or a sift_table of format major 2", RelationGetRelationName(rel))));
	return sift_f2_maint_step(rel, claim);
}

/* The lock of the table of the claim, without waiting: a step that waited for a VACUUM or an ALTER TABLE would hold the slot of its database the while */
static bool
lock_table(const SiftMaintClaim *claim)
{
	Oid			heapoid = IndexGetRelation(claim->rel, true);

	return ConditionalLockRelationOid(OidIsValid(heapoid) ? heapoid : claim->rel, step_lock(claim->kind));
}

SiftMaintStep
sift_maint_step(const SiftMaintClaim *claim, void *arg pg_attribute_unused())
{
	Relation	rel;
	SiftMaintStep step = SIFT_MSTEP_GONE;

	StartTransactionCommand();
	if (!lock_table(claim))
	{
		sift_pacer_note_lock_yield();
		CommitTransactionCommand();
		return SIFT_MSTEP_BUSY;
	}
	PushActiveSnapshot(GetTransactionSnapshot());
	rel = sift_maint_open(claim->rel, step_lock(claim->kind));
	if (rel != NULL)
	{
		step = step_of(rel, claim);
		relation_close(rel, NoLock);
	}
	PopActiveSnapshot();
	CommitTransactionCommand();
	return step;
}
