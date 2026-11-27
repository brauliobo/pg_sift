/*
 * test/unit/table/f2/fetch_subxip.c - the snapshot key of a fetch (table/f2/fetch.c, snap_key_set and snap_same): a snapshot whose subtransactions overflowed keeps a count of them
 * but no array (CopySnapshot), so the key reads no subxid of it and tells it apart from a snapshot that lists them.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "table/f2/fetch.c"

MemoryContext TopTransactionContext = (MemoryContext) 1;

FullTransactionId
GetTopFullTransactionIdIfAny(void)
{
	return InvalidFullTransactionId;
}

SubTransactionId
GetCurrentSubTransactionId(void)
{
	return 66;
}

int
main(void)
{
	TransactionId xip[] = {700, 701},
				subxip[] = {800, 801, 802};
	SnapshotData over = {.snapshot_type = SNAPSHOT_MVCC,.xmin = 700,.xmax = 900,.xip = xip,.xcnt = 2,.subxcnt = 65,.suboverflowed = true},
				listed = {.snapshot_type = SNAPSHOT_MVCC,.xmin = 700,.xmax = 900,.xip = xip,.xcnt = 2,.subxip = subxip,.subxcnt = 3};
	SnapKey		k = {0};

	snap_key_set(&k, &over, CurrentMemoryContext);
	SIFT_CHECKF(snap_same(&k, &over), "an overflowed snapshot matches its own key");
	SIFT_CHECKF(!snap_same(&k, &listed), "a snapshot listing its subxids does not match an overflowed key");
	snap_key_set(&k, &listed, CurrentMemoryContext);
	SIFT_CHECKF(snap_same(&k, &listed), "a listed snapshot matches its own key");
	SIFT_CHECKF(!snap_same(&k, &over), "an overflowed snapshot does not match a listed key");
	subxip[2] = 803;
	SIFT_CHECKF(!snap_same(&k, &listed), "another subxid misses the key");
	return sift_check_summary();
}
