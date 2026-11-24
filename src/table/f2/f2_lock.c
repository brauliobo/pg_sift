/*
 * table/f2/f2_lock.c - tier 1 of the row locks of a sift_table, and EvalPlanQual.
 *
 * Locks live in shared memory, 128 stripes of 64 entries of 128 bytes and the chunks of the stripes that fill (f2_lock_ovf.h), and write no WAL: SELECT FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE and FOR UPDATE keep the conflict
 * matrix of the heap. A row changed by a transaction that has not ended is known by its entry (the change itself is staged in that backend until its commit), so a waiter learns whom
 * to wait for. After the commit of an update the entry stays until the updater is older than every snapshot (SIFT_ROWLOCK_COMMITTED_UPD): the waiter that wakes finds the new
 * version's address and EvalPlanQual runs on it. Should the entry be reused, the same address is kept on disk, as the second slot of the pair of the pending delete of the update
 * (SIFT_PD_TID_UPDATED, then SIFT_PD_TID_SUCC), until the fold of that delete makes it unneeded.
 *
 * A transaction takes an entry only while it holds few rows of a partition: past SIFT_ROWLOCK_ESCALATE_AT rows, or where the area has no memory for an entry, it holds the partition or the
 * relation (f2_lock_esc.h) and gives its entries back. Whoever waits on that learns the outcome of a row from the row on disk, as after any commit.
 *
 * An update that is a column delta keeps the address: the writer is a holder of the row's lock until its end, and the chain of the partition (table/f2/f2_col_delta.h) tells whoever locks
 * the row after it what the writer left. A committed delta its snapshot cannot see is TM_Updated with the address of the row itself; EvalPlanQual locks the row there and
 * acknowledges the delta it found, so that the retry of the statement does not meet it again.
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "table/f2/f2_lock_esc.h"
#include "table/f2/f2_lock_int.h"
#include "table/f2/f2_row_state.h"
#include "table/f2/f2_spec.h"
#include "table/f2/f2_tbl.h"

#define MOVED_PARTITIONS_TID	((UINT64CONST(0xFFFFFFFF) << 16) | 0xFFFD)

SIFT_BACKEND_LOCAL SiftRowLockTable *rows = NULL;

static bool
tier1_attached(void)
{
	SiftRowLockShmLayout *layout;

	if (rows != NULL)
		return true;
	if ((layout = sift_lock_shm_attach()) == NULL)
		return false;
	rows = &layout->rows;
	return true;
}

static LWLock *
stripe_lock(int si)
{
	return &rows->stripe_locks[si].lock;
}

/* ---- the entries ---- */

static const uint8 conflict_with[4] = {
	[LockTupleKeyShare] = 1 << LockTupleExclusive,
	[LockTupleShare] = (1 << LockTupleNoKeyExclusive) | (1 << LockTupleExclusive),
	[LockTupleNoKeyExclusive] = (1 << LockTupleShare) | (1 << LockTupleNoKeyExclusive) | (1 << LockTupleExclusive),
	[LockTupleExclusive] = 0xF,
};

static SiftRowLockKey
key_of(const RelFileLocator *loc, uint64 tid)
{
	SiftRowLockKey k;

	memset(&k, 0, sizeof(k));
	k.locator = *loc;
	k.tid = tid & SIFT_PD_TID_MASK;
	return k;
}

static int
stripe_of(const SiftRowLockKey *k)
{
	return hash_bytes((const unsigned char *) k, sizeof(*k)) % SIFT_ROWLOCK_PARTITIONS;
}

static inline LockTupleMode
holder_mode(const SiftRowLockEntry *e, int i)
{
	return (LockTupleMode) ((e->holder_modes >> (2 * i)) & 3);
}

static inline void
set_holder_mode(SiftRowLockEntry *e, int i, LockTupleMode m)
{
	e->holder_modes = (e->holder_modes & ~(UINT32_C(3) << (2 * i))) | ((uint32) m << (2 * i));
}

static void
drop_holder(SiftRowLockEntry *e, int i)
{
	e->holders[i] = e->holders[--e->nholders];
	set_holder_mode(e, i, holder_mode(e, e->nholders));
}

static MultiXactStatus
mxact_status(LockTupleMode m)
{
	static const MultiXactStatus map[4] = {MultiXactStatusForKeyShare, MultiXactStatusForShare, MultiXactStatusForNoKeyUpdate,
	MultiXactStatusForUpdate};

	return map[m];
}

static LockTupleMode
mode_of_status(MultiXactStatus s)
{
	return s == MultiXactStatusForKeyShare ? LockTupleKeyShare : s == MultiXactStatusForShare ? LockTupleShare :
		s == MultiXactStatusForNoKeyUpdate ? LockTupleNoKeyExclusive : LockTupleExclusive;
}

static bool
xid_running(TransactionId xid)
{
	return TransactionIdIsCurrentTransactionId(xid) || TransactionIdIsInProgress(xid);
}

static bool
overflow_live(const SiftRowLockEntry *e)
{
	MultiXactMember *members;
	int			n;
	bool		live = false;

	if (!MultiXactIdIsValid(e->overflow_mxid))
		return false;
	n = GetMultiXactIdMembers(e->overflow_mxid, &members, false, false);
	for (int i = 0; i < n; i++)
		live = live || xid_running(members[i].xid);
	if (n > 0)
		pfree(members);
	return live;
}

/* The first other running transaction whose lock conflicts with mode, or InvalidTransactionId; a holder that ended is dropped from the entry (the caller holds its stripe exclusively) */
static TransactionId
conflicting_holder(SiftRowLockEntry *e, LockTupleMode mode)
{
	MultiXactMember *members;
	TransactionId found = InvalidTransactionId;
	int			n;

	for (int i = 0; i < e->nholders;)
	{
		TransactionId xid = e->holders[i].xid;

		if (TransactionIdIsCurrentTransactionId(xid) || (conflict_with[mode] & (1 << holder_mode(e, i))) == 0)
			i++;
		else if (TransactionIdIsInProgress(xid))
			return xid;
		else
			drop_holder(e, i);
	}
	if (!MultiXactIdIsValid(e->overflow_mxid))
		return found;
	n = GetMultiXactIdMembers(e->overflow_mxid, &members, false, false);
	for (int i = 0; i < n && !TransactionIdIsValid(found); i++)
		if (!TransactionIdIsCurrentTransactionId(members[i].xid) && (conflict_with[mode] & (1 << mode_of_status(members[i].status))) != 0 &&
			TransactionIdIsInProgress(members[i].xid))
			found = members[i].xid;
	if (n > 0)
		pfree(members);
	return found;
}

static void
add_holder(SiftRowLockEntry *e, LockTupleMode mode)
{
	TransactionId xid = GetCurrentTransactionId();

	for (int i = 0; i < e->nholders; i++)
		if (e->holders[i].xid == xid)
		{
			if (mode > holder_mode(e, i))
				set_holder_mode(e, i, mode);
			return;
		}
	if (e->nholders < SIFT_ROWLOCK_INLINE_MAX)
	{
		e->holders[e->nholders] = (SiftRowLockHolder) {xid, GetCurrentSubTransactionId()};
		set_holder_mode(e, e->nholders++, mode);
	}
	else if (MultiXactIdIsValid(e->overflow_mxid))
		e->overflow_mxid = MultiXactIdExpand(e->overflow_mxid, xid, mxact_status(mode));
	else
		e->overflow_mxid = MultiXactIdCreate(e->holders[0].xid, mxact_status(holder_mode(e, 0)), xid, mxact_status(mode));
}

static bool
holds_entry(const SiftRowLockEntry *e)
{
	TransactionId xid = GetCurrentTransactionId();

	for (int i = 0; i < e->nholders; i++)
		if (e->holders[i].xid == xid)
			return true;
	return false;
}

static TransactionId
updater_of(const SiftRowLockEntry *e)
{
	return (e->flags & SIFT_ROWLOCK_ENDING_FLAGS) != 0 ? XidFromFullTransactionId(e->updater_fxid) : InvalidTransactionId;
}

static void
clear_updater(SiftRowLockEntry *e)
{
	e->flags &= ~(SIFT_ROWLOCK_ENDING_FLAGS | SIFT_ROWLOCK_COMMITTED_UPD);
	e->updated_tid = 0;
	e->updater_fxid = InvalidFullTransactionId;
	e->updater_cid = 0;
}

/* The updater ended: its successor or its delta is kept for EvalPlanQual when it committed, nothing when it did not (and no one held the row for it to commit: a prepared transaction ends in another backend) */
static void
resolve_updater(SiftRowLockEntry *e)
{
	if (TransactionIdDidCommit(updater_of(e)) && (e->flags & (SIFT_ROWLOCK_UPDATE_CHAIN | SIFT_ROWLOCK_COL_DELTA)) != 0)
		e->flags |= SIFT_ROWLOCK_COMMITTED_UPD;
	else
		clear_updater(e);
}

/* The entry of a prepared transaction that has ended since: its holders go, its update is settled as the commit or the abort of its transaction would have */
static void
reap_prepared(SiftRowLockEntry *e)
{
	TransactionId updater = updater_of(e);

	if ((e->flags & SIFT_ROWLOCK_PREPARE_HELD) == 0 || (TransactionIdIsValid(updater) && xid_running(updater)))
		return;
	for (int i = 0; i < e->nholders;)
		if (xid_running(e->holders[i].xid))
			i++;
		else
			drop_holder(e, i);
	if (e->nholders > 0 || overflow_live(e))
		return;
	if (TransactionIdIsValid(updater) && (e->flags & SIFT_ROWLOCK_COMMITTED_UPD) == 0)
		resolve_updater(e);
	e->flags &= ~SIFT_ROWLOCK_PREPARE_HELD;
}

static bool
removable(Relation rel, const SiftRowLockEntry *e)
{
	return (e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0 && GlobalVisCheckRemovableFullXid(rel, e->updater_fxid);
}

static bool
idle(const SiftRowLockEntry *e)
{
	return e->flags == 0 && e->nholders == 0 && !overflow_live(e);
}

static bool
in_relation(SiftRowLockEntry *e, void *arg)
{
	return RelFileLocatorEquals(e->key.locator, *(const RelFileLocator *) arg);
}

/* The oldest transaction that a snapshot may still wait on, computed now: the committed update of an earlier one is known to every snapshot */
static FullTransactionId
oldest_needed(void)
{
	return FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), GetOldestNonRemovableTransactionId(NULL));
}

static bool
reclaimable(SiftRowLockEntry *e, void *arg)
{
	reap_prepared(e);
	return ((e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0 && FullTransactionIdPrecedes(e->updater_fxid, *(FullTransactionId *) arg)) || idle(e);
}

static bool
track_oldest_committed(SiftRowLockEntry *e, void *arg)
{
	SiftRowLockEntry **oldest = arg;

	if ((e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0 && e->nholders == 0 &&
		(*oldest == NULL || FullTransactionIdPrecedes(e->updater_fxid, (*oldest)->updater_fxid)))
		*oldest = e;
	return false;
}

/*
 * A slot for an entry of the stripe, NULL when the area has no memory for one. A stripe without room frees the shells and the updates that no snapshot can still wait on, then reuses the slot of
 * the oldest committed update (its successor is on disk), and only then asks for a chunk: what is not a lock in flight does not make the stripe grow.
 */
static SiftRowLockEntry *
slot_for_write(int si)
{
	SiftRowLockEntry *oldest = NULL;
	FullTransactionId horizon;

	if (!sift_rowlock_has_room(rows, si))
	{
		horizon = oldest_needed();
		sift_rowlock_sweep(rows, si, reclaimable, &horizon);
	}
	if (!sift_rowlock_has_room(rows, si))
		sift_rowlock_sweep(rows, si, track_oldest_committed, &oldest);
	return sift_rowlock_has_room(rows, si) || oldest == NULL ? sift_rowlock_add(rows, si) : oldest;
}

static SiftRowLockEntry *
entry_for_write(int si, const SiftRowLockKey *k)
{
	SiftRowLockEntry *e = sift_rowlock_find(rows, si, k);

	if (e != NULL)
		return e;
	e = slot_for_write(si);
	if (e != NULL)
	{
		memset(e, 0, sizeof(*e));
		e->key = *k;
	}
	return e;
}

/* ---- what this transaction touched, to settle at its end (f2_lock_touched.c) ---- */

SIFT_BACKEND_LOCAL uint64 overflowed[SIFT_ROWLOCK_PARTITIONS / 64] = {0};

/* The stripes that had a chunk while the transaction gave its rows back: what no snapshot can wait on any more goes now, and the chunks it empties go with it */
static void
reclaim_overflowed(void)
{
	FullTransactionId horizon = InvalidFullTransactionId;

	for (int si = 0; si < SIFT_ROWLOCK_PARTITIONS; si++)
		if ((overflowed[si / 64] >> (si % 64) & 1) != 0)
		{
			horizon = FullTransactionIdIsValid(horizon) ? horizon : oldest_needed();
			LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
			sift_rowlock_sweep(rows, si, reclaimable, &horizon);
			LWLockRelease(stripe_lock(si));
		}
	memset(overflowed, 0, sizeof(overflowed));
}

/* Gives the row back: our holders go, our update is the commit's (kept for EvalPlanQual) or the abort's (forgotten); false for an entry that stays because a speculative insert is in flight */
static bool
settle(const SiftTouched *t, bool commit, bool keep_speculative)
{
	int			si = stripe_of(&t->key);
	SiftRowLockEntry *e;
	bool		settled;

	LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
	overflowed[si / 64] |= (uint64) (rows->overflow[si] != InvalidDsaPointer) << (si % 64);
	e = sift_rowlock_find(rows, si, &t->key);
	settled = e == NULL || !keep_speculative || (e->flags & SIFT_ROWLOCK_SPECULATIVE) == 0;
	if (e != NULL && settled)
	{
		sift_spec_end(e, t->xid);
		for (int i = 0; i < e->nholders;)
			if (e->holders[i].xid == t->xid)
				drop_holder(e, i);
			else
				i++;
		if (updater_of(e) == t->xid)
		{
			if (commit)
				resolve_updater(e);
			else
				clear_updater(e);
		}
		if (e->flags == 0 && e->nholders == 0 && !MultiXactIdIsValid(e->overflow_mxid))
			sift_rowlock_remove(rows, si, e);
	}
	LWLockRelease(stripe_lock(si));
	return settled;
}

static void
mark_prepare_held(const SiftTouched *t)
{
	int			si = stripe_of(&t->key);
	SiftRowLockEntry *e;

	LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &t->key);
	if (e != NULL)
		e->flags |= SIFT_ROWLOCK_PREPARE_HELD;
	LWLockRelease(stripe_lock(si));
}

static void
retag_holder(const SiftTouched *t, SubTransactionId parent)
{
	int			si = stripe_of(&t->key);
	SiftRowLockEntry *e;

	LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &t->key);
	for (int i = 0; e != NULL && i < e->nholders; i++)
		if (e->holders[i].xid == t->xid)
			e->holders[i].subxid = parent;
	LWLockRelease(stripe_lock(si));
}

/* The tier 2 lock holds the rows of the (sub)transaction now: its entries go, but those of the speculative inserts in flight */
static void
release_tier1(const RelFileLocator *loc, uint64 part_id, SubTransactionId subxid)
{
	for (int32 i = sift_touched_sub_first(subxid); i >= 0;)
	{
		SiftTouched t = *sift_touched_at((uint32) i);
		int32		next = sift_touched_next((uint32) i),
					moved;

		if (!(RelFileLocatorEquals(t.key.locator, *loc) && (part_id == SIFT_ESC_PART_ALL || t.part_id == part_id)) || !settle(&t, false, true))
		{
			i = next;
			continue;
		}
		sift_esc_note_tier1_release(loc, t.part_id, 1);
		moved = sift_touched_remove((uint32) i);
		i = moved >= 0 && moved == next ? i : next;
	}
}

void
sift_tbl_rowlock_xact_callback(XactEvent event, void *arg pg_attribute_unused())
{
	bool		commit = event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT;

	if (event == XACT_EVENT_PRE_PREPARE)
	{
		sift_esc_pre_prepare(release_tier1);
		for (uint32 i = 0; i < sift_touched_count(); i++)
			mark_prepare_held(sift_touched_at(i));
		return;
	}
	if (event != XACT_EVENT_PREPARE && !commit && event != XACT_EVENT_ABORT && event != XACT_EVENT_PARALLEL_ABORT)
		return;
	for (uint32 i = 0; i < sift_touched_count() && event != XACT_EVENT_PREPARE; i++)
		settle(sift_touched_at(i), commit, false);
	if (event != XACT_EVENT_PREPARE)
		reclaim_overflowed();
	sift_touched_reset();
	sift_esc_xact_end(event);
}

void
sift_tbl_rowlock_subxact_callback(SubXactEvent event, SubTransactionId mysubid, SubTransactionId parentsubid, void *arg pg_attribute_unused())
{
	if (event != SUBXACT_EVENT_ABORT_SUB && event != SUBXACT_EVENT_COMMIT_SUB)
		return;
	if (event == SUBXACT_EVENT_COMMIT_SUB)
		sift_touched_promote(mysubid, parentsubid, retag_holder);
	else
	{
		for (int32 i; (i = sift_touched_sub_first(mysubid)) >= 0;)
		{
			SiftTouched t = *sift_touched_at((uint32) i);

			(void) sift_touched_remove((uint32) i);
			settle(&t, false, false);
			sift_esc_note_tier1_release(&t.key.locator, t.part_id, 1);
		}
		sift_touched_sub_close(mysubid);
	}
	sift_esc_subxact_event(event, mysubid, parentsubid);
}

void
sift_tbl_f2_locks_init(void)
{
	RegisterXactCallback(sift_tbl_rowlock_xact_callback, NULL);
	RegisterSubXactCallback(sift_tbl_rowlock_subxact_callback, NULL);
}

/* ---- the protocol ---- */

/* The result of what an entry says of the row: the update that left it, a lock that conflicts, or none */
static void
judge_entry(SiftRowLockEntry *e, LockTupleMode mode, SiftRowLockResult *r)
{
	TransactionId updater = updater_of(e);

	if ((e->flags & (SIFT_ROWLOCK_UPDATE_CHAIN | SIFT_ROWLOCK_DELETED)) != 0)
	{
		r->xid = updater;
		r->cid = e->updater_cid;
		r->updated_tid = e->updated_tid;
		if ((e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0)
			r->result = TM_Updated;
		else if (TransactionIdIsCurrentTransactionId(updater))
			r->result = TM_SelfModified;
		else if (TransactionIdIsInProgress(updater))
			r->result = TM_BeingModified;
		else
		{
			resolve_updater(e);
			r->result = (e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0 && (e->flags & SIFT_ROWLOCK_UPDATE_CHAIN) != 0 ? TM_Updated : TM_Ok;
		}
	}
	if (r->result == TM_Ok)
	{
		r->xid = conflicting_holder(e, mode);
		if (TransactionIdIsValid(r->xid))
			r->result = TM_BeingModified;
	}
}

SiftRowLockResult
sift_tbl_rowlock_acquire(Relation rel, uint64 tid, uint64 part_id, LockTupleMode mode)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = stripe_of(&k);
	SiftRowLockResult r = {.result = TM_Ok};
	SiftRowLockEntry *e;

	SIFT_INVARIANT(tier1_attached(), "the row lock table of sift_table is mapped in every backend that locks a row");
	LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &k);
	if (e != NULL)
		reap_prepared(e);
	if (e != NULL && removable(rel, e))
	{
		sift_rowlock_remove(rows, si, e);
		e = NULL;
	}
	if (e != NULL)
		judge_entry(e, mode, &r);
	if (r.result == TM_Ok && e == NULL)
		e = entry_for_write(si, &k);
	r.stripe_full = r.result == TM_Ok && e == NULL;
	if (r.result == TM_Ok && e != NULL)
	{
		bool		added = sift_touched_add(&k, part_id);

		add_holder(e, mode);
		sift_esc_note_tier1_acquire(&rel->rd_locator, part_id, mode, added);
	}
	LWLockRelease(stripe_lock(si));
	return r;
}

/* The entry of a row the current (sub)transaction holds, locked, or NULL: a row held by a partition or the relation has none to change */
static SiftRowLockEntry *
held_entry(const SiftRowLockKey *k, int *si)
{
	SiftRowLockEntry *e;

	if (!tier1_attached())
		return NULL;
	*si = stripe_of(k);
	LWLockAcquire(stripe_lock(*si), LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, *si, k);
	if (e != NULL && holds_entry(e))
		return e;
	LWLockRelease(stripe_lock(*si));
	return NULL;
}

void
sift_tbl_rowlock_mark_updated(Relation rel, uint64 old_tid, uint64 new_tid, CommandId cid, bool deleted, bool moved_partition)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, old_tid);
	int			si;
	SiftRowLockEntry *e = held_entry(&k, &si);

	if (e == NULL)
		return;
	e->flags = (e->flags & ~(SIFT_ROWLOCK_COMMITTED_UPD | SIFT_ROWLOCK_COL_DELTA)) |
		(deleted && !moved_partition ? SIFT_ROWLOCK_DELETED : SIFT_ROWLOCK_UPDATE_CHAIN);
	e->updated_tid = moved_partition ? MOVED_PARTITIONS_TID : new_tid;
	e->updater_fxid = GetCurrentFullTransactionId();
	e->updater_cid = cid;
	LWLockRelease(stripe_lock(si));
}

void
sift_tbl_rowlock_mark_col_delta(Relation rel, uint64 tid, CommandId cid)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si;
	SiftRowLockEntry *e = held_entry(&k, &si);

	if (e == NULL)
		return;
	e->flags = (e->flags & ~SIFT_ROWLOCK_COMMITTED_UPD) | SIFT_ROWLOCK_COL_DELTA;
	e->delta_ver++;
	e->updater_fxid = GetCurrentFullTransactionId();
	e->updater_cid = cid;
	LWLockRelease(stripe_lock(si));
}

void
sift_tbl_rowlock_set_speculative(Relation rel, uint64 tid, uint32 token, bool on)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = stripe_of(&k);
	SiftRowLockEntry *e;

	if (!tier1_attached())
		return;
	LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
	e = on ? entry_for_write(si, &k) : sift_rowlock_find(rows, si, &k);
	if (e != NULL && on)
	{
		sift_spec_stage(e, GetTopFullTransactionId(), token);
		(void) sift_touched_add(&k, SIFT_ESC_PART_ALL);
	}
	LWLockRelease(stripe_lock(si));
}

bool
sift_tbl_rowlock_spec_done(Relation rel, uint64 tid, bool succeeded)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = stripe_of(&k);
	SiftRowLockEntry *e;
	bool		need_flush = false;

	if (!tier1_attached())
		return false;
	LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &k);
	if (e != NULL && succeeded)
		need_flush = sift_spec_succeeded(e);
	else if (e != NULL)
	{
		sift_spec_aborted(e);
		if (idle(e))
		{
			sift_rowlock_remove(rows, si, e);
			sift_touched_forget(&k);
		}
	}
	LWLockRelease(stripe_lock(si));
	return need_flush;
}

void
sift_tbl_rowlock_staged_flushed(Relation rel, uint64 tid)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = stripe_of(&k);
	SiftRowLockEntry *e;

	if (!tier1_attached())
		return;
	LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &k);
	if (e != NULL)
		sift_spec_flushed(e);
	LWLockRelease(stripe_lock(si));
}

SiftSpecProbe
sift_tbl_rowlock_probe_staged(Relation rel, uint64 tid, TransactionId *xmin, uint32 *token)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = stripe_of(&k);
	SiftRowLockEntry *e;
	SiftSpecProbe r = SIFT_SPEC_NONE;

	if (!tier1_attached())
		return r;
	LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &k);
	if (e != NULL)
		r = sift_spec_probe(e, (e->flags & SIFT_ROWLOCK_STAGED_INSERT) != 0 && TransactionIdIsInProgress(XidFromFullTransactionId(e->updater_fxid)), xmin, token);
	LWLockRelease(stripe_lock(si));
	return r;
}

uint32
sift_tbl_rowlock_speculative_token(Relation rel, uint64 tid)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = stripe_of(&k);
	SiftRowLockEntry *e;
	uint32		token = 0;

	if (!tier1_attached())
		return 0;
	LWLockAcquire(stripe_lock(si), LW_SHARED);
	e = sift_rowlock_find(rows, si, &k);
	if (e != NULL && (e->flags & SIFT_ROWLOCK_SPECULATIVE) != 0)
		token = e->spec_token;
	LWLockRelease(stripe_lock(si));
	return token;
}

bool
sift_tbl_rowlock_successor(Relation rel, uint64 old_tid, uint64 *succ)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, old_tid);
	int			si = stripe_of(&k);
	SiftRowLockEntry *e;
	bool		found;

	if (!tier1_attached())
		return false;
	LWLockAcquire(stripe_lock(si), LW_SHARED);
	e = sift_rowlock_find(rows, si, &k);
	found = e != NULL && (e->flags & SIFT_ROWLOCK_UPDATE_CHAIN) != 0;
	if (found)
		*succ = e->updated_tid;
	LWLockRelease(stripe_lock(si));
	return found;
}

bool
sift_tbl_rowlock_is_escalated(Relation rel)
{
	return sift_esc_rel_is_escalated(&rel->rd_locator);
}

void
sift_tbl_rowlock_drop_rel(const RelFileLocator *loc)
{
	sift_esc_drop_rel(loc);
	if (!tier1_attached())
		return;
	for (int si = 0; si < SIFT_ROWLOCK_PARTITIONS; si++)
	{
		LWLockAcquire(stripe_lock(si), LW_EXCLUSIVE);
		sift_rowlock_sweep(rows, si, in_relation, (void *) loc);
		LWLockRelease(stripe_lock(si));
	}
}

/* ---- locking a row ---- */

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
lock_entry_or_partition(Relation rel, uint64 tid, uint64 part_id, LockTupleMode mode, LockWaitPolicy wait_policy, TM_FailureData *tmfd, bool *reread)
{
	SiftRowLockResult lr = {.result = TM_Ok};
	bool		escalated;
	TM_Result	res;

	if (!sift_esc_should_escalate(&rel->rd_locator, part_id, mode, false))
	{
		lr = sift_tbl_rowlock_acquire(rel, tid, part_id, mode);
		if (!lr.stripe_full)
			return tier1_result(&lr, tmfd);
	}
	res = sift_esc_promote(rel, part_id, sift_row_part_count(rel), mode, wait_policy, lr.stripe_full, release_tier1, &escalated, reread, tmfd);
	if (res != TM_Ok || *reread)
		return res;
	lr = sift_tbl_rowlock_acquire(rel, tid, part_id, mode);
	return lr.stripe_full ? TM_BeingModified : tier1_result(&lr, tmfd);
}

static TM_Result
lock_held(Relation rel, const SiftRowState *rs, uint64 tid, LockTupleMode mode, LockWaitPolicy wait_policy, TM_FailureData *tmfd, bool *reread)
{
	bool		covered;
	TM_Result	res = sift_esc_before_row_lock(rel, rs->part_id, mode, wait_policy, &covered, reread, tmfd);

	if (res != TM_Ok || *reread || covered)
		return res;
	return lock_entry_or_partition(rel, tid, rs->part_id, mode, wait_policy, tmfd, reread);
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
		bool		reread = false;

		if (res == TM_Ok)
			res = lock_held(rel, &rs, tid, mode, wait_policy, tmfd, &reread);
		if (res == TM_Ok && reread)
		{
			*waited = true;
			continue;
		}
		if (res == TM_Ok && judge_delta && mode != LockTupleKeyShare)
			res = sift_row_delta_result(rel, tid, cid, snap, tmfd);
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

static void
fetch_locked(Relation rel, ItemPointer tid, TupleTableSlot *slot)
{
	bool		found = sift_tbl_f2_tuple_fetch_row_version(rel, tid, SnapshotAny, slot);

	SIFT_INVARIANT(found, "a locked row of table \"%s\" is gone", RelationGetRelationName(rel));
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
		if (res == TM_Updated && (flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) != 0)
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
			fetch_locked(rel, &cur, slot);
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

void
sift_tbl_f2_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	Relation	rel = scan->rs_rd;
	uint64		cur = sift_tbl_tid_pack(tid),
				succ;

	sift_stage_flush(rel);
	while (sift_row_visible_successor(rel, cur, scan->rs_snapshot, &succ) && succ != MOVED_PARTITIONS_TID)
		cur = succ;
	sift_tbl_tid_unpack(tid, cur);
}
