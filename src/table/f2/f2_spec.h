/*
 * table/f2/f2_spec.h - the handshake of an ON CONFLICT insert with the backends that probe the unique index it is about to enter.
 *
 * The row of a speculative insert is staged in memory of its backend and reaches a part only when the stage is flushed, so a concurrent probe with a dirty snapshot (the check of a unique index) that
 * finds the entry of the row in the index finds no row at its address. The entry of the row in the row lock table (table/f2/f2_rowlock.h, still 128 bytes) says it is there: SIFT_ROWLOCK_STAGED_INSERT
 * with the inserter in updater_fxid (no updater lives in an entry whose row was never visible), the token of the insertion while it is speculative, and SIFT_ROWLOCK_NEED_FLUSH once a probe
 * has asked the inserter to write the stage out. These functions change the flags of one entry; the caller holds its stripe.
 */
#ifndef SIFT_TBL_F2_SPEC_H
#define SIFT_TBL_F2_SPEC_H

#include "table/f2/f2_rowlock.h"

/* The row is staged by an insert of the inserter (token: of the speculative insertion, 0 for none) */
extern void sift_spec_stage(SiftRowLockEntry *e, FullTransactionId inserter, uint32 token);

/* The insertion succeeded: it is no longer speculative. True when a probe asked for the stage to be written, which the caller does now and then calls sift_spec_flushed */
extern bool sift_spec_succeeded(SiftRowLockEntry *e);

/* The stage that held the row was written: no probe needs to ask for it any more */
extern void sift_spec_flushed(SiftRowLockEntry *e);

/* The insertion was undone: nothing of the row is left to find */
extern void sift_spec_aborted(SiftRowLockEntry *e);

typedef enum SiftSpecProbe
{
	SIFT_SPEC_NONE,				/* no staged row at the address: it is not there */
	SIFT_SPEC_ACTIVE,			/* its inserter is still running: wait for it (xmin, and token while it is speculative) */
	SIFT_SPEC_ENDED				/* its inserter is over: the row is in a part now (committed) or nowhere (aborted); look again */
} SiftSpecProbe;

/* What a probe of a dirty snapshot learns about an address no part holds; inserter_running: the inserter is a running transaction. An active inserter is asked to flush */
extern SiftSpecProbe sift_spec_probe(SiftRowLockEntry *e, bool inserter_running, TransactionId *xmin, uint32 *token);

/* The probe of the address of a row in the row lock table (f2_lock.c): the entry is looked at under its stripe, and the inserter of an active one is asked to write its stage */
extern SiftSpecProbe sift_tbl_rowlock_probe_staged(Relation rel, uint64 tid, TransactionId *xmin, uint32 *token);

/* The (sub)transaction of xid ends: the staged mark of its rows goes with the transaction */
extern void sift_spec_end(SiftRowLockEntry *e, TransactionId xid);

#endif							/* SIFT_TBL_F2_SPEC_H */
