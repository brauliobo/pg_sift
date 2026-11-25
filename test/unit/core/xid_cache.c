/*
 * test/unit/core/xid_cache.c - the transaction ids of core/xid_cache.h that a pass meets after it read the next id: one that began since is of the epoch of next (or of the one after, once the
 * ids wrapped), and the frozen one is committed before every horizon.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "core/xid_cache.c"

static void
check_fxid(uint32 epoch, TransactionId next_xid, TransactionId xid, uint32 want_epoch, const char *what)
{
	FullTransactionId f = sift_xid_fxid_at(FullTransactionIdFromEpochAndXid(epoch, next_xid), xid);

	SIFT_CHECKF(EpochFromFullTransactionId(f) == want_epoch && XidFromFullTransactionId(f) == xid, "%s: epoch %u xid %u, wanted epoch %u", what, EpochFromFullTransactionId(f),
		  XidFromFullTransactionId(f), want_epoch);
}

static void
test_ids_past_next(void)
{
	check_fxid(1, 1000, 900, 1, "an id before next");
	check_fxid(1, 1000, 1000, 1, "the id of next");
	check_fxid(1, 1000, 1500, 1, "an id that began after next was read");
	check_fxid(1, 1000, 4000000000U, 0, "an id of the epoch before");
	check_fxid(1, 4294967290U, 20, 2, "an id past next after the ids wrapped");
}

static void
test_frozen(void)
{
	SiftXidHorizonCache c;

	memset(&c, 0, sizeof(c));
	c.next = FullTransactionIdFromEpochAndXid(1, 1000);
	c.horizon = FullTransactionIdFromEpochAndXid(1, 900);
	SIFT_CHECKF(sift_xid_horizon_status(&c, FrozenTransactionId) == SIFT_H_COMMITTED_OLD, "the frozen id is committed before the horizon");
	SIFT_CHECKF(!sift_xid_horizon_older(&c, 1500), "an id past next is not before the horizon");
	SIFT_CHECKF(sift_xid_horizon_older(&c, FrozenTransactionId), "the frozen id is before the horizon");
}

int
main(void)
{
	test_ids_past_next();
	test_frozen();
	return sift_check_summary();
}
