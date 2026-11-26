/*
 * col/f2/l0/stage_test.c - the test-only SQL functions of the stage: the deletes it holds (sift_test_f2_stage_spec_abort), the partitions its writers made hot (sift_test_f2_l0_hot_dirs) and
 * the entries of the hot partitions a backend that exits holds (sift_test_f2_l0_hot_hold_fatal, sift_test_f2_l0_hot_holders), the stages the end of a subtransaction read (sift_test_f2_stage_sub_scans)
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/relation.h"

#include "col/f2/l0/shard_dir.h"
#include "col/f2/test_int.h"

PG_FUNCTION_INFO_V1(sift_test_f2_stage_spec_abort);
PG_FUNCTION_INFO_V1(sift_test_f2_l0_hot_dirs);
PG_FUNCTION_INFO_V1(sift_test_f2_l0_hot_hold_fatal);
PG_FUNCTION_INFO_V1(sift_test_f2_l0_hot_holders);
PG_FUNCTION_INFO_V1(sift_test_f2_stage_sub_scans);

/*
 * (index, n, tid_base): the stage withdraws the speculative insertions of n rows one after the other, and writes what it holds. The result is how many withdrawals left the xmax of a
 * withdrawn insertion (FrozenTransactionId) where the element before the array of deletes would have it: what one that is written at the moment the array is full does when it names the
 * element it just added by the count the write emptied. The addresses are those of no part, so the write drops them.
 */
Datum
sift_test_f2_stage_spec_abort(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	SiftPartSpec spec = f2t_spec(false);
	SiftTxnStage *st = sift_stage_get(rel, &spec);
	int			n = PG_GETARG_INT32(1);
	int64		tid_base = PG_GETARG_INT64(2);
	int			changed = 0;

	for (int i = 0; i < n; i++)
	{
		uint8		key[SIFT_BOUND_BYTES];
		const SiftStageDel *array = st->dels;
		TransactionId before = array != NULL ? array[-1].xmax : InvalidTransactionId;

		f2t_key(i, key);
		sift_stage_delete_spec_abort(st, key, f2t_tid(tid_base, i));
		changed += array != NULL && array == st->dels && before != FrozenTransactionId && array[-1].xmax == FrozenTransactionId;
	}
	sift_stage_flush(rel);
	index_close(rel, NoLock);
	PG_RETURN_INT32(changed);
}

/* (relation): the partitions of a sift_col index or a sift_table whose micro-slab is a directory of shards, which writers that contend for the lock of the partition promote it to */
Datum
sift_test_f2_l0_hot_dirs(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	int			n = 0;

	for (int i = 0; i < mr.n; i++)
		n += mr.entries[i].l0_slab_blk != InvalidBlockNumber && sift_l0_slab_is_dir(rel, mr.entries[i].part_id, mr.entries[i].l0_slab_blk);
	relation_close(rel, AccessShareLock);
	PG_RETURN_INT32(n);
}

static uint64
first_part(Relation rel)
{
	return sift_manifest_read_range(rel, NULL, NULL).entries[0].part_id;
}

/* (relation): the backend holds the entry of the hot partitions of the first partition and exits with FATAL */
Datum
sift_test_f2_l0_hot_hold_fatal(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);

	sift_l0_hot_test_hold_fatal(rel, first_part(rel));
	PG_RETURN_VOID();
}

/* (relation): the other backends that hold the entry of the hot partitions of the first partition */
Datum
sift_test_f2_l0_hot_holders(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	uint32		n = sift_l0_hot_test_holders(rel, first_part(rel));

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT32((int32) n);
}

/* (): the stages whose rows and deletes the end of a subtransaction read, since the backend started */
Datum
sift_test_f2_stage_sub_scans(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) sift_stage_sub_scans);
}

#endif							/* SIFT_TESTING */
