/*
 * table/f2/f2_lock_esc.h - tiers 2 and 3 of the row locks of a sift_table: the partition and the relation held for a transaction, its subtransactions and its prepare.
 *
 * Tier 1 (f2_rowlock.h) is a row entry in shared memory. A transaction that locks SIFT_ROWLOCK_ESCALATE_AT rows of a partition, or finds no memory for an entry (f2_lock_ovf.h), holds the partition (tier 2A) and, after
 * SIFT_ROWLOCK_PART_TO_REL_AT partitions of a relation, the relation (tier 2B): a page lock of the relation at a virtual block (core/part_lock.h), so that the deadlock detector sees every wait.
 * Every row locker holds the page locks of its relation and partition in an intention mode, once, before it takes an entry: the intention modes (1..3) are compatible with each other, so the rows
 * are arbitrated by the entries; a coarse holder conflicts with exactly the row lockers whose mode conflicts with its own (sift_esc_modes_conflict). A plain read takes no lock of either tier.
 * The table below the stripes is the directory of the coarse holders: whom a NOWAIT or SKIP LOCKED request names as the one in its way, and what a prepared transaction left behind.
 *
 * Without the shared table (no room in /dev/shm) every row lock is a coarse lock of the partition or the relation (SIFT_LOCK_BACKEND_PGLM): the lock manager of the server does all of it.
 */
#ifndef SIFT_TBL_F2_LOCK_ESC_H
#define SIFT_TBL_F2_LOCK_ESC_H

#include "postgres.h"

#include "storage/lmgr.h"

#include "core/part_lock.h"
#include "table/f2/f2_lock_ovf.h"
#include "table/f2/f2_rowlock.h"

#define SIFT_LOCK_REL_KEY_BLKNO		0xFFFFD000U		/* the key block of the whole relation (the row block is SIFT_LOCK_REL_BULK_BLKNO) */
#define SIFT_LOCK_PART_KEY_BASE		0xFFFFD001U		/* the key block of one partition, the row block of which is SIFT_LOCK_PART_ESC_BLKNO */
#define SIFT_LOCK_PART_KEY_BLKNO(part_id) (SIFT_LOCK_PART_KEY_BASE + (BlockNumber) ((part_id) % SIFT_LOCK_PART_ESC_SLOTS))

StaticAssertDecl(SIFT_LOCK_PART_KEY_BASE + SIFT_LOCK_PART_ESC_SLOTS <= SIFT_LOCK_REL_BULK_BLKNO, "the key blocks end before the row blocks of the escalation");
StaticAssertDecl(SIFT_LOCK_REL_KEY_BLKNO >= 0xFFFF0000U, "the key blocks are above the blocks of a relation");

#define SIFT_ROWLOCK_ESCALATE_AT	4096	/* rows of one partition a transaction locks one by one before it holds the partition */
#define SIFT_ROWLOCK_REL_ROW_CAP	4096	/* rows of one relation, over all its partitions, before the heaviest partition is held */
#define SIFT_ROWLOCK_PART_TO_REL_AT	8		/* partitions of one relation held before the relation is */
#define SIFT_ROWLOCK_RETRY_ROWS		256		/* rows a NOWAIT or SKIP LOCKED request goes on one by one after the partition was refused before it asks again */

#define SIFT_ESC_STRIPES			16
#define SIFT_ESC_BUCKETS			32
#define SIFT_ESC_PART_ALL			UINT64CONST(0)	/* the whole relation; a partition id is 1 or more */
#define SIFT_ESC_FLAG_ACTIVE		(UINT8_C(1) << 0)
#define SIFT_ESC_FLAG_PREPARED		(UINT8_C(1) << 1)	/* the holder is a prepared transaction: gone when it ends */

#define SIFT_ROWLOCK_DSM_NAME		"sift_rowlock"
#define SIFT_ROWLOCK_TRANCHE		"sift_rowlock"
#define SIFT_ESC_TRANCHE			"sift_lock_esc"

typedef struct pg_attribute_aligned(64) SiftPartEscEntry
{
	RelFileLocator locator;		/* 12 */
	uint32		pad0;
	uint64		part_id;		/* SIFT_ESC_PART_ALL: the relation */
	TransactionId top_xid;
	TransactionId sub_eff_xid;	/* the (sub)transaction that took the lock */
	SubTransactionId subxid;
	uint8		mode;			/* LockTupleMode */
	uint8		flags;
	uint16		pad1;
	uint8		reserved[24];
} SiftPartEscEntry;

StaticAssertDecl(sizeof(SiftPartEscEntry) == 64, "SiftPartEscEntry must occupy one cache line");

typedef struct SiftEscStripe
{
	SiftPartEscEntry entries[SIFT_ESC_BUCKETS];
} SiftEscStripe;

/* The one named segment of the table: both tiers and their locks, 1.05 MiB; the entries in use of a directory stripe are counted beside the stripes (a count inside would pad a stripe by a cache line) */
typedef struct SiftRowLockShmLayout
{
	SiftRowLockTable rows;
	SiftEscStripe esc_stripes[SIFT_ESC_STRIPES];
	LWLockPadded esc_locks[SIFT_ESC_STRIPES];
	uint16		esc_n[SIFT_ESC_STRIPES];
} SiftRowLockShmLayout;

/*
 * What a row lock of the mode takes of the relation and partition locks, in two page locks: the row block holds the intention and coarse modes that arbitrate Share, NoKeyExclusive and
 * Exclusive, the key block the ones that tell a KeyShare holder from a NoKeyExclusive one (both are RowExclusive on the row block, and only Exclusive conflicts with KeyShare). A lock of the
 * mode takes its modes on the key block first, then on the row block. NoLock: the block is not taken. The conflicts of the four modes are those of the heap's tuple locks:
 *
 *	held \ asked		KeyShare	Share		NoKeyExcl	Exclusive
 *	KeyShare			-			-			-			conflict
 *	Share				-			-			conflict	conflict
 *	NoKeyExclusive		-			conflict	conflict	conflict
 *	Exclusive			conflict	conflict	conflict	conflict
 */
typedef struct SiftEscModes
{
	LOCKMODE	row;
	LOCKMODE	key;
} SiftEscModes;

static inline SiftEscModes
sift_esc_intent_modes(LockTupleMode mode)
{
	switch (mode)
	{
		case LockTupleKeyShare:
			return (SiftEscModes) {AccessShareLock, NoLock};
		case LockTupleShare:
			return (SiftEscModes) {RowShareLock, NoLock};
		case LockTupleNoKeyExclusive:
			return (SiftEscModes) {RowExclusiveLock, NoLock};
		case LockTupleExclusive:
			return (SiftEscModes) {RowExclusiveLock, RowExclusiveLock};
	}
	pg_unreachable();
}

static inline SiftEscModes
sift_esc_coarse_modes(LockTupleMode mode)
{
	switch (mode)
	{
		case LockTupleKeyShare:
			return (SiftEscModes) {AccessShareLock, ShareLock};
		case LockTupleShare:
			return (SiftEscModes) {ShareLock, NoLock};
		case LockTupleNoKeyExclusive:
			return (SiftEscModes) {ExclusiveLock, NoLock};
		case LockTupleExclusive:
			return (SiftEscModes) {AccessExclusiveLock, NoLock};
	}
	pg_unreachable();
}

static inline bool
sift_esc_modes_conflict(SiftEscModes a, SiftEscModes b)
{
	return (a.row != NoLock && b.row != NoLock && DoLockModesConflict(a.row, b.row)) ||
		(a.key != NoLock && b.key != NoLock && DoLockModesConflict(a.key, b.key));
}

/* A transition of the locks of a (sub)transaction, to undo when it rolls back: what the relation or partition held before it */
typedef struct SiftEscSubRecord
{
	RelFileLocator locator;
	uint64		part_id;		/* SIFT_ESC_PART_ALL: the relation */
	SubTransactionId subxid;
	TransactionId holder_xid;
	int8		prev_intent;	/* -1: none, else a LockTupleMode */
	int8		prev_esc;		/* -1: not held coarsely, else a LockTupleMode */
	int8		prev_held_max;	/* the strongest entry taken before, -1 for none */
	SubTransactionId prev_esc_sub;	/* the (sub)transaction that held the coarse lock before */
	bool		took_split_pin;	/* this one made the transaction hold SIFT_F2_SPLIT_LOCK_BLKNO */
	uint32		chain;			/* the next record of its bucket, plus one: the log finds a record by a hash */
} SiftEscSubRecord;

/* Tier 1 gives back the entries the (sub)transaction owns in the partition (SIFT_ESC_PART_ALL: all partitions of the relation) once a coarse lock holds them */
typedef void (*SiftTier1ReleaseCb) (const RelFileLocator *loc, uint64 part_id, SubTransactionId subxid);

/* The memory the locks of the transaction keep, in tier 1 and here: it is reset when the transaction ends, so an idle backend holds none */
extern MemoryContext sift_rowlock_txn_cx(void);

/* The table in shared memory of this backend; NULL where /dev/shm has no room, and the whole of the locking is in the lock manager of the server */
extern SiftRowLockShmLayout *sift_lock_shm_attach(void);
extern bool sift_lock_pglm_only(void);

/*
 * Before an entry is taken: *covered when a lock held already holds the row (take no entry). Otherwise the intention locks of the relation and partition. *reread: a lock was waited for or
 * is new, so the row on disk must be read again before it is judged. TM_BeingModified (tmfd->xmax: the holder in the way, when the directory knows it) where the policy does not wait.
 */
extern TM_Result sift_esc_before_row_lock(Relation rel, uint64 part_id, LockTupleMode mode, LockWaitPolicy wait_policy, bool *covered, bool *reread, TM_FailureData *tmfd);

/* Whether the request holds the partition or the relation now: a threshold, a mode over the one held, a full stripe, or the lock manager alone */
extern bool sift_esc_should_escalate(const RelFileLocator *loc, uint64 part_id, LockTupleMode mode, bool stripe_full);

/*
 * Holds the partition (or the relation) for the transaction, waiting for every row locker in it unless the policy forbids; once it is held the entries of the current subtransaction go
 * back to tier 1 through release_cb. *escalated false and TM_Ok: a NOWAIT or SKIP LOCKED request that was refused the partition and can go on row by row.
 */
extern TM_Result sift_esc_promote(Relation rel, uint64 part_id, uint32 nparts, LockTupleMode mode, LockWaitPolicy wait_policy, bool stripe_full,
								  SiftTier1ReleaseCb release_cb, bool *escalated, bool *reread, TM_FailureData *tmfd);

/* An entry was taken for a row of the partition (new_row: the (sub)transaction did not hold it before) or released: the counts the thresholds are tested by */
extern void sift_esc_note_tier1_acquire(const RelFileLocator *loc, uint64 part_id, LockTupleMode mode, bool new_row);
extern void sift_esc_note_tier1_release(const RelFileLocator *loc, uint64 part_id, uint32 count);

/* Whether a prepared transaction may hold row locks of the relation in the lock manager (f2_lock_2pc.h): asked once for each transaction and relation, and not at all without prepared transactions */
extern bool sift_esc_prepared_locks_possible(Relation rel);

/* The ids of the lock manager of the relation that a row the transaction holds by entry belongs to; *first: PREPARE has not marked the relation yet (asked once for each relation) */
extern bool sift_esc_prepared_ids(const RelFileLocator *loc, Oid *dbid, Oid *relid, bool *first);
extern bool sift_esc_rel_is_escalated(const RelFileLocator *loc);

/* The transaction is about to prepare: partitions nobody else is in are held coarsely (their entries go), the holders it keeps are marked prepared */
extern void sift_esc_pre_prepare(SiftTier1ReleaseCb release_cb);
extern void sift_esc_xact_end(XactEvent event);
extern void sift_esc_subxact_event(SubXactEvent event, SubTransactionId mysubid, SubTransactionId parentsubid);
extern void sift_esc_drop_rel(const RelFileLocator *loc);

/* The holders in the directory, after those of ended prepared transactions are reclaimed; the one in the way of a request for the mode of the partition, or InvalidTransactionId */
extern uint32 sift_esc_live_count(void);
extern TransactionId sift_esc_find_conflicting_xid(const RelFileLocator *loc, uint64 part_id, LockTupleMode mode);

#endif							/* SIFT_TBL_F2_LOCK_ESC_H */
