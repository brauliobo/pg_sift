/*
 * table/f2/lock/mxid.h - the holders of a row of tier 1 (f2_rowlock.h): the ones the entry has room for, and, past them, the members of a MultiXactId. The members of a MultiXactId never change, so a
 * caller reads them, or makes the MultiXactId that adds a holder, with no stripe lock held: the multixact SLRU does I/O and writes WAL, and a stripe holds a 128th of the rows of every table.
 */
#ifndef SIFT_TABLE_F2_LOCK_MXID_H
#define SIFT_TABLE_F2_LOCK_MXID_H

#include "postgres.h"

#include "access/multixact.h"
#include "access/tableam.h"
#include "utils/rel.h"

#include "table/f2/lock/rowlock.h"

/* LockTupleMode of a holder of an entry: two bits of holder_modes (f2_rowlock.h) */
static inline LockTupleMode
sift_hold_mode(const SiftRowLockEntry *e, int i)
{
	return (LockTupleMode) ((e->holder_modes >> (2 * i)) & 3);
}

static inline void
sift_hold_set_mode(SiftRowLockEntry *e, int i, LockTupleMode m)
{
	e->holder_modes = (e->holder_modes & ~(UINT32_C(3) << (2 * i))) | ((uint32) m << (2 * i));
}

static inline void
sift_hold_drop(SiftRowLockEntry *e, int i)
{
	e->holders[i] = e->holders[--e->nholders];
	sift_hold_set_mode(e, i, sift_hold_mode(e, e->nholders));
}

typedef struct SiftMxMembers
{
	MultiXactId mx;				/* InvalidMultiXactId: nothing read */
	MultiXactMember *members;
	int			n;
} SiftMxMembers;

extern void sift_mx_read(SiftMxMembers *s, MultiXactId mx);
extern void sift_mx_free(SiftMxMembers *s);

/* Whether s holds the members of mx */
extern bool sift_mx_has(const SiftMxMembers *s, MultiXactId mx);

/* A member runs: one of this transaction, or one the server knows in progress; ignore_own leaves out the ones of this transaction (it ends) */
extern bool sift_mx_live(const SiftMxMembers *s, bool ignore_own);

/* The first running member of another transaction whose mode is in the set of modes (1 << LockTupleMode) that conflict with the request, or InvalidTransactionId */
extern TransactionId sift_mx_conflict(const SiftMxMembers *s, uint8 conflicting_modes);

/* The strongest LockTupleMode of the members that are this transaction's, -1 for none */
extern int	sift_mx_held_mode(const SiftMxMembers *s);

/* The MultiXactId of base with xid added in the mode; without a base, of first and xid. Makes the multixact: not under a stripe lock */
extern MultiXactId sift_mx_grow(MultiXactId base, TransactionId first, LockTupleMode first_mode, TransactionId xid, LockTupleMode mode);

/* ---- the holders of an entry: those it has room for, and the MultiXactId of the rest ---- */

/* What an acquire learns of the MultiXactId of an entry, and makes for a new holder, with no stripe lock held, and what it asks to be done outside the lock before it tries again */
typedef struct SiftHoldOutside
{
	SiftMxMembers seen;			/* the members of the MultiXactId the entry names */
	MultiXactId grown;			/* a MultiXactId that adds the holder to grown_from (or to grown_first alone where there is none) */
	MultiXactId grown_from;
	TransactionId grown_first;
	MultiXactId read;			/* asked: read the members of this one */
	bool		grow;			/* asked: make the MultiXactId for the holder, from base (or first) */
	MultiXactId base;
	TransactionId first;
	LockTupleMode first_mode;
} SiftHoldOutside;

static inline bool
sift_hold_asks(const SiftHoldOutside *o)
{
	return MultiXactIdIsValid(o->read) || o->grow;
}

extern bool sift_hold_running(TransactionId xid);
extern bool sift_hold_overflow_live(const SiftRowLockEntry *e, const SiftMxMembers *seen);
extern void sift_hold_drop_ended(SiftRowLockEntry *e);
extern TransactionId sift_hold_conflict(SiftRowLockEntry *e, LockTupleMode mode, const SiftMxMembers *seen);
extern bool sift_hold_add(SiftRowLockEntry *e, LockTupleMode mode, SiftHoldOutside *o);
extern void sift_hold_do(SiftHoldOutside *o, LockTupleMode mode);
extern int	sift_hold_held_mode(const SiftRowLockEntry *e);

/*
 * The relminmxid VACUUM of rel may write: the oldest MultiXactId that an entry of the row locks of rel still names for a holder that runs, or oldest_mxact where there is none. The MultiXactIds
 * of holders that all ended are given up on the way, so that no entry keeps one that the truncation of the multixact SLRU has removed.
 */
extern MultiXactId sift_tbl_rowlock_vacuum_mxid(Relation rel, MultiXactId oldest_mxact);

#endif
