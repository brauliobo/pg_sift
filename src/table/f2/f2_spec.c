/*
 * table/f2/f2_spec.c - the state of a staged insert in the entry of its row (table/f2/f2_spec.h).
 */
#include "postgres.h"

#include "table/f2/f2_spec.h"

#define SPEC_BITS	(SIFT_ROWLOCK_SPECULATIVE | SIFT_ROWLOCK_STAGED_INSERT | SIFT_ROWLOCK_NEED_FLUSH)

void
sift_spec_stage(SiftRowLockEntry *e, FullTransactionId inserter, uint32 token)
{
	e->flags |= SIFT_ROWLOCK_STAGED_INSERT | (token != 0 ? SIFT_ROWLOCK_SPECULATIVE : 0);
	e->spec_token = token;
	e->updater_fxid = inserter;
}

bool
sift_spec_succeeded(SiftRowLockEntry *e)
{
	e->flags &= ~SIFT_ROWLOCK_SPECULATIVE;
	e->spec_token = 0;
	return (e->flags & SIFT_ROWLOCK_NEED_FLUSH) != 0;
}

void
sift_spec_flushed(SiftRowLockEntry *e)
{
	if ((e->flags & SIFT_ROWLOCK_STAGED_INSERT) != 0 && (e->flags & (SIFT_ROWLOCK_ENDING_FLAGS | SIFT_ROWLOCK_COMMITTED_UPD)) == 0)
		e->updater_fxid = InvalidFullTransactionId;
	e->flags &= ~(SIFT_ROWLOCK_STAGED_INSERT | SIFT_ROWLOCK_NEED_FLUSH);
}

void
sift_spec_aborted(SiftRowLockEntry *e)
{
	e->flags &= ~SPEC_BITS;
	e->spec_token = 0;
	e->updater_fxid = InvalidFullTransactionId;
}

SiftSpecProbe
sift_spec_probe(SiftRowLockEntry *e, bool inserter_running, TransactionId *xmin, uint32 *token)
{
	if ((e->flags & SIFT_ROWLOCK_STAGED_INSERT) == 0)
		return SIFT_SPEC_NONE;
	if (!inserter_running)
		return SIFT_SPEC_ENDED;
	e->flags |= SIFT_ROWLOCK_NEED_FLUSH;
	*xmin = XidFromFullTransactionId(e->updater_fxid);
	*token = (e->flags & SIFT_ROWLOCK_SPECULATIVE) != 0 ? e->spec_token : 0;
	return SIFT_SPEC_ACTIVE;
}

void
sift_spec_end(SiftRowLockEntry *e, TransactionId xid)
{
	if ((e->flags & SIFT_ROWLOCK_STAGED_INSERT) != 0 && XidFromFullTransactionId(e->updater_fxid) == xid)
		sift_spec_aborted(e);
}
