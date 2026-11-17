/*
 * col/vstore/vstore_gc.c - the epochs of a shared value store: a new epoch when many codes are no longer used, and the retirement of the old one when no segment names it
 *
 * The codes of a store never move inside an epoch, and a value that no segment names any more keeps its code. Compaction is lazy: a new epoch begins (the codes
 * start again at 0, nothing is copied), a segment that is written afterwards names the new epoch, and the strings it needs are appended to it when it is written
 * (the merge, the rewrite and the REINDEX of a member read the strings of the old epoch from its runs and write the new epoch's). When no segment of any member names
 * an old epoch its runs are marked retired, and removed once the transactions that could have read a segment naming it have ended (sift_vstore_retire).
 *
 * The locks: a writer holds the store in RowExclusiveLock to the end of its transaction, from its codes to its segment, so that an epoch is not retired under a
 * segment that is not yet published; this code holds it in ShareRowExclusiveLock (after the members, in the order the writers take them), which blocks new
 * writers while the segments of the members are read for the epochs they name. Every lock of a pass is asked for without waiting, as VACUUM does: a pass that finds one held
 * fails (sift.col_value_store_compact) or is skipped (VACUUM) and names what is locked.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "col/am.h"
#include "col/scope.h"
#include "col/vstore/vstore.h"
#include "core/catname.h"
#include "core/ops.h"
#include "core/restrict.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(sift_col_value_store_begin);
PG_FUNCTION_INFO_V1(sift_col_value_store_end);

/* The lock of a pass that is asked for, as VACUUM asks (try_lock_all): not waited for, since a request that queues blocks every statement behind it */
static void
lock_or_fail(Oid rel, LOCKMODE mode)
{
	if (!ConditionalLockRelationOid(rel, mode))
		ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						errmsg("could not lock %s for the shared value store: another session holds a lock that conflicts with it", sift_rel_label(rel)),
						errhint("Try again when that session ends.")));
}

/*
 * The store of the column of the table, the members and the store locked, in that order. The store is locked in ShareRowExclusiveLock because the pass reads the segments
 * of the members for the epochs they name: a writer between its codes and its segment (RowExclusiveLock on the store) would be missed
 */
static SiftVStore *
open_locked(Oid store)
{
	Oid			heap = sift_vstore_table(store);

	if (!sift_vstore_is_store(store) || !OidIsValid(heap))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("relation %s is not a shared value store", sift_rel_label(store))));
	if (!object_ownercheck(RelationRelationId, heap, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(heap));
	lock_or_fail(heap, ShareUpdateExclusiveLock);
	foreach_oid(index, sift_vstore_members(store))
		lock_or_fail(index, ShareUpdateExclusiveLock);
	lock_or_fail(store, ShareRowExclusiveLock);
	return sift_vstore_open(store, NoLock);
}

/*
 * sift._col_value_store_begin(table, column): a new epoch when codes of the current one are not in use, the store (NULL when the column has none). The locks
 * stay to the end of the transaction, which is the one that remaps the members (sift.col_value_store_compact) and retires the old epoch
 */
Datum
sift_col_value_store_begin(PG_FUNCTION_ARGS)
{
	Oid			heap = sift_rel_must_exist(PG_GETARG_OID(0));
	AttrNumber	attno = get_attnum(heap, NameStr(*PG_GETARG_NAME(1)));
	Oid			store;
	SiftVStore *vs;
	uint64		epoch,
				total,
				live;
	Bitmapset  *epochs;

	if (attno == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" of relation \"%s\" does not exist", NameStr(*PG_GETARG_NAME(1)), get_rel_name(heap))));
	store = sift_vstore_find_oid(heap, attno);
	if (!OidIsValid(store))
		PG_RETURN_NULL();
	vs = open_locked(store);
	sift_vstore_usage(store, &epoch, &total, &live, &epochs);
	if (live < total && !sift_scope_key_store_pinned(store))
		sift_vstore_bump_epoch(vs);
	sift_vstore_close(vs);
	PG_RETURN_OID(store);
}

/* sift._col_value_store_end(store): retires the epochs before the current one that no segment names; the runs removed (those marked wait for the horizon) */
Datum
sift_col_value_store_end(PG_FUNCTION_ARGS)
{
	Oid			store = PG_GETARG_OID(0);
	SiftVStore *vs = open_locked(store);
	uint64		epoch,
				total,
				live;
	Bitmapset  *epochs;
	int			removed;

	sift_vstore_usage(store, &epoch, &total, &live, &epochs);
	removed = sift_vstore_retire(vs, epochs);
	sift_vstore_close(vs);
	PG_RETURN_INT64(removed);
}

int			sift_vstore_gc_kb = 8192;	/* the size of a store (kB) from which VACUUM looks for garbage; sift.test_vstore_gc_kb in a testing build */

#define VSTORE_GARBAGE_PCT	50		/* the share of the codes of a store that no segment names from which VACUUM begins a new epoch */

/* The locks of a pass of VACUUM, taken only when no one holds one against them: false (and none kept) otherwise */
static bool
try_lock_all(Oid store, List *members, List **locked)
{
	*locked = NIL;
	foreach_oid(m, members)
	{
		if (!ConditionalLockRelationOid(m, ShareUpdateExclusiveLock))
			goto fail;
		*locked = lappend_oid(*locked, m);
	}
	if (!ConditionalLockRelationOid(store, ShareRowExclusiveLock))
		goto fail;
	return true;
fail:
	foreach_oid(m, *locked)
		UnlockRelationOid(m, ShareUpdateExclusiveLock);
	return false;
}

/*
 * VACUUM of a table with a sift_col index that uses a shared value store, once for each store (by the member with the lowest oid): an old epoch that no segment names
 * is retired, and a store of at least sift.test_vstore_gc_kb whose codes are mostly unused begins a new epoch, so that the segments that the maintenance of the members
 * writes name it (the index is then due for a rewrite, why "value store epoch")
 */
static void
vacuum_store(Relation index, Oid store)
{
	List	   *members = sift_vstore_members(store),
			   *locked;
	SiftVStore *vs;
	uint64		epoch,
				total,
				live;
	Bitmapset  *epochs;
	Relation	rel;
	int64		bytes;
	TimestampTz start;
	bool		worked = true;

	if (!try_lock_all(store, members, &locked))
		return;
	start = GetCurrentTimestamp();
	rel = table_open(store, NoLock);
	bytes = (int64) RelationGetNumberOfBlocks(rel) * BLCKSZ;
	table_close(rel, NoLock);
	vs = sift_vstore_open(store, NoLock);
	if (sift_vstore_old_epochs(vs))
	{
		sift_vstore_usage(store, &epoch, &total, &live, &epochs);
		(void) sift_vstore_retire(vs, epochs);
	}
	else if (bytes >= (int64) sift_vstore_gc_kb * 1024 && !sift_scope_key_store_pinned(store))
	{
		sift_vstore_usage(store, &epoch, &total, &live, &epochs);
		if (total > 0 && (total - live) * 100 >= total * VSTORE_GARBAGE_PCT)
		{
			ereport(NOTICE,
					(errmsg("shared value store \"%s\": %.0f%% of its %s codes are named by no index (%.1f MB): a new epoch begins",
							get_rel_name(store), 100.0 * (total - live) / total, psprintf(UINT64_FORMAT, total), bytes / 1048576.0),
					 errdetail("The segments that merges and rewrites write name the new epoch; the old one is removed when no segment names it."),
					 errhint("SELECT sift.col_value_store_compact(%s, %s) does it now.", quote_literal_cstr(get_rel_name(sift_vstore_table(store))),
							 quote_literal_cstr(get_attname(store, 1, false)))));
			sift_vstore_bump_epoch(vs);
		}
		else
			worked = false;
	}
	else
		worked = false;
	sift_vstore_close(vs);
	foreach_oid(m, locked)
		UnlockRelationOid(m, ShareUpdateExclusiveLock);
	UnlockRelationOid(store, ShareRowExclusiveLock);
	/* the writers of the members waited from the locks to here */
	if (worked)
		sift_log_maint(index, "value store compaction", start, -1, bytes);
}

/*
 * The leaves of the index that use a store nothing else shares (sift_vstore_leaf_left) and none of whose segments names it any more, because the merges and
 * rewrites have put the strings back: the index stops depending on the store, which goes with the transaction
 */
void
sift_vstore_release_left(Relation index)
{
	const SiftColSchema *schema = sift_col_rel(index)->schema;
	bool	   *left = sift_vstore_left_leaves(index, schema);
	Relation	heap;

	if (left == NULL)
		return;
	/* releasing a store deletes dependency rows (and drops the store with the transaction): not in a VACUUM, whose transaction id the other sessions do not see */
	if (!sift_catalog_write_ok())
	{
		sift_col_vstore_request(index);
		return;
	}
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	for (int c = 0; c < schema->ncols; c++)
	{
		Oid			store = left[c] ? sift_vstore_leaf_store(index, &schema->cols[c]) : InvalidOid;
		List	   *members,
				   *locked;
		uint64		epoch,
					total,
					live;
		Bitmapset  *epochs;

		if (!OidIsValid(store))
			continue;
		members = sift_vstore_members(store);
		if (list_length(members) != 1 || !try_lock_all(store, members, &locked))
			continue;
		sift_vstore_usage(store, &epoch, &total, &live, &epochs);
		if (bms_is_empty(epochs))
			sift_vstore_release(heap, index, sift_vstore_leaf_attno(index, &schema->cols[c]));
		foreach_oid(m, locked)
			UnlockRelationOid(m, ShareUpdateExclusiveLock);
		UnlockRelationOid(store, ShareRowExclusiveLock);
	}
	table_close(heap, NoLock);
}

/* The stores the leaves of the index use, each looked at by the member with the lowest oid: an epoch that no segment names is retired, a store of mostly unused codes begins a new one */
void
sift_vstore_compact(Relation index)
{
	Oid		   *stores = sift_vstore_leaf_stores(index, sift_col_rel(index)->schema);

	for (int c = 0; stores != NULL && c < sift_col_rel(index)->schema->ncols; c++)
	{
		Oid			low = RelationGetRelid(index);
		bool		again = false;

		if (!OidIsValid(stores[c]))
			continue;
		for (int d = 0; d < c; d++)
			again |= stores[d] == stores[c];
		foreach_oid(m, sift_vstore_members(stores[c]))
			low = Min(low, m);
		if (!again && low == RelationGetRelid(index))
			vacuum_store(index, stores[c]);
	}
}

/* VACUUM of the table of the index: the stores compacted, then the leaves left alone */
void
sift_vstore_vacuum(Relation index)
{
	sift_vstore_compact(index);
	sift_vstore_release_left(index);
}
