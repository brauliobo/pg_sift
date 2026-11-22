/*
 * core/xid_cache.h - what a scan, a settle or a compaction asks of the transaction log about a transaction id, remembered.
 *
 * The rows of the small parts and of the slab of a table under write load come from a handful of transactions interleaved by key, so the one-entry cache of the transaction log
 * (cachedFetchXid) misses nearly every row and each miss takes the lock of an SLRU bank. A cache of SIFT_XID_CACHE_SIZE ids, on the stack of the caller, asks the log once for each
 * id: a snapshot never changes its mind about a transaction that is no longer running, and a settle or a compaction asks for what a transaction is at the moment its pass began.
 */
#ifndef SIFT_XID_CACHE_H
#define SIFT_XID_CACHE_H

#include "core/host.h"

#include "access/transam.h"
#include "utils/snapshot.h"

#define SIFT_XID_CACHE_SIZE		64
#define SIFT_XID_CACHE_PROBES	4

/* The id as a full one, the epoch that of next, the next transaction id read once by the caller; an id that is not normal is the frozen one (epoch 0) */
static inline FullTransactionId
sift_xid_fxid_at(FullTransactionId next, TransactionId xid)
{
	return TransactionIdIsNormal(xid) ? FullTransactionIdFromAllowableAt(next, xid) : FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);
}

/* The greater of two full ids, an invalid one being the least */
static inline FullTransactionId
sift_fxid_max(FullTransactionId a, FullTransactionId b)
{
	return !FullTransactionIdIsValid(a) || FullTransactionIdFollows(b, a) ? b : a;
}

typedef enum SiftXidStatus
{
	SIFT_XSTAT_EMPTY = 0,
	SIFT_XSTAT_COMMITTED,			/* committed before the snapshot: its rows are there for it */
	SIFT_XSTAT_NOT_SEEN,			/* running, or committed after the snapshot: its rows are not there for it */
	SIFT_XSTAT_ABORTED,
	SIFT_XSTAT_SELF				/* the transaction itself or one of its subtransactions: the command decides */
} SiftXidStatus;

typedef struct SiftXidSlot
{
	TransactionId xid;			/* InvalidTransactionId: free */
	uint8		status;
} SiftXidSlot;

/* What a snapshot sees of a transaction: for the rows of a snapshot of an MVCC kind; any other snapshot (dirty, self, any) is not cached, see sift_vis_inserted */
typedef struct SiftXidCache
{
	Snapshot	snap;
	bool		is_mvcc;
	bool		has_own_xid;	/* the transaction has an xid: only then can an id be its own */
	TransactionId last_xid;		/* the id asked last: a run of rows of one transaction is one comparison */
	uint8		last_status;
	SiftXidSlot slots[SIFT_XID_CACHE_SIZE];
} SiftXidCache;

extern void sift_xid_cache_init(SiftXidCache *c, Snapshot snap);
extern SiftXidStatus sift_xid_cache_status(SiftXidCache *c, TransactionId xid);

/* What a settle or a compaction does with the transaction of a row, as of the horizon: nothing a snapshot may still tell from a frozen row, or what has to be kept */
typedef enum SiftHorizonStatus
{
	SIFT_H_EMPTY = 0,
	SIFT_H_COMMITTED_OLD,		/* committed, and before the horizon: no snapshot can tell it from a frozen row */
	SIFT_H_COMMITTED_NEW,		/* committed, not before the horizon */
	SIFT_H_RUNNING,
	SIFT_H_ABORTED
} SiftHorizonStatus;

typedef struct SiftXidHorizonCache
{
	FullTransactionId next;		/* the next transaction id, read once: the epoch of every id asked and the end of what a pass can meet */
	FullTransactionId horizon;
	SiftXidSlot slots[SIFT_XID_CACHE_SIZE];
} SiftXidHorizonCache;

extern void sift_xid_horizon_init(SiftXidHorizonCache *c, FullTransactionId horizon);
extern SiftHorizonStatus sift_xid_horizon_status(SiftXidHorizonCache *c, TransactionId xid);

/* The id is before the horizon (a normal id: the frozen one always is) */
static inline bool
sift_xid_horizon_older(const SiftXidHorizonCache *c, TransactionId xid)
{
	return FullTransactionIdPrecedes(sift_xid_fxid_at(c->next, xid), c->horizon);
}

#endif							/* SIFT_XID_CACHE_H */
