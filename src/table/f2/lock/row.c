/*
 * table/f2/lock/row.c - locking a row of a sift_table for a statement (lock.c has the entries): an entry of tier 1 while the partition has few rows and the stripe has room, else the partition or
 * the relation (esc.h), the wait for the transaction in the way, and the tuple lock callbacks of the table access method.
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"

#include "table/f2/lock/2pc.h"
#include "table/f2/lock/esc.h"
#include "table/f2/row/state.h"
#include "table/f2/spec.h"
#include "table/f2/tbl.h"

static TM_Result
tier1_result(const SiftRowLockResult *lr, TM_FailureData *tmfd)
{
	if (lr->result == TM_Updated)
	{
		sift_tbl_tid_unpack(&tmfd->ctid, lr->updated_tid);
		tmfd->xmax = lr->xid;
	}
	else if (lr->result == TM_BeingModified || lr->result == TM_SelfModified)
	{
		tmfd->xmax = lr->xid;
		tmfd->cmax = lr->cid;
	}
	return lr->result;
}

/*
 * The row is not held yet: an entry while the partition has few and the stripe has room, else the partition or the relation. *reread: a lock was waited for or taken that no entry stands
 * for, so the row on disk is read again. A NOWAIT or SKIP LOCKED request that was refused the partition goes on with entries; one that finds no slot either is in someone's way.
 */
static TM_Result
lock_entry_or_partition(Relation rel, uint64 tid, uint64 part_id, LockTupleMode mode, LockWaitPolicy wait_policy, TM_FailureData *tmfd, bool *reread, bool *added)
{
	SiftRowLockResult lr = {.result = TM_Ok};
	bool		escalated;
	TM_Result	res;

	if (!sift_esc_should_escalate(&rel->rd_locator, part_id, mode, false))
	{
		lr = sift_tbl_rowlock_acquire(rel, tid, part_id, mode);
		*added = lr.added;
		if (!lr.stripe_full)
			return tier1_result(&lr, tmfd);
	}
	res = sift_esc_promote(rel, part_id, sift_row_part_count(rel), mode, wait_policy, lr.stripe_full, sift_rowlock_release_tier1, &escalated, reread, tmfd);
	if (res != TM_Ok || *reread)
		return res;
	lr = sift_tbl_rowlock_acquire(rel, tid, part_id, mode);
	*added = lr.added;
	if (!lr.stripe_full)
		return tier1_result(&lr, tmfd);
	res = sift_esc_promote(rel, part_id, sift_row_part_count(rel), mode, wait_policy, true, sift_rowlock_release_tier1, &escalated, reread, tmfd);
	SIFT_INVARIANT(res != TM_Ok || *reread, "a request for a coarse lock that must be taken is granted, or it is refused");
	return res;
}

/* A prepared transaction that crashed with the server holds its rows in the lock manager only (f2_lock_2pc.h): asked first, so that a request it refuses takes no entry and no intention lock */
static TM_Result
lock_held(Relation rel, const SiftRowState *rs, uint64 tid, LockTupleMode mode, LockWaitPolicy wait_policy, TM_FailureData *tmfd, bool *reread, bool *added)
{
	bool		covered = false;
	TM_Result	res = TM_Ok;

	if (sift_esc_prepared_locks_possible(rel))
		res = sift_lock_2pc_probe(rel->rd_lockInfo.lockRelId.dbId, rel->rd_lockInfo.lockRelId.relId, tid & SIFT_PD_TID_MASK, mode, tmfd);
	if (res == TM_Ok)
		res = sift_esc_before_row_lock(rel, rs->part_id, mode, wait_policy, &covered, reread, tmfd);
	if (res != TM_Ok || *reread || covered)
		return res;
	return lock_entry_or_partition(rel, tid, rs->part_id, mode, wait_policy, tmfd, reread, added);
}

/* The lock is held before the deltas are read: no one else can add one while the row is ours */
static TM_Result
lock_row(Relation rel, uint64 tid, LockTupleMode mode, LockWaitPolicy wait_policy, Snapshot snap, CommandId cid, TM_FailureData *tmfd, bool judge_delta, bool *waited)
{
	ItemPointerData waiting_for;

	sift_tbl_tid_unpack(&waiting_for, tid);
	memset(tmfd, 0, sizeof(*tmfd));
	*waited = false;
	for (;;)
	{
		SiftRowState rs = sift_row_state_read(rel, tid);
		TM_Result	res = sift_row_state_result(&rs, cid, tmfd, tid);
		bool		reread = false,
					added = false;

		if (res == TM_Ok)
			res = lock_held(rel, &rs, tid, mode, wait_policy, tmfd, &reread, &added);
		if (res == TM_Ok && reread)
		{
			*waited = true;
			continue;
		}
		if (res == TM_Ok && judge_delta && mode != LockTupleKeyShare)
		{
			res = sift_row_delta_result(rel, tid, cid, snap, tmfd);
			if (res == TM_BeingModified && added)
				sift_rowlock_release_new(rel, tid, rs.part_id);
		}
		if (res != TM_BeingModified || wait_policy != LockWaitBlock)
			return res;
		*waited = true;
		XactLockTableWait(tmfd->xmax, rel, &waiting_for, XLTW_Lock);
	}
}

TM_Result
sift_tbl_f2_lock_for_modify(Relation rel, uint64 tid, LockTupleMode mode, bool wait, Snapshot snap, CommandId cid, TM_FailureData *tmfd, bool *waited)
{
	return lock_row(rel, tid, mode, wait ? LockWaitBlock : LockWaitError, snap, cid, tmfd, true, waited);
}

/* ---- the callbacks ---- */

void
sift_tbl_f2_record_update(Relation rel, uint64 old_tid, uint64 new_tid, CommandId cid)
{
	sift_tbl_rowlock_mark_updated(rel, old_tid, new_tid, cid, false, false);
}

void
sift_tbl_f2_record_delete(Relation rel, uint64 tid, CommandId cid, bool changing_part)
{
	sift_tbl_rowlock_mark_updated(rel, tid, 0, cid, true, changing_part);
}

TM_Result
sift_tbl_f2_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
					   uint8 flags, TM_FailureData *tmfd)
{
	ItemPointerData cur = *tid;
	bool		traversed = false,
				at_latest = false;

	sift_stage_flush(rel);
	for (;;)
	{
		bool		waited;
		TM_Result	res = lock_row(rel, sift_tbl_tid_pack(&cur), mode, wait_policy, snapshot, cid, tmfd, !at_latest, &waited);

		if (res == TM_BeingModified && wait_policy == LockWaitSkip)
			return TM_WouldBlock;
		if (res == TM_BeingModified)
			ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE), errmsg("could not obtain lock on row in relation \"%s\"", RelationGetRelationName(rel))));
		if (res == TM_Updated && ((flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) != 0 || (!IsolationUsesXactSnapshot() && ItemPointerEquals(&tmfd->ctid, &cur))))
		{
			if (IsolationUsesXactSnapshot())
				ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("could not serialize access due to concurrent update")));
			if (ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
				ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								errmsg("tuple to be locked was already moved to another partition due to concurrent update")));
			at_latest = ItemPointerEquals(&tmfd->ctid, &cur);
			cur = tmfd->ctid;
			traversed = true;
			continue;
		}
		if (res == TM_Ok)
		{
			bool		found = sift_tbl_f2_tuple_fetch_row_version(rel, &cur, SnapshotAny, slot);

			SIFT_INVARIANT(found, "a locked row of table \"%s\" is gone", RelationGetRelationName(rel));
			sift_tbl_f2_predicate_point(rel, sift_tbl_tid_pack(&cur), snapshot);
			if (at_latest)
				sift_row_delta_ack(rel, sift_tbl_tid_pack(&cur), cid);
			slot->tts_tid = cur;
			tmfd->traversed = traversed;
			ItemPointerCopy(&cur, &tmfd->ctid);
			ItemPointerCopy(&cur, tid);
		}
		return res;
	}
}
