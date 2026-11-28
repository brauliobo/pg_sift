/*
 * table/f2/lock/lock.c - tier 1 of the row locks of a sift_table, and EvalPlanQual.
 *
 * Locks live in shared memory, 256 stripes of 64 entries of 128 bytes and the chunks of the stripes that fill (f2_lock_ovf.h), and write no WAL: SELECT FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE and FOR UPDATE keep the conflict
 * matrix of the heap. A row changed by a transaction that has not ended is known by its entry (the change itself is staged in that backend until its commit), so a waiter learns whom
 * to wait for. After the commit of an update the entry stays until the updater is older than every snapshot (SIFT_ROWLOCK_COMMITTED_UPD): the waiter that wakes finds the new
 * version's address and EvalPlanQual runs on it. Should the entry be reused, the same address is kept on disk, as the second slot of the pair of the pending delete of the update
 * (SIFT_PD_TID_UPDATED, then SIFT_PD_TID_SUCC), until the fold of that delete makes it unneeded.
 *
 * A transaction takes an entry only while it holds few rows of a partition: past SIFT_ROWLOCK_ESCALATE_AT rows, or where the area has no memory for an entry, it holds the partition or the
 * relation (f2_lock_esc.h) and gives its entries back. Whoever waits on that learns the outcome of a row from the row on disk, as after any commit.
 *
 * The holders of a row past the room of its entry are the members of a MultiXactId, which is read and made with no stripe lock held (f2_lock_mxid.h), and so is whether a holder or an updater
 * runs or committed: a stripe lock waits for no I/O of the server. The row locks of a prepared transaction are also tuple locks of the lock manager, which survive a crash and a restart (f2_lock_2pc.h).
 *
 * An update that is a column delta keeps the address: the writer is a holder of the row's lock until its end, and the chain of the partition (table/f2/col_delta/col_delta.h) tells whoever locks
 * the row after it what the writer left. A committed delta its snapshot cannot see is TM_Updated with the address of the row itself; EvalPlanQual locks the row there and
 * acknowledges the delta it found, so that the retry of the statement does not meet it again.
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "core/compat.h"
#include "core/dispatch.h"
#include "table/f2/lock/2pc.h"
#include "table/f2/lock/esc.h"
#include "table/f2/lock/int.h"
#include "table/f2/lock/mxid.h"
#include "table/f2/row/state.h"
#include "table/f2/spec.h"
#include "table/f2/tbl.h"

SIFT_BACKEND_LOCAL SiftRowLockTable *rows = NULL;

/* The table mapped, and the area of its chunks where it exists: no stripe lock waits for a mapping */
static bool
tier1_ready(void)
{
	SiftRowLockShmLayout *layout;

	if (rows == NULL && (layout = sift_lock_shm_attach()) != NULL)
		rows = &layout->rows;
	if (rows != NULL)
		sift_rowlock_area_map(rows);
	return rows != NULL;
}

static LWLock *
stripe_lock(int si)
{
	return &rows->hdr[si].lock;
}

static void
stripe_unlock(int si)
{
	LWLockRelease(stripe_lock(si));
	sift_rowlock_free_emptied();
}

/* ---- the entries ---- */

static SiftRowLockKey
key_of(const RelFileLocator *loc, uint64 tid)
{
	SiftRowLockKey k;

	memset(&k, 0, sizeof(k));
	k.locator = *loc;
	k.tid = tid & SIFT_PD_TID_MASK;
	return k;
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
resolve_updater(SiftRowLockEntry *e, bool committed)
{
	if (committed && (e->flags & (SIFT_ROWLOCK_UPDATE_CHAIN | SIFT_ROWLOCK_COL_DELTA)) != 0)
		e->flags |= SIFT_ROWLOCK_COMMITTED_UPD;
	else
		clear_updater(e);
}

/* The entry of a prepared transaction that has ended since: its holders go, its update is settled as the commit or the abort of its transaction would have (seen: the members of its overflow, if read) */
static void
reap_prepared(SiftRowLockEntry *e, const SiftMxMembers *seen, SiftXidAsk *a)
{
	TransactionId updater = updater_of(e);

	if ((e->flags & SIFT_ROWLOCK_PREPARE_HELD) == 0 || (TransactionIdIsValid(updater) && sift_ask_running(a, updater)))
		return;
	sift_hold_drop_ended(e, a);
	if (e->nholders > 0 || sift_hold_overflow_live(e, seen))
		return;
	e->overflow_mxid = InvalidMultiXactId;
	if (TransactionIdIsValid(updater) && (e->flags & SIFT_ROWLOCK_COMMITTED_UPD) == 0)
		resolve_updater(e, sift_ask_committed(a, updater));
	e->flags &= ~SIFT_ROWLOCK_PREPARE_HELD;
}

static bool
removable(const SiftRowLockEntry *e, SiftXidAsk *a)
{
	return (e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0 && sift_rowlock_unheld(e) && sift_ask_gone(a, e->updater_fxid);
}

/* Asks every answer that judging the entry for the mode needs: a try that has one outstanding lets go of the stripe and comes back with it */
static void
ask_entry(const SiftRowLockEntry *e, LockTupleMode mode, SiftXidAsk *a)
{
	if ((e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0)
		(void) sift_ask_gone(a, e->updater_fxid);
	else if (TransactionIdIsValid(updater_of(e)))
		(void) sift_ask_running(a, updater_of(e));
	sift_hold_ask(e, mode, a);
}

/* An entry of the table of its stripe, not of a chunk that the stripe took: a chunk is given back when it is empty, which a last locker would prevent */
static bool
in_table(int si, const SiftRowLockEntry *e)
{
	return e >= rows->stripes[si].entries && e < rows->stripes[si].entries + SIFT_ROWLOCK_BUCKETS;
}

static bool
idle(const SiftRowLockEntry *e)
{
	return (e->flags & ~SIFT_ROWLOCK_LAST_LOCKER) == 0 && sift_rowlock_unheld(e);
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

/* What a request needs of the server and asks for with the stripe lock let go: the horizon of the entries that no snapshot waits for (it takes the lock of the process array), or the spare chunk of the backend for a full stripe (it may make a segment) */
typedef struct Outside
{
	FullTransactionId horizon;
	SiftXidAsk *ask;			/* whether the holders of the entries of prepared transactions run: NULL asks nothing, and they do */
	bool		wants_horizon;
	bool		wants_chunk;
	bool		wants_answers;
	bool		no_memory;
} Outside;

#define OUTSIDE_INIT(a) {.horizon = InvalidFullTransactionId,.ask = (a)}

static bool
needs_outside(const Outside *o)
{
	return o->wants_horizon || o->wants_chunk || o->wants_answers;
}

static void
provide(Outside *o)
{
	if (o->wants_answers)
		sift_ask_answer(o->ask);
	else if (o->wants_horizon)
		o->horizon = oldest_needed();
	else
		o->no_memory = !sift_rowlock_spare_take(rows);
	o->wants_horizon = o->wants_chunk = o->wants_answers = false;
}

static bool
reclaimable(SiftRowLockEntry *e, void *arg)
{
	Outside    *o = arg;

	reap_prepared(e, NULL, o->ask);
	return ((e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0 && sift_rowlock_unheld(e) && FullTransactionIdPrecedes(e->updater_fxid, o->horizon)) || idle(e);
}

/* The end of a transaction keeps the xmax its locks left: only a stripe that has no room for an entry gives those up */
static bool
reclaimable_keeping_lockers(SiftRowLockEntry *e, void *arg)
{
	return (e->flags & SIFT_ROWLOCK_LAST_LOCKER) == 0 && reclaimable(e, arg);
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
 * A slot for an entry of the key in the stripe, NULL where the stripe needs something of the server first (o says what: the caller lets go of the lock, gets it, and asks again) or where the area has no
 * memory for a chunk (o->no_memory). A stripe without room frees the shells and the updates that no snapshot can still wait on, then reuses the slot of the oldest committed update (its successor is on
 * disk), and only then takes a chunk: what is not a lock in flight does not make the stripe grow.
 */
static SiftRowLockEntry *
slot_for_write(int si, const SiftRowLockKey *k, Outside *o)
{
	SiftRowLockEntry *oldest = NULL;

	if (!sift_rowlock_has_room(rows, si))
	{
		if (!FullTransactionIdIsValid(o->horizon))
		{
			o->wants_horizon = true;
			return NULL;
		}
		sift_rowlock_sweep(rows, si, reclaimable, o);
	}
	if (!sift_rowlock_has_room(rows, si) && sift_ask_must(o->ask))
	{
		o->wants_answers = true;
		return NULL;
	}
	if (!sift_rowlock_has_room(rows, si))
		sift_rowlock_sweep(rows, si, track_oldest_committed, &oldest);
	if (!sift_rowlock_has_room(rows, si) && oldest != NULL)
	{
		sift_rowlock_reset(rows, si, oldest, k);
		return oldest;
	}
	if (!sift_rowlock_has_room(rows, si) && !sift_rowlock_has_spare())
	{
		o->wants_chunk = !o->no_memory;
		return NULL;
	}
	return sift_rowlock_add(rows, si, k);
}

static SiftRowLockEntry *
entry_for_write(int si, const SiftRowLockKey *k, Outside *o)
{
	SiftRowLockEntry *e = sift_rowlock_find(rows, si, k);

	return e != NULL ? e : slot_for_write(si, k, o);
}

/* ---- what this transaction touched, to settle at its end (f2_lock_touched.c) ---- */

/*
 * The stripes with chunks whose oldest committed update no snapshot can wait on any more, at the end of a transaction that touched rows or whose snapshot
 * came before one of them (no other end moves the horizon past it, and the process array is not read for it): what is past the horizon goes, and the chunks it empties go with it. A backend that does not map the chunks frees none, and a stripe another backend holds is left to a later end: the ends of transactions do not queue on the stripes.
 */
static void
reclaim_kept(void)
{
	Outside		o = OUTSIDE_INIT(NULL);
	bool		touched = sift_touched_count() > 0;

	for (int si = 0; sift_rowlock_area_mapped() && si < SIFT_ROWLOCK_PARTITIONS; si++)
	{
		FullTransactionId kept = sift_rowlock_kept_at_end(rows, si, touched, TransactionXmin);

		o.horizon = FullTransactionIdIsValid(o.horizon) || !FullTransactionIdIsValid(kept) ? o.horizon : oldest_needed();
		if (!FullTransactionIdIsValid(kept) || !FullTransactionIdPrecedes(kept, o.horizon) || !LWLockConditionalAcquire(stripe_lock(si), LW_EXCLUSIVE))
			continue;
		sift_rowlock_sweep(rows, si, reclaimable_keeping_lockers, &o);
		stripe_unlock(si);
	}
}

/* The MultiXactId of the entry of the row names no running holder: it goes, and the entry with it when nothing else keeps it (the entry may have changed since the caller read the members) */
static void
clear_overflow(const SiftRowLockKey *k, MultiXactId mx)
{
	int			si = sift_rowlock_stripe_of(k);
	SiftRowLockEntry *e;

	sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, k);
	if (e != NULL && e->overflow_mxid == mx)
	{
		e->overflow_mxid = InvalidMultiXactId;
		if (idle(e))
			sift_rowlock_remove(rows, si, e);
	}
	stripe_unlock(si);
}

/* The commit of the last holder of an overflow forgets its MultiXactId: the members are read with no stripe lock held, and the ones of this transaction have ended */
static void
retire_overflow(const SiftRowLockKey *k, MultiXactId mx)
{
	SiftMxMembers members = SIFT_MX_NONE;

	sift_mx_read(&members, mx);
	if (!sift_mx_live(&members, true))
		clear_overflow(k, mx);
	sift_mx_free(&members);
}

/* Gives the row back, the stripe locked: our holders go, our update is the commit's (kept for EvalPlanQual) or the abort's (forgotten); false for an entry that stays because a speculative insert is in flight. *overflow: the MultiXactId to retire once the lock is let go */
static bool
settle_locked(const SiftTouched *t, int si, bool commit, bool keep_speculative, MultiXactId *overflow)
{
	SiftRowLockEntry *e;
	bool		settled;
	int			nholders;

	e = sift_rowlock_find(rows, si, &t->key);
	settled = e == NULL || !keep_speculative || (e->flags & SIFT_ROWLOCK_SPECULATIVE) == 0;
	if (e != NULL && settled)
	{
		sift_spec_end(e, t->xid);
		nholders = e->nholders;
		for (int i = 0; i < e->nholders;)
			if (e->holders[i].xid == t->xid)
				sift_hold_drop(e, i);
			else
				i++;
		if (updater_of(e) == t->xid)
			resolve_updater(e, commit);
		if ((e->flags & SIFT_ROWLOCK_COMMITTED_UPD) != 0 && sift_rowlock_unheld(e))
			sift_rowlock_keep(rows, si, e->updater_fxid);
		if (idle(e) && e->nholders < nholders && in_table(si, e))
		{
			e->flags = SIFT_ROWLOCK_LAST_LOCKER;
			e->updater_fxid = FullTransactionIdFromU64(t->xid);
		}
		else if (idle(e))
			sift_rowlock_remove(rows, si, e);
		else if ((e->flags & ~SIFT_ROWLOCK_LAST_LOCKER) == 0 && e->nholders == 0)
			*overflow = e->overflow_mxid;
	}
	return settled;
}

static bool
settle(const SiftTouched *t, bool commit, bool keep_speculative)
{
	int			si = sift_rowlock_stripe_of(&t->key);
	MultiXactId overflow = InvalidMultiXactId;
	bool		settled;

	sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
	settled = settle_locked(t, si, commit, keep_speculative, &overflow);
	stripe_unlock(si);
	if (commit && MultiXactIdIsValid(overflow))
		retire_overflow(&t->key, overflow);
	return settled;
}

#define SETTLE_WINDOW	4096

/*
 * The rows touched[at[0..n)] given back stripe by stripe, each stripe locked once for all of them (at: NULL for touched[base, base + n)): the end of a transaction takes no memory, so the order of the
 * window is on the stack. settled[j]: the row of at[j] went (a speculative insert in flight keeps it where keep_speculative)
 */
static void
settle_window(const uint32 *at, uint32 base, uint32 n, bool commit, bool keep_speculative, bool *settled)
{
	uint16		order[SETTLE_WINDOW];
	MultiXactId overflow[SETTLE_WINDOW];
	uint32		pos[SETTLE_WINDOW];
	int16		stripe[SETTLE_WINDOW];

	sift_touched_by_stripe(at, base, n, order);
	for (uint32 j = 0; j < n; j++)
	{
		const SiftRowLockKey k = sift_touched_key(pos[j] = at != NULL ? at[order[j]] : base + order[j]);

		stripe[j] = (int16) sift_rowlock_stripe_of(&k);
	}
	for (uint32 lo = 0, hi; lo < n; lo = hi)
	{
		int			si = stripe[lo];

		sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
		for (hi = lo; hi < n && stripe[hi] == si; hi++)
		{
			const SiftTouched t = sift_touched_at(pos[hi]);

			overflow[hi] = InvalidMultiXactId;
			settled[order[hi]] = settle_locked(&t, si, commit, keep_speculative, &overflow[hi]);
		}
		stripe_unlock(si);
		for (uint32 j = lo; commit && j < hi; j++)
			if (MultiXactIdIsValid(overflow[j]))
			{
				const SiftRowLockKey k = sift_touched_key(pos[j]);

				retire_overflow(&k, overflow[j]);
			}
	}
}

static void
settle_all(bool commit)
{
	uint32		n = sift_touched_count();
	bool		settled[SETTLE_WINDOW];

	for (uint32 base = 0; base < n; base += SETTLE_WINDOW)
		settle_window(NULL, base, Min(SETTLE_WINDOW, n - base), commit, false, settled);
}

/* The row stays held after the prepare: the entry says so, and the lock manager gets the tuple lock that a restart finds again (f2_lock_2pc.h) */
static void
prepare_row(const SiftTouched *t)
{
	int			si = sift_rowlock_stripe_of(&t->key);
	SiftRowLockEntry *e;
	SiftMxMembers members = SIFT_MX_NONE;
	MultiXactId overflow = InvalidMultiXactId;
	int			mode = -1;
	Oid			dbid,
				relid;
	bool		first;

	sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &t->key);
	if (e != NULL)
	{
		e->flags |= SIFT_ROWLOCK_PREPARE_HELD;
		mode = sift_hold_held_mode(e);
		overflow = e->overflow_mxid;
	}
	stripe_unlock(si);
	if (MultiXactIdIsValid(overflow))
	{
		sift_mx_read(&members, overflow);
		mode = Max(mode, sift_mx_held_mode(&members));
		sift_mx_free(&members);
	}
	if (mode >= 0 && sift_esc_prepared_ids(&t->key.locator, &dbid, &relid, &first))
		sift_lock_2pc_hold(dbid, relid, t->key.tid, (LockTupleMode) mode, first);
}

/* A subtransaction released: its holder is its parent's, and one of the transaction that the parent has of the row already takes its mode (a loop of savepoints is one holder, not one for each) */
static void
retag_holder(const SiftTouched *t, SubTransactionId parent)
{
	int			si = sift_rowlock_stripe_of(&t->key);
	SiftRowLockEntry *e;

	sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &t->key);
	for (int i = 0; e != NULL && i < e->nholders; i++)
	{
		int			same = -1;

		if (e->holders[i].xid != t->xid)
			continue;
		for (int j = 0; j < e->nholders && same < 0; j++)
			if (j != i && e->holders[j].subxid == parent && TransactionIdIsCurrentTransactionId(e->holders[j].xid))
				same = j;
		if (same < 0)
			e->holders[i].subxid = parent;
		else
		{
			sift_hold_set_mode(e, same, Max(sift_hold_mode(e, same), sift_hold_mode(e, i)));
			sift_hold_drop(e, i);
		}
		break;
	}
	stripe_unlock(si);
}

/* The entry this call took of a row that a transaction that has not ended changed: the row is not held, so that a request that skips it, or waits for that transaction, holds nothing of it */
void
sift_rowlock_release_new(Relation rel, uint64 tid, uint64 part_id)
{
	SiftTouched t = {.key = key_of(&rel->rd_locator, tid),.part_id = part_id,.xid = GetCurrentTransactionId(),.subxid = GetCurrentSubTransactionId()};

	(void) settle(&t, false, false);
	sift_touched_forget(&t.key);
	sift_esc_note_tier1_release(&rel->rd_locator, part_id, 1);
}

/* The tier 2 lock holds the rows of the (sub)transaction now: its entries go, a window of them for each lock of a stripe, but those of the speculative inserts in flight */
void
sift_rowlock_release_tier1(const RelFileLocator *loc, uint64 part_id, SubTransactionId subxid)
{
	uint32		n;
	uint32	   *at = sift_touched_sub_of(loc, part_id, subxid, &n);
	bool		settled[SETTLE_WINDOW];

	for (uint32 lo = 0; lo < n; lo += SETTLE_WINDOW)
	{
		uint32		w = Min(SETTLE_WINDOW, n - lo);

		CHECK_FOR_INTERRUPTS();
		settle_window(at + lo, 0, w, false, true, settled);
		for (uint32 j = 0; j < w; j++)
			if (settled[j])
			{
				sift_esc_note_tier1_release(loc, sift_touched_at(at[lo + j]).part_id, 1);
				(void) sift_touched_remove(at[lo + j]);
			}
	}
	if (at != NULL)
		pfree(at);
}

void
sift_tbl_rowlock_xact_callback(XactEvent event, void *arg pg_attribute_unused())
{
	bool		commit = event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT;

	if (event == XACT_EVENT_PRE_COMMIT && rows != NULL)
		sift_rowlock_area_map(rows);
	if (event == XACT_EVENT_PRE_PREPARE)
	{
		sift_esc_pre_prepare(sift_rowlock_release_tier1);
		SIFT_POINT("sift-rowlock-prepare-rows");
		for (uint32 i = 0; i < sift_touched_count(); i++)
		{
			const SiftTouched t = sift_touched_at(i);

			prepare_row(&t);
		}
		sift_esc_prepared();
		return;
	}
	if (event != XACT_EVENT_PREPARE && !commit && event != XACT_EVENT_ABORT && event != XACT_EVENT_PARALLEL_ABORT)
		return;
	if (event != XACT_EVENT_PREPARE)
		settle_all(commit);
	sift_rowlock_chunks_release();
	if (rows != NULL && (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT))
		reclaim_kept();
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
			SiftTouched t = sift_touched_at((uint32) i);

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
	sift_xact_callback(sift_tbl_rowlock_xact_callback, NULL);
	sift_subxact_callback(sift_tbl_rowlock_subxact_callback, NULL);
}

/* ---- the protocol ---- */

/* The result of what an entry says of the row: the update that left it, a lock that conflicts, or none */
static void
judge_entry(SiftRowLockEntry *e, LockTupleMode mode, SiftRowLockResult *r, const SiftMxMembers *seen, SiftXidAsk *a)
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
		else if (sift_ask_running(a, updater))
			r->result = TM_BeingModified;
		else
		{
			r->result = !sift_ask_committed(a, updater) ? TM_Ok : (e->flags & SIFT_ROWLOCK_UPDATE_CHAIN) != 0 ? TM_Updated : TM_Deleted;
			resolve_updater(e, r->result != TM_Ok);
		}
	}
	if (r->result == TM_Ok)
	{
		r->xid = sift_hold_conflict(e, mode, seen, a);
		if (TransactionIdIsValid(r->xid))
			r->result = TM_BeingModified;
	}
}

/* The mode the current (sub)transaction holds the row in, -1 for none: its holder in the entry, or its member of the overflow that was read */
static int
own_mode(const SiftRowLockEntry *e, const SiftMxMembers *seen)
{
	TransactionId xid = GetCurrentTransactionId();

	for (int i = 0; i < e->nholders; i++)
		if (e->holders[i].xid == xid)
			return sift_hold_mode(e, i);
	return MultiXactIdIsValid(e->overflow_mxid) && sift_mx_has(seen, e->overflow_mxid) ? sift_mx_held_mode(seen) : -1;
}

/* One try of an acquire under the stripe lock: o asks what must be done outside it (the members of the MultiXactId of the entry, a MultiXactId for the holder) before the try is made again, and so does out */
static SiftRowLockResult
try_acquire(Relation rel, const SiftRowLockKey *k, int si, uint64 part_id, LockTupleMode mode, SiftHoldOutside *o, Outside *out)
{
	SiftRowLockResult r = {.result = TM_Ok,.prev_mode = (int) mode};
	SiftRowLockEntry *e = sift_rowlock_find(rows, si, k);

	if (e != NULL && MultiXactIdIsValid(e->overflow_mxid) && !sift_mx_has(&o->seen, e->overflow_mxid))
	{
		o->read = e->overflow_mxid;
		return r;
	}
	if (e != NULL)
		ask_entry(e, mode, &o->ask);
	if ((o->answer = sift_ask_must(&o->ask)))
		return r;
	if (e != NULL)
		reap_prepared(e, &o->seen, &o->ask);
	if (e != NULL && removable(e, &o->ask))
	{
		sift_rowlock_remove(rows, si, e);
		e = NULL;
	}
	if (e != NULL)
		judge_entry(e, mode, &r, &o->seen, &o->ask);
	if (r.result == TM_Ok && e == NULL)
		e = slot_for_write(si, k, out);
	r.stripe_full = r.result == TM_Ok && e == NULL;
	if (r.result == TM_Ok && e != NULL)
		r.prev_mode = own_mode(e, &o->seen);
	if (r.result == TM_Ok && e != NULL && sift_hold_add(e, mode, o))
	{
		r.added = sift_touched_add(k, part_id);
		sift_esc_note_tier1_acquire(&rel->rd_locator, part_id, mode, r.added);
	}
	return r;
}

SiftRowLockResult
sift_tbl_rowlock_acquire(Relation rel, uint64 tid, uint64 part_id, LockTupleMode mode)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = sift_rowlock_stripe_of(&k);
	SiftHoldOutside o = {.ask = {.rel = rel},.seen = SIFT_MX_NONE};
	Outside		out = OUTSIDE_INIT(&o.ask);
	SiftRowLockResult r;

	SIFT_INVARIANT(tier1_ready(), "the row lock table of sift_table is mapped in every backend that locks a row");
	(void) GetCurrentTransactionId();
	for (;;)
	{
		sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
		r = try_acquire(rel, &k, si, part_id, mode, &o, &out);
		stripe_unlock(si);
		if (needs_outside(&out))
			provide(&out);
		else if (!sift_hold_asks(&o))
			break;
		else
			sift_hold_do(&o, mode, &rows->grow_lock[si]);
	}
	sift_rowlock_chunks_release();
	sift_hold_done(&o);
	return r;
}

void
sift_tbl_rowlock_lower(Relation rel, uint64 tid, int prev_mode)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = sift_rowlock_stripe_of(&k);
	TransactionId xid = GetCurrentTransactionId();
	SiftRowLockEntry *e;

	sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
	e = sift_rowlock_find(rows, si, &k);
	for (int i = 0; e != NULL && i < e->nholders; i++)
		if (e->holders[i].xid == xid && prev_mode < 0)
			sift_hold_drop(e, i);
		else if (e->holders[i].xid == xid)
			sift_hold_set_mode(e, i, (LockTupleMode) prev_mode);
	stripe_unlock(si);
}

/* The entry of a row the current (sub)transaction holds, locked, or NULL: a row held by a partition or the relation has none to change, a row still in the stage of this transaction keeps it in
 * updater_fxid (table/f2/spec.h) and its stage records the change, and the staged mark of an inserter that ended goes (the row is in a part); a holder past the room of the entry is a member of its MultiXactId */
static SiftRowLockEntry *
held_entry(const SiftRowLockKey *k, int *si)
{
	SiftMxMembers seen = SIFT_MX_NONE;
	SiftRowLockEntry *e;

	if (!tier1_ready())
		return NULL;
	*si = sift_rowlock_stripe_of(k);
	for (;;)
	{
		MultiXactId unread = InvalidMultiXactId;

		sift_rowlock_stripe_acquire(rows, *si, LW_EXCLUSIVE);
		e = sift_rowlock_find(rows, *si, k);
		if (e != NULL && ((e->flags & SIFT_ROWLOCK_STAGED_INSERT) == 0 || !TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(e->updater_fxid))) &&
			(sift_hold_held_mode(e) >= 0 || (sift_mx_has(&seen, e->overflow_mxid) && sift_mx_held_mode(&seen) >= 0)))
		{
			sift_spec_flushed(e);
			break;
		}
		if (e != NULL && MultiXactIdIsValid(e->overflow_mxid) && !sift_mx_has(&seen, e->overflow_mxid))
			unread = e->overflow_mxid;
		stripe_unlock(*si);
		if (!MultiXactIdIsValid(unread))
		{
			e = NULL;
			break;
		}
		sift_mx_read(&seen, unread);
	}
	sift_mx_free(&seen);
	return e;
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
	e->updated_tid = moved_partition ? SIFT_MOVED_PARTITIONS_TID : new_tid;
	e->updater_fxid = GetCurrentFullTransactionId();
	e->updater_cid = cid;
	stripe_unlock(si);
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
	stripe_unlock(si);
}

bool
sift_tbl_rowlock_set_staged(Relation rel, uint64 tid, uint32 token)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = sift_rowlock_stripe_of(&k);
	SiftRowLockEntry *e;
	SiftXidAsk	ask = {.rel = rel};
	Outside		out = OUTSIDE_INIT(&ask);

	if (!tier1_ready())
		return false;
	for (;;)
	{
		sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
		e = entry_for_write(si, &k, &out);
		if (e != NULL)
		{
			sift_spec_stage(e, GetCurrentFullTransactionId(), token);
			(void) sift_touched_add(&k, SIFT_ESC_PART_ALL);
		}
		stripe_unlock(si);
		if (!needs_outside(&out))
			break;
		provide(&out);
	}
	sift_rowlock_chunks_release();
	return e != NULL;
}

SiftRowLockEntry *
sift_tbl_rowlock_find_locked(Relation rel, uint64 tid, LWLockMode mode, int *si)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	SiftRowLockEntry *e;

	*si = sift_rowlock_stripe_of(&k);
	if (!tier1_ready() || sift_rowlock_stripe_empty(rows, *si))
		return NULL;
	sift_rowlock_stripe_acquire(rows, *si, mode);
	e = sift_rowlock_find(rows, *si, &k);
	if (e == NULL)
		stripe_unlock(*si);
	return e;
}

void
sift_tbl_rowlock_unlock(int si)
{
	stripe_unlock(si);
}

void
sift_tbl_rowlock_drop_idle(int si, SiftRowLockEntry *e)
{
	SiftRowLockKey k = e->key;

	if (!idle(e))
		return;
	sift_rowlock_remove(rows, si, e);
	sift_touched_forget(&k);
}

SiftSpecProbe
sift_tbl_rowlock_probe_staged(Relation rel, uint64 tid, TransactionId *xmin, uint32 *token)
{
	SiftRowLockKey k = key_of(&rel->rd_locator, tid);
	int			si = sift_rowlock_stripe_of(&k);
	SiftRowLockEntry *e;
	SiftSpecProbe r = SIFT_SPEC_NONE;
	FullTransactionId inserter = InvalidFullTransactionId,
				now;
	bool		running = false;

	if (!tier1_ready() || sift_rowlock_stripe_empty(rows, si))
		return r;
	for (;;)
	{
		sift_rowlock_stripe_acquire(rows, si, running ? LW_EXCLUSIVE : LW_SHARED);
		e = sift_rowlock_find(rows, si, &k);
		now = e != NULL && (e->flags & SIFT_ROWLOCK_STAGED_INSERT) != 0 ? e->updater_fxid : InvalidFullTransactionId;
		if (FullTransactionIdEquals(now, inserter))
			break;
		stripe_unlock(si);
		CHECK_FOR_INTERRUPTS();
		inserter = now;
		running = FullTransactionIdIsValid(inserter) && TransactionIdIsInProgress(XidFromFullTransactionId(inserter));
	}
	if (e != NULL)
		r = sift_spec_probe(e, running, xmin, token);
	stripe_unlock(si);
	return r;
}

static bool
last_locker_in(SiftRowLockEntry *e, void *loc)
{
	return (e->flags & SIFT_ROWLOCK_LAST_LOCKER) != 0 && idle(e) && in_relation(e, loc);
}

void
sift_tbl_rowlock_clear_lockers(Relation rel)
{
	if (!tier1_ready())
		return;
	for (int si = 0; si < SIFT_ROWLOCK_PARTITIONS; si++)
	{
		if (sift_rowlock_stripe_empty(rows, si))
			continue;
		sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
		sift_rowlock_sweep(rows, si, last_locker_in, &rel->rd_locator);
		stripe_unlock(si);
	}
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
	if (!tier1_ready())
		return;
	for (int si = 0; si < SIFT_ROWLOCK_PARTITIONS; si++)
	{
		if (sift_rowlock_stripe_empty(rows, si))
			continue;
		sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
		sift_rowlock_sweep(rows, si, in_relation, (void *) loc);
		stripe_unlock(si);
	}
}

typedef struct OverflowRef
{
	SiftRowLockKey key;
	MultiXactId mx;
} OverflowRef;

typedef struct OverflowRefs
{
	const RelFileLocator *loc;
	OverflowRef *refs;
	int			n;
	int			cap;
} OverflowRefs;

static bool
collect_overflow(SiftRowLockEntry *e, void *arg)
{
	OverflowRefs *r = arg;

	if (!MultiXactIdIsValid(e->overflow_mxid) || !RelFileLocatorEquals(e->key.locator, *r->loc))
		return false;
	if (r->n == r->cap)
	{
		/* check-memory: allow the overflows of one stripe, the entries of a stripe and of its chunks that hold more holders than an entry has room for */
		r->cap = Max(16, r->cap * 2);
		r->refs = r->refs == NULL ? palloc(sizeof(OverflowRef) * r->cap) : repalloc(r->refs, sizeof(OverflowRef) * r->cap);
	}
	r->refs[r->n++] = (OverflowRef) {e->key, e->overflow_mxid};
	return false;
}

MultiXactId
sift_tbl_rowlock_vacuum_mxid(Relation rel, MultiXactId oldest_mxact)
{
	MultiXactId floor = oldest_mxact;
	OverflowRefs found = {.loc = &rel->rd_locator};

	if (!tier1_ready())
		return floor;
	for (int si = 0; si < SIFT_ROWLOCK_PARTITIONS; si++)
	{
		CHECK_FOR_INTERRUPTS();
		found.n = 0;
		if (sift_rowlock_stripe_empty(rows, si))
			continue;
		sift_rowlock_stripe_acquire(rows, si, LW_EXCLUSIVE);
		sift_rowlock_sweep(rows, si, collect_overflow, &found);
		stripe_unlock(si);
		for (int i = 0; i < found.n; i++)
		{
			SiftMxMembers members = SIFT_MX_NONE;

			sift_mx_read(&members, found.refs[i].mx);
			if (!sift_mx_live(&members, false))
				clear_overflow(&found.refs[i].key, found.refs[i].mx);
			else if (MultiXactIdPrecedes(found.refs[i].mx, floor))
				floor = found.refs[i].mx;
			sift_mx_free(&members);
		}
	}
	if (found.refs != NULL)
		pfree(found.refs);
	return floor;
}

