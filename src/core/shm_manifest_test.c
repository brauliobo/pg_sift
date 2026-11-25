/*
 * core/shm_manifest_test.c - the test functions of the manifest registry (core/shm_manifest.h) and of the partition locks (core/part_lock.h), compiled only with SIFT_TESTING.
 * An index is named by the relation number alone (tablespace and database are fixed), and its meta by one number: every field is the version, so a copy that mixes two versions is torn.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/relation.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"

#include "core/catname.h"
#include "core/part_lock.h"
#include "core/shm_manifest.h"

PG_FUNCTION_INFO_V1(sift_test_shm_manifest_publish);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_peek);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_invalidate);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_served);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_read);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_overlap);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_stat);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_hammer);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_churn);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_read_stale);
PG_FUNCTION_INFO_V1(sift_test_shm_manifest_announce_epoch);
PG_FUNCTION_INFO_V1(sift_test_part_lock_stripe);
PG_FUNCTION_INFO_V1(sift_test_part_lock_try);
PG_FUNCTION_INFO_V1(sift_test_part_unlock);
PG_FUNCTION_INFO_V1(sift_test_part_lock_pair);
PG_FUNCTION_INFO_V1(sift_test_part_unlock_pair);
PG_FUNCTION_INFO_V1(sift_test_part_lock_blkno);

#define ENTRY_BYTES		32
#define SCOPE_BYTES		16

static RelFileLocator
locator_of(Oid rel)
{
	return (RelFileLocator) {.spcOid = 1, .dbOid = 2, .relNumber = rel};
}

static SiftShmIndexMeta
meta_of(Oid rel, uint64 v)
{
	return (SiftShmIndexMeta) {
		.locator = locator_of(rel), .manifest_version = v, .scope_dir_gen = v, .partdir_root_blk = (BlockNumber) v, .scope_dir_blk = (BlockNumber) v,
		.partdir_level = (uint16) v, .nscopes = (uint16) v, .npartitions = (uint32) v, .total_rows = v, .total_dead = v, .total_bytes = v,
		.oldest_active_xid = (TransactionId) v, .meta_flags = (uint32) v, .compat_flags = v, .ro_compat_flags = v, .inline_off = (uint16) v, .inline_len = (uint16) v
	};
}

static bool
meta_whole(const SiftShmIndexMeta *m)
{
	uint64		v = m->manifest_version;

	return m->scope_dir_gen == v && m->partdir_root_blk == (BlockNumber) v && m->scope_dir_blk == (BlockNumber) v && m->partdir_level == (uint16) v &&
		m->nscopes == (uint16) v && m->npartitions == (uint32) v && m->total_rows == v && m->total_dead == v && m->total_bytes == v &&
		m->oldest_active_xid == (TransactionId) v && m->meta_flags == (uint32) v && m->compat_flags == v && m->ro_compat_flags == v &&
		m->inline_off == (uint16) v && m->inline_len == (uint16) v;
}

static bool
publish_version(Oid rel, uint64 v, int32 nparts, bool with_snapshot)
{
	SiftShmIndexMeta meta = meta_of(rel, v);
	uint8	   *entries = palloc0(Max((Size) nparts * ENTRY_BYTES, 1));
	uint8		scopes[2 * SCOPE_BYTES];
	SiftShmSnapSpec spec = {
		.manifest_version = v, .scope_dir_gen = v, .npartitions = nparts, .nscopes = 2, .part_entries = entries,
		.part_entry_size = ENTRY_BYTES, .scopes = scopes, .scope_entry_size = SCOPE_BYTES
	};
	bool		ok;

	for (int p = 0; p < nparts; p++)
		memset(entries + (Size) p * ENTRY_BYTES, (uint8) (v + p), ENTRY_BYTES);
	memset(scopes, (uint8) v, sizeof(scopes));
	ok = sift_shm_manifest_publish(&meta, with_snapshot ? &spec : NULL);
	pfree(entries);
	return ok;
}

/* sift._test_shm_manifest_publish(rel oid, version int8, nparts int4, snapshot bool): the meta of the index, and a snapshot of nparts entries (entry p is filled with version + p) and 2 scopes; false where the snapshot was asked and not made */
Datum
sift_test_shm_manifest_publish(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(publish_version(PG_GETARG_OID(0), PG_GETARG_INT64(1), PG_GETARG_INT32(2), PG_GETARG_BOOL(3)));
}

/* sift._test_shm_manifest_peek(rel oid): the meta as a reader copies it, 'version flags whole', or NULL where the registry does not know the index */
Datum
sift_test_shm_manifest_peek(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));
	SiftShmIndexMeta meta;

	if (!sift_shm_manifest_peek(&loc, &meta))
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(psprintf(UINT64_FORMAT " %u %s", meta.manifest_version, meta.state_flags, meta_whole(&meta) ? "whole" : "torn")));
}

/* sift._test_shm_manifest_invalidate(rel oid, quarantine bool) */
Datum
sift_test_shm_manifest_invalidate(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));

	sift_shm_manifest_invalidate(&loc, PG_GETARG_BOOL(1));
	PG_RETURN_VOID();
}

/* sift._test_shm_manifest_served(index regclass): whether a reader is served the manifest of the file of the index (an index the registry does not hold, or holds out of service: false) */
Datum
sift_test_shm_manifest_served(PG_FUNCTION_ARGS)
{
	Relation	index = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	SiftShmIndexMeta meta;
	bool		served = sift_shm_manifest_peek(&index->rd_locator, &meta);

	relation_close(index, AccessShareLock);
	PG_RETURN_BOOL(served);
}

/* The snapshot holds what its version published: the entries, the scopes, the counts */
static bool
snap_whole(const SiftShmManifestSnap *snap)
{
	const uint8 *entries = sift_shm_snap_part_entries(snap),
			   *scopes = sift_shm_snap_scopes(snap);
	bool		ok = snap->nscopes == 2 && snap->scope_dir_gen == snap->manifest_version && scopes != NULL;

	for (uint32 p = 0; ok && p < snap->npartitions; p++)
		for (int b = 0; b < ENTRY_BYTES; b++)
			ok = ok && entries[(Size) p * ENTRY_BYTES + b] == (uint8) (snap->manifest_version + p);
	for (int b = 0; ok && b < 2 * SCOPE_BYTES; b++)
		ok = scopes[b] == (uint8) snap->manifest_version;
	return ok;
}

/*
 * sift._test_shm_manifest_read(rel oid, part int4, list_bytes int8): pins the snapshot of the index for the statement, checks what it holds and, where part is 0 or more, makes the list of that partition
 * (list_bytes) if it is not there. 'version whole made' (made: this call made the list), or NULL where the index has no snapshot. A pin is released at the end of the statement that took it.
 */
Datum
sift_test_shm_manifest_read(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));
	int32		part = PG_GETARG_INT32(1);
	const SiftShmManifestSnap *snap = sift_shm_snap_pin(&loc);
	bool		whole,
				made = false;
	uint64		version;

	if (snap == NULL)
		PG_RETURN_NULL();
	version = snap->manifest_version;
	whole = snap_whole(snap);
	if (part >= 0)
	{
		Size		bytes = PG_GETARG_INT64(2);
		char	   *list = palloc0(bytes);

		made = sift_shm_snap_part_list(snap, part) == NULL && sift_shm_snap_part_list_install(snap, part, list, bytes);
		pfree(list);
	}
	sift_shm_snap_unpin(snap);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf(UINT64_FORMAT " %s %s", version, whole ? "whole" : "damaged", made ? "made" : "not made")));
}

/*
 * sift._test_shm_manifest_read_stale(found oid, expected oid): a reader that looked up the slot of `found` and pins it for `expected`, as one does whose slot was given to another index in between:
 * 'version' of the snapshot it got, or NULL where it got none (it must not get the snapshot of another index)
 */
Datum
sift_test_shm_manifest_read_stale(PG_FUNCTION_ARGS)
{
	RelFileLocator found = locator_of(PG_GETARG_OID(0)),
				expected = locator_of(PG_GETARG_OID(1));
	const SiftShmManifestSnap *snap = sift_shm_snap_pin_found_for(&found, &expected);
	uint64		version;

	if (snap == NULL)
		PG_RETURN_NULL();
	version = snap->manifest_version;
	sift_shm_snap_unpin(snap);
	PG_RETURN_INT64((int64) version);
}

/* sift._test_shm_manifest_announce_epoch(): this backend announces an epoch as one inside a pin does and keeps it (the end of the session must take it back) */
Datum
sift_test_shm_manifest_announce_epoch(PG_FUNCTION_ARGS)
{
	sift_shm_manifest_announce_epoch();
	PG_RETURN_VOID();
}

/*
 * sift._test_shm_manifest_overlap(rel oid, version int8, nparts int4): a reader pins the snapshot, a commit publishes the next version with its snapshot, the reader's is still whole, and
 * the snapshots alive are counted before the reader lets go and after: 'old version, whole, alive with the pin, alive after'
 */
Datum
sift_test_shm_manifest_overlap(PG_FUNCTION_ARGS)
{
	Oid			rel = PG_GETARG_OID(0);
	RelFileLocator loc = locator_of(rel);
	const SiftShmManifestSnap *snap = sift_shm_snap_pin(&loc);
	SiftShmManifestStats during,
				after;
	uint64		old;
	bool		whole;

	if (snap == NULL)
		PG_RETURN_NULL();
	old = snap->manifest_version;
	publish_version(rel, PG_GETARG_INT64(1), PG_GETARG_INT32(2), true);
	whole = snap_whole(snap);
	sift_shm_manifest_stats(&during);
	sift_shm_snap_unpin(snap);
	sift_shm_manifest_stats(&after);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf(UINT64_FORMAT " %s %u %u", old, whole ? "whole" : "damaged", during.snaps_live, after.snaps_live)));
}

/* sift._test_shm_manifest_stat(what text): slots_valid, snaps_live, snap_bytes, area_users, area_bytes, evictions or epochs_active */
Datum
sift_test_shm_manifest_stat(PG_FUNCTION_ARGS)
{
	char	   *what = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SiftShmManifestStats st;

	sift_shm_manifest_stats(&st);
	if (strcmp(what, "slots_valid") == 0)
		PG_RETURN_INT64(st.slots_valid);
	if (strcmp(what, "snaps_live") == 0)
		PG_RETURN_INT64(st.snaps_live);
	if (strcmp(what, "snap_bytes") == 0)
		PG_RETURN_INT64((int64) st.snap_bytes);
	if (strcmp(what, "area_users") == 0)
		PG_RETURN_INT64(st.area_users);
	if (strcmp(what, "area_bytes") == 0)
		PG_RETURN_INT64((int64) st.area_bytes);
	if (strcmp(what, "evictions") == 0)
		PG_RETURN_INT64((int64) st.evictions);
	if (strcmp(what, "epochs_active") == 0)
		PG_RETURN_INT64(st.epochs_active);
	elog(ERROR, "sift test: unknown statistic \"%s\"", what);
	PG_RETURN_NULL();
}

/*
 * sift._test_shm_manifest_hammer(rel oid, iters int8, writer bool): a writer publishes the versions 1 to iters of the meta of the index, a reader peeks iters times; returns the copies
 * that were torn (mixed two versions) or, for a reader, went backwards. Run in two sessions at once.
 */
Datum
sift_test_shm_manifest_hammer(PG_FUNCTION_ARGS)
{
	Oid			rel = PG_GETARG_OID(0);
	int64		iters = PG_GETARG_INT64(1);
	RelFileLocator loc = locator_of(rel);
	int64		bad = 0;
	uint64		last = 0;

	for (int64 n = 0; n < iters; n++)
	{
		int64		i = n + 1;

		if (PG_GETARG_BOOL(2))
		{
			SiftShmIndexMeta meta = meta_of(rel, i);

			sift_shm_manifest_publish(&meta, NULL);
		}
		else
		{
			SiftShmIndexMeta meta;

			if (sift_shm_manifest_peek(&loc, &meta))
			{
				bad += !meta_whole(&meta) || meta.manifest_version < last;
				last = meta.manifest_version;
			}
		}
		CHECK_FOR_INTERRUPTS();
	}
	PG_RETURN_INT64(bad);
}

/*
 * sift._test_shm_manifest_churn(rel oid, iters int8, writer bool): a writer publishes iters versions of the index, each with a snapshot of 8 partitions, a reader pins the current snapshot iters times
 * and checks it is whole. Returns the damaged snapshots a reader found. Run in several sessions at once: the snapshots are freed by whoever lets go of one last.
 */
Datum
sift_test_shm_manifest_churn(PG_FUNCTION_ARGS)
{
	Oid			rel = PG_GETARG_OID(0);
	int64		iters = PG_GETARG_INT64(1);
	RelFileLocator loc = locator_of(rel);
	int64		bad = 0;

	for (int64 n = 0; n < iters; n++)
	{
		if (PG_GETARG_BOOL(2))
			publish_version(rel, n + 1, 8, true);
		else
		{
			const SiftShmManifestSnap *snap = sift_shm_snap_pin(&loc);

			if (snap != NULL)
			{
				bad += !snap_whole(snap);
				sift_shm_snap_unpin(snap);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}
	PG_RETURN_INT64(bad);
}

/* sift._test_part_lock_stripe(rel oid, part int4): the stripe of the partition */
Datum
sift_test_part_lock_stripe(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));

	PG_RETURN_INT32((int32) sift_part_lock_stripe(&loc, PG_GETARG_INT32(1)));
}

/* sift._test_part_lock_try(rel oid, part int4, exclusive bool): takes the stripe of the partition if it can, and holds it to sift._test_part_unlock */
Datum
sift_test_part_lock_try(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));

	PG_RETURN_BOOL(sift_part_lock_conditional(&loc, PG_GETARG_INT32(1), PG_GETARG_BOOL(2) ? LW_EXCLUSIVE : LW_SHARED));
}

/* sift._test_part_unlock(rel oid, part int4) */
Datum
sift_test_part_unlock(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));

	sift_part_unlock(&loc, PG_GETARG_INT32(1));
	PG_RETURN_VOID();
}

/* sift._test_part_lock_pair(rel oid, a int4, b int4) and sift._test_part_unlock_pair */
Datum
sift_test_part_lock_pair(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));

	sift_part_lock_pair(&loc, PG_GETARG_INT32(1), PG_GETARG_INT32(2));
	PG_RETURN_VOID();
}

Datum
sift_test_part_unlock_pair(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));

	sift_part_unlock_pair(&loc, PG_GETARG_INT32(1), PG_GETARG_INT32(2));
	PG_RETURN_VOID();
}

/* sift._test_part_lock_blkno(part int4): the block of the page lock of the partition */
Datum
sift_test_part_lock_blkno(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) SIFT_PART_LOCK_BLKNO((uint32) PG_GETARG_INT32(0)));
}

#endif							/* SIFT_TESTING */
