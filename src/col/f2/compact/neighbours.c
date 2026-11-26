/*
 * col/f2/compact/neighbours.c - the merge of neighbouring partitions that compaction and VACUUM left small (compact.h).
 */
#include "core/host.h"

#include "commands/vacuum.h"
#include "storage/lmgr.h"

#include "col/f2/compact/compact.h"
#include "core/compat.h"

static bool
partition_idle(const SiftPartitionEntry *e)
{
	return e->l0_slab_blk == InvalidBlockNumber && e->pend_del_blk == InvalidBlockNumber && (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0;
}

/* The entry of a partition as it is now, in place of the one a read of the manifest gave: only the fields a flush changes differ, the partitions themselves cannot move under the lock of the splits */
static void
refresh_entry(Relation rel, SiftPartitionEntry *e)
{
	if (!sift_manifest_find_entry(rel, e->part_id, e->key_lo, e))
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) e->part_id)));
}

/*
 * A transaction that holds a partition for its rows (table/f2/lock/esc.h) holds the split lock shared: the merge yields to it and comes again with the next pass. The manifest is read
 * once, under the lock that no split or merge passes: the neighbours are walked in one pass, the pair at the cursor judged by its entries as they are now, and a merged partition is judged
 * again against the one after it (the one before it was judged against its smaller self and did not join, so it does not join the larger).
 */
int
sift_compact_merge_neighbours(Relation rel)
{
	int			merged = 0;

	if (!ConditionalLockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock))
		return 0;
	{
		SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
		int		   *next = palloc(Max(mr.n, 1) * sizeof(int));
		int			i = 0;

		for (int k = 0; k < mr.n; k++)
			next[k] = k + 1 < mr.n ? k + 1 : -1;
		while (i >= 0 && next[i] >= 0)
		{
			SiftPartitionEntry *a = &mr.entries[i],
					   *b = &mr.entries[next[i]];

			sift_vacuum_delay_point();
			refresh_entry(rel, a);
			refresh_entry(rel, b);
			if (partition_idle(a) && partition_idle(b) && a->total_bytes + b->total_bytes < SIFT_PARTITION_MERGE_BYTES)
			{
				sift_manifest_merge(rel, a->part_id, a->key_lo, b->part_id, b->key_lo);
				merged++;
				a->total_bytes += b->total_bytes;
				memcpy(a->key_hi, b->key_hi, SIFT_BOUND_BYTES);
				next[i] = next[next[i]];
			}
			else
				i = next[i];
		}
		pfree(next);
	}
	UnlockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
	return merged;
}
