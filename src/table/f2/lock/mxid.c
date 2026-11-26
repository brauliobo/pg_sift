/*
 * table/f2/lock/mxid.c - the MultiXactIds of the holders of a row of tier 1 (f2_lock_mxid.h).
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/procarray.h"

#include "table/f2/lock/mxid.h"

static MultiXactStatus
status_of(LockTupleMode m)
{
	static const MultiXactStatus map[4] = {MultiXactStatusForKeyShare, MultiXactStatusForShare, MultiXactStatusForNoKeyUpdate, MultiXactStatusForUpdate};

	return map[m];
}

static LockTupleMode
mode_of(MultiXactStatus s)
{
	return s == MultiXactStatusForKeyShare ? LockTupleKeyShare : s == MultiXactStatusForShare ? LockTupleShare :
		s == MultiXactStatusForNoKeyUpdate ? LockTupleNoKeyExclusive : LockTupleExclusive;
}

void
sift_mx_read(SiftMxMembers *s, MultiXactId mx)
{
	sift_mx_free(s);
	s->mx = mx;
	s->n = GetMultiXactIdMembers(mx, &s->members, false, false);
	if (s->n <= 0)
	{
		s->members = NULL;
		s->n = 0;
		return;
	}
	s->running = palloc(s->n * sizeof(bool));
	for (int i = 0; i < s->n; i++)
		s->running[i] = TransactionIdIsInProgress(s->members[i].xid);
}

void
sift_mx_free(SiftMxMembers *s)
{
	if (s->members != NULL)
		pfree(s->members);
	if (s->running != NULL)
		pfree(s->running);
	*s = (SiftMxMembers) SIFT_MX_NONE;
}

bool
sift_mx_has(const SiftMxMembers *s, MultiXactId mx)
{
	return MultiXactIdIsValid(s->mx) && s->mx == mx;
}

bool
sift_mx_live(const SiftMxMembers *s, bool ignore_own)
{
	for (int i = 0; i < s->n; i++)
		if (TransactionIdIsCurrentTransactionId(s->members[i].xid) ? !ignore_own : s->running[i])
			return true;
	return false;
}

TransactionId
sift_mx_conflict(const SiftMxMembers *s, uint8 conflicting_modes)
{
	for (int i = 0; i < s->n; i++)
		if (!TransactionIdIsCurrentTransactionId(s->members[i].xid) && (conflicting_modes & (1 << mode_of(s->members[i].status))) != 0 && s->running[i])
			return s->members[i].xid;
	return InvalidTransactionId;
}

int
sift_mx_held_mode(const SiftMxMembers *s)
{
	int			held = -1;

	for (int i = 0; i < s->n; i++)
		if (TransactionIdIsCurrentTransactionId(s->members[i].xid))
			held = Max(held, (int) mode_of(s->members[i].status));
	return held;
}

MultiXactId
sift_mx_grow(MultiXactId base, TransactionId first, LockTupleMode first_mode, TransactionId xid, LockTupleMode mode)
{
	MultiXactIdSetOldestMember();
	return MultiXactIdIsValid(base) ? MultiXactIdExpand(base, xid, status_of(mode)) : MultiXactIdCreate(first, status_of(first_mode), xid, status_of(mode));
}

/* ---- the answers of the server, found with no stripe lock held ---- */

enum
{
	XID_RUNNING = 1,
	XID_COMMITTED,
	XID_ABORTED
};

static int
answer_of(const SiftXidAsk *a, TransactionId xid)
{
	for (int i = 0; i < a->n; i++)
		if (a->xid[i] == xid)
			return a->state[i];
	return 0;
}

bool
sift_ask_running(SiftXidAsk *a, TransactionId xid)
{
	int			state;

	if (TransactionIdIsCurrentTransactionId(xid))
		return true;
	if (a == NULL || (state = answer_of(a, xid)) == XID_RUNNING)
		return true;
	if (state != 0)
		return false;
	for (int i = 0; i < a->nq; i++)
		if (a->q[i] == xid)
			return true;
	if (a->nq < SIFT_ASK_MAX)
		a->q[a->nq++] = xid;
	return true;
}

bool
sift_ask_committed(const SiftXidAsk *a, TransactionId xid)
{
	return a != NULL && answer_of(a, xid) == XID_COMMITTED;
}

bool
sift_ask_gone(SiftXidAsk *a, FullTransactionId fxid)
{
	if (a == NULL)
		return false;
	if (FullTransactionIdEquals(a->gone_fxid, fxid))
		return a->gone;
	a->gone_q = fxid;
	return false;
}

bool
sift_ask_must(const SiftXidAsk *a)
{
	return a != NULL && (a->nq > 0 || FullTransactionIdIsValid(a->gone_q)) && a->rounds < SIFT_ASK_ROUNDS;
}

void
sift_ask_answer(SiftXidAsk *a)
{
	CHECK_FOR_INTERRUPTS();
	if (a->n + a->nq > SIFT_ASK_MAX)
		a->n = 0;
	for (int i = 0; i < a->nq; i++)
	{
		a->xid[a->n] = a->q[i];
		a->state[a->n++] = TransactionIdIsInProgress(a->q[i]) ? XID_RUNNING : TransactionIdDidCommit(a->q[i]) ? XID_COMMITTED : XID_ABORTED;
	}
	if (FullTransactionIdIsValid(a->gone_q))
	{
		a->gone = GlobalVisCheckRemovableFullXid(a->rel, a->gone_q);
		a->gone_fxid = a->gone_q;
	}
	a->nq = 0;
	a->gone_q = InvalidFullTransactionId;
	a->rounds++;
}

/* ---- the holders of an entry ---- */

static const uint8 conflict_with[4] = {
	[LockTupleKeyShare] = 1 << LockTupleExclusive,
	[LockTupleShare] = (1 << LockTupleNoKeyExclusive) | (1 << LockTupleExclusive),
	[LockTupleNoKeyExclusive] = (1 << LockTupleShare) | (1 << LockTupleNoKeyExclusive) | (1 << LockTupleExclusive),
	[LockTupleExclusive] = 0xF,
};

/* A MultiXactId whose members are not read is live: only the members, read with no stripe lock held, say that none runs */
bool
sift_hold_overflow_live(const SiftRowLockEntry *e, const SiftMxMembers *seen)
{
	return MultiXactIdIsValid(e->overflow_mxid) && (seen == NULL || !sift_mx_has(seen, e->overflow_mxid) || sift_mx_live(seen, false));
}

void
sift_hold_drop_ended(SiftRowLockEntry *e, SiftXidAsk *a)
{
	for (int i = 0; i < e->nholders;)
		if (sift_ask_running(a, e->holders[i].xid))
			i++;
		else
			sift_hold_drop(e, i);
}

void
sift_hold_ask(const SiftRowLockEntry *e, LockTupleMode mode, SiftXidAsk *a)
{
	bool		all = e->nholders == SIFT_ROWLOCK_INLINE_MAX || (e->flags & SIFT_ROWLOCK_PREPARE_HELD) != 0;

	for (int i = 0; i < e->nholders; i++)
		if (all || (conflict_with[mode] & (1 << sift_hold_mode(e, i))) != 0)
			(void) sift_ask_running(a, e->holders[i].xid);
}

/* The first other running transaction whose lock conflicts with mode, or InvalidTransactionId; a holder that ended is dropped from the entry, and an overflow of holders that all ended is forgotten (the caller holds its stripe exclusively and read the members of the overflow) */
TransactionId
sift_hold_conflict(SiftRowLockEntry *e, LockTupleMode mode, const SiftMxMembers *seen, SiftXidAsk *a)
{
	TransactionId found;

	for (int i = 0; i < e->nholders;)
	{
		TransactionId xid = e->holders[i].xid;

		if (TransactionIdIsCurrentTransactionId(xid) || (conflict_with[mode] & (1 << sift_hold_mode(e, i))) == 0)
			i++;
		else if (sift_ask_running(a, xid))
			return xid;
		else
			sift_hold_drop(e, i);
	}
	if (!MultiXactIdIsValid(e->overflow_mxid))
		return InvalidTransactionId;
	found = sift_mx_conflict(seen, conflict_with[mode]);
	if (!TransactionIdIsValid(found) && !sift_mx_live(seen, false))
		e->overflow_mxid = InvalidMultiXactId;
	return found;
}

/* False where the holder needs a MultiXactId that is not made yet: *o asks for it, the caller makes it with no stripe lock held and asks again */
bool
sift_hold_add(SiftRowLockEntry *e, LockTupleMode mode, SiftHoldOutside *o)
{
	TransactionId xid = GetCurrentTransactionId();

	for (int i = 0; i < e->nholders; i++)
		if (e->holders[i].xid == xid)
		{
			if (mode > sift_hold_mode(e, i))
				sift_hold_set_mode(e, i, mode);
			return true;
		}
	if (e->nholders == SIFT_ROWLOCK_INLINE_MAX)
		sift_hold_drop_ended(e, &o->ask);
	if (e->nholders < SIFT_ROWLOCK_INLINE_MAX)
	{
		e->holders[e->nholders] = (SiftRowLockHolder) {xid, GetCurrentSubTransactionId()};
		sift_hold_set_mode(e, e->nholders++, mode);
		return true;
	}
	if (MultiXactIdIsValid(e->overflow_mxid) && sift_mx_held_mode(&o->seen) >= (int) mode)
		return true;
	if (!MultiXactIdIsValid(o->grown) || o->grown_from != e->overflow_mxid || (!MultiXactIdIsValid(e->overflow_mxid) && o->grown_first != e->holders[0].xid))
	{
		o->grow = true;
		o->base = e->overflow_mxid;
		o->first = e->holders[0].xid;
		o->first_mode = sift_hold_mode(e, 0);
		return false;
	}
	e->overflow_mxid = o->grown;
	o->grown = InvalidMultiXactId;
	return true;
}

/* What the acquire asked, done with no stripe lock held: the first ask to grow takes the grow lock and tries again, for the MultiXactId another holder may have grown meanwhile */
void
sift_hold_do(SiftHoldOutside *o, LockTupleMode mode, LWLock *grow)
{
	CHECK_FOR_INTERRUPTS();
	if (o->answer)
		sift_ask_answer(&o->ask);
	if (MultiXactIdIsValid(o->read))
		sift_mx_read(&o->seen, o->read);
	if (o->grow && o->grow_lock == NULL)
	{
		LWLockAcquire(grow, LW_EXCLUSIVE);
		o->grow_lock = grow;
	}
	else if (o->grow)
	{
		o->grown = sift_mx_grow(o->base, o->first, o->first_mode, GetCurrentTransactionId(), mode);
		o->grown_from = o->base;
		o->grown_first = o->first;
	}
	o->read = InvalidMultiXactId;
	o->grow = o->answer = false;
}

void
sift_hold_done(SiftHoldOutside *o)
{
	if (o->grow_lock != NULL)
		LWLockRelease(o->grow_lock);
	o->grow_lock = NULL;
	sift_mx_free(&o->seen);
}

/* The strongest mode of the transaction among the inline holders, -1 for none: a holder that was a subtransaction's is the transaction's once it is released */
int
sift_hold_held_mode(const SiftRowLockEntry *e)
{
	int			held = -1;

	for (int i = 0; i < e->nholders; i++)
		if (TransactionIdIsCurrentTransactionId(e->holders[i].xid))
			held = Max(held, (int) sift_hold_mode(e, i));
	return held;
}

