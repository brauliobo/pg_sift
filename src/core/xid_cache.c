/*
 * core/xid_cache.c - the cache of what the transaction log says of a transaction id (core/xid_cache.h).
 */
#include "core/host.h"

#include "access/xact.h"
#include "storage/procarray.h"
#include "utils/snapmgr.h"

#include "core/xid_cache.h"

/* The slot of an id: the one that holds it, else the first free among the probes of its hash, else the first of them (which the id replaces) */
static SiftXidSlot *
slot_of(SiftXidSlot *slots, TransactionId xid)
{
	uint32		h = (xid * UINT32_C(2654435761)) >> 26;

	for (uint32 p = 0; p < SIFT_XID_CACHE_PROBES; p++)
	{
		SiftXidSlot *s = &slots[(h + p) % SIFT_XID_CACHE_SIZE];

		if (s->xid == xid || s->xid == InvalidTransactionId)
			return s;
	}
	return &slots[h];
}

/* MEM[MEM-CORE-09]: stack/scan embedded SiftXidCache (544 B per scan; no heap alloc) */
void
sift_xid_cache_init(SiftXidCache *c, Snapshot snap)
{
	memset(c, 0, sizeof(*c));
	c->snap = snap;
	c->is_mvcc = snap != NULL && snap->snapshot_type == SNAPSHOT_MVCC;
	c->has_own_xid = FullTransactionIdIsValid(GetTopFullTransactionIdIfAny());
}

static SiftXidStatus
resolve(const SiftXidCache *c, TransactionId xid)
{
	if (c->has_own_xid && TransactionIdIsCurrentTransactionId(xid))
		return SIFT_XSTAT_SELF;
	if (XidInMVCCSnapshot(xid, c->snap))
		return SIFT_XSTAT_NOT_SEEN;
	return TransactionIdDidCommit(xid) ? SIFT_XSTAT_COMMITTED : SIFT_XSTAT_ABORTED;
}

SiftXidStatus
sift_xid_cache_status(SiftXidCache *c, TransactionId xid)
{
	SiftXidSlot *s;

	Assert(c->is_mvcc && TransactionIdIsNormal(xid));
	if (xid == c->last_xid)
		return (SiftXidStatus) c->last_status;
	s = slot_of(c->slots, xid);
	if (s->xid != xid)
		*s = (SiftXidSlot) {xid, resolve(c, xid)};
	c->last_xid = xid;
	c->last_status = s->status;
	return (SiftXidStatus) s->status;
}

void
sift_xid_horizon_init(SiftXidHorizonCache *c, FullTransactionId horizon)
{
	memset(c, 0, sizeof(*c));
	c->next = ReadNextFullTransactionId();
	c->horizon = horizon;
}

SiftHorizonStatus
sift_xid_horizon_status(SiftXidHorizonCache *c, TransactionId xid)
{
	SiftXidSlot *s;

	Assert(TransactionIdIsNormal(xid));
	s = slot_of(c->slots, xid);
	if (s->xid != xid)
	{
		SiftHorizonStatus st;

		/* in progress first: a transaction that commits between the two questions is asked committed, never aborted */
		if (TransactionIdIsInProgress(xid))
			st = SIFT_H_RUNNING;
		else if (TransactionIdDidCommit(xid))
			st = sift_xid_horizon_older(c, xid) ? SIFT_H_COMMITTED_OLD : SIFT_H_COMMITTED_NEW;
		else
			st = SIFT_H_ABORTED;
		*s = (SiftXidSlot) {xid, st};
	}
	return (SiftHorizonStatus) s->status;
}
