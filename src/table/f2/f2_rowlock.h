/*
 * table/f2/f2_rowlock.h - tier 1 of the row locks of a sift_table: 128 stripes of shared memory, 128 bytes an entry, no WAL.
 *
 * An entry names one row (relation file and address). It holds the shared lockers, the updater with the successor address while EvalPlanQual may still ask for it, the latest column
 * delta of the row, and a speculative insertion token. The entry of a committed update stays until the updater is older than every snapshot (GlobalVis), so that a waiter woken
 * by the commit finds the successor. A transaction that locks more than SIFT_ROWLOCK_ESCALATE_AT rows holds the partition or the relation instead (f2_lock_esc.h).
 */
#ifndef SIFT_TBL_F2_ROWLOCK_H
#define SIFT_TBL_F2_ROWLOCK_H

#include "postgres.h"

#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "port/pg_bitutils.h"
#include "storage/lwlock.h"
#include "storage/relfilelocator.h"

#define SIFT_ROWLOCK_PARTITIONS		128
#define SIFT_ROWLOCK_INLINE_MAX		8
#define SIFT_ROWLOCK_BUCKETS		64	/* entries a stripe holds in the table; the ones past them are chunks of the area (f2_lock_ovf.h) */

#define SIFT_ROWLOCK_UPDATE_CHAIN	(UINT16_C(1) << 0)	/* updated_tid is valid */
#define SIFT_ROWLOCK_SPECULATIVE	(UINT16_C(1) << 1)	/* an ON CONFLICT insert of the row is in flight */
#define SIFT_ROWLOCK_COMMITTED_UPD	(UINT16_C(1) << 2)	/* the updater committed; kept until GlobalVis for EvalPlanQual */
#define SIFT_ROWLOCK_DELETED		(UINT16_C(1) << 3)	/* the row was deleted, not updated */
#define SIFT_ROWLOCK_COL_DELTA		(UINT16_C(1) << 4)	/* the latest update of the address is a column delta: the address stays valid */
#define SIFT_ROWLOCK_PREPARE_HELD	(UINT16_C(1) << 5)	/* a holder or the updater is a prepared transaction: reclaimed when it ends */
#define SIFT_ROWLOCK_STAGED_INSERT	(UINT16_C(1) << 6)	/* the row is in the stage of its inserter (updater_fxid), not in a part yet (table/f2/f2_spec.h) */
#define SIFT_ROWLOCK_NEED_FLUSH		(UINT16_C(1) << 7)	/* a probe of a dirty snapshot asked the inserter to write the stage */

#define SIFT_ROWLOCK_ENDING_FLAGS	(SIFT_ROWLOCK_UPDATE_CHAIN | SIFT_ROWLOCK_DELETED | SIFT_ROWLOCK_COL_DELTA)

#ifdef SIFT_LOCK_ESC_STANDALONE
#define SIFT_BACKEND_LOCAL			static __thread
#else
#define SIFT_BACKEND_LOCAL			static
#endif

typedef struct SiftRowLockKey
{
	RelFileLocator locator;
	uint32		pad;			/* zero: the key is compared and hashed as bytes */
	uint64		tid;			/* (virtual block << 16) | offset */
} SiftRowLockKey;

typedef struct SiftRowLockHolder
{
	TransactionId xid;			/* the (sub)transaction that took the lock */
	SubTransactionId subxid;
} SiftRowLockHolder;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftRowLockEntry
{
	SiftRowLockKey key;			/* 0..23 */
	uint16		flags;			/* 24: SIFT_ROWLOCK_* */
	uint16		nholders;		/* 26 */
	uint32		spec_token;		/* 28 */
	uint64		updated_tid;	/* 32: the successor, or the moved-partitions address */
	FullTransactionId updater_fxid;	/* 40 */
	CommandId	updater_cid;	/* 48 */
	uint32		holder_modes;	/* 52: 2 bits of LockTupleMode for each of the holders */
	MultiXactId overflow_mxid;	/* 56: the holders past SIFT_ROWLOCK_INLINE_MAX */
	uint32		delta_ver;		/* 60: column deltas the row had while the entry lived */
	SiftRowLockHolder holders[SIFT_ROWLOCK_INLINE_MAX];	/* 64..127 */
} SiftRowLockEntry;

StaticAssertDecl(sizeof(SiftRowLockEntry) == 128, "SiftRowLockEntry must occupy 128 bytes");
StaticAssertDecl(offsetof(SiftRowLockEntry, delta_ver) == 60, "delta_ver sits at byte 60 of the entry");

typedef struct SiftRowLockStripe
{
	SiftRowLockEntry entries[SIFT_ROWLOCK_BUCKETS];
} SiftRowLockStripe;

/* The outcome of a lock request: granted, or why not (the transaction to wait for, the successor of an updated row); stripe_full: the area has no memory for an entry, the caller escalates */
typedef struct SiftRowLockResult
{
	TM_Result	result;
	TransactionId xid;
	CommandId	cid;
	uint64		updated_tid;
	bool		stripe_full;
} SiftRowLockResult;

/* Takes the lock of the mode on the row of the partition for the current (sub)transaction unless it conflicts: the caller waits for the xid of a TM_BeingModified result and asks again */
extern SiftRowLockResult sift_tbl_rowlock_acquire(Relation rel, uint64 tid, uint64 part_id, LockTupleMode mode);

/* The row is updated to new_tid (or deleted, or moved to another partition) by the current transaction: the entry keeps it until the transaction ends */
extern void sift_tbl_rowlock_mark_updated(Relation rel, uint64 old_tid, uint64 new_tid, CommandId cid, bool deleted, bool moved_partition);

/* The row is changed in place by a column delta of the current transaction: the entry keeps its address, the writer and the delta count */
extern void sift_tbl_rowlock_mark_col_delta(Relation rel, uint64 tid, CommandId cid);
extern void sift_tbl_rowlock_set_speculative(Relation rel, uint64 tid, uint32 token, bool on);
extern uint32 sift_tbl_rowlock_speculative_token(Relation rel, uint64 tid);

/* The speculative insert of the row ended (table/f2/f2_spec.h): true when a probe asked for the stage that holds it to be written; the stage was written; what a dirty probe of an address no part holds learns */
extern bool sift_tbl_rowlock_spec_done(Relation rel, uint64 tid, bool succeeded);
extern void sift_tbl_rowlock_staged_flushed(Relation rel, uint64 tid);
extern bool sift_tbl_rowlock_successor(Relation rel, uint64 old_tid, uint64 *succ);

/* The transaction holds a partition or the relation, not row by row (f2_lock_esc.h) */
extern bool sift_tbl_rowlock_is_escalated(Relation rel);
extern void sift_tbl_rowlock_drop_rel(const RelFileLocator *loc);
extern void sift_tbl_rowlock_xact_callback(XactEvent event, void *arg);
extern void sift_tbl_rowlock_subxact_callback(SubXactEvent event, SubTransactionId mysubid, SubTransactionId parentsubid, void *arg);

#endif							/* SIFT_TBL_F2_ROWLOCK_H */
