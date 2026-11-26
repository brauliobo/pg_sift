/*
 * core/shm/lease_test.c - test functions of the Block 0 leases and of the contention of the registries (core/shm/lease.h, manifest.h, core/maint/sched.h), compiled only with SIFT_TESTING:
 * the exclusive locks of Block 0 a backend took, and loops that a test runs in many sessions at once to time the shared-memory paths.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "core/maint/sched.h"
#include "core/shm/lease.h"
#include "core/shm/manifest.h"

PG_FUNCTION_INFO_V1(sift_test_lease_block0_x);
PG_FUNCTION_INFO_V1(sift_test_lease_maint);
PG_FUNCTION_INFO_V1(sift_test_lease_mix);
PG_FUNCTION_INFO_V1(sift_test_lease_stat);

uint64		sift_lease_block0_x_locks;

#define MIX_ENTRY_BYTES		32
#define MIX_PARTS			32
#define MIX_LIST_BYTES		256

/* sift._test_lease_block0_x(): the exclusive locks of Block 0 this backend has taken at the sites of a writer (a commit of a part, the counters, the allocator, the vblocks) */
Datum
sift_test_lease_block0_x(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) sift_lease_block0_x_locks);
}

/*
 * sift._test_lease_maint(rel oid, iters int8, what int4): the microseconds this backend takes for iters requests that a queued request of the relation already covers (what 0), or for iters
 * notes of a read of a relation of the heat table (what 1: the relations rel .. rel + 63)
 */
Datum
sift_test_lease_maint(PG_FUNCTION_ARGS)
{
	Oid			rel = PG_GETARG_OID(0);
	int64		iters = PG_GETARG_INT64(1);
	int32		what = PG_GETARG_INT32(2);
	SiftMaintReq req = {.db = MyDatabaseId,.rel = rel,.kind = SIFT_MK_COMPACT_PART,.tier = SIFT_MAINT_TIER_DEFAULT};
	SiftMaintShmem *m = sift_maint_table();
	TimestampTz start;

	if (m == NULL)
		elog(ERROR, "sift test: the maintenance queue is not there");
	(void) sift_maint_enqueue(m, &sift_maint_server_host, &req);
	start = GetCurrentTimestamp();
	for (int64 n = 0; n < iters; n++)
	{
		if (what == 0)
			(void) sift_maint_enqueue(m, &sift_maint_server_host, &req);
		else
			sift_maint_heat_note(m, rel + (Oid) (n & 63));
		if ((n & 1023) == 0)
			CHECK_FOR_INTERRUPTS();
	}
	PG_RETURN_INT64(GetCurrentTimestamp() - start);
}

static SiftShmIndexMeta
mix_meta(Oid rel, uint64 v)
{
	return (SiftShmIndexMeta) {.locator = {.spcOid = 1,.dbOid = 2,.relNumber = rel},.manifest_version = v,.scope_dir_gen = v,.npartitions = MIX_PARTS};
}

static void
mix_publish(Oid rel, uint64 v)
{
	SiftShmIndexMeta meta = mix_meta(rel, v);
	uint8		entries[MIX_PARTS * MIX_ENTRY_BYTES] = {0};
	uint8		scopes[2 * 16] = {0};
	SiftShmSnapSpec spec = {.manifest_version = v,.scope_dir_gen = v,.npartitions = MIX_PARTS,.nscopes = 2,.part_entries = entries,
		.part_entry_size = MIX_ENTRY_BYTES,.scopes = scopes,.scope_entry_size = 16};

	(void) sift_shm_manifest_publish(&meta, &spec);
}

/*
 * sift._test_lease_mix(rel oid, iters int8, write_every int4): the microseconds this backend takes for iters rounds of a reader of the registry (peek, pin the snapshot, read the list of one partition and
 * install it when there is none, let go), every write_every-th round a publication of a new version with a snapshot instead. The first round of the first session publishes version 1.
 */
Datum
sift_test_lease_mix(PG_FUNCTION_ARGS)
{
	Oid			rel = PG_GETARG_OID(0);
	int64		iters = PG_GETARG_INT64(1);
	int32		write_every = PG_GETARG_INT32(2);
	RelFileLocator loc = mix_meta(rel, 0).locator;
	char		list[MIX_LIST_BYTES] = {0};
	SiftShmIndexMeta peeked;
	TimestampTz start;

	if (!sift_shm_manifest_peek(&loc, &peeked))
		mix_publish(rel, 1);
	start = GetCurrentTimestamp();
	for (int64 n = 0; n < iters; n++)
	{
		if (write_every > 0 && n % write_every == write_every - 1 && sift_shm_manifest_peek(&loc, &peeked))
			mix_publish(rel, peeked.manifest_version + 1);
		else
		{
			const SiftShmManifestSnap *snap = sift_shm_snap_pin(&loc);

			if (snap != NULL)
			{
				uint32		p = (uint32) n % snap->npartitions;

				if (sift_shm_snap_part_list(snap, p) == NULL)
					(void) sift_shm_snap_part_list_install(snap, p, list, sizeof(list));
				sift_shm_snap_unpin(snap);
			}
		}
		if ((n & 255) == 0)
			CHECK_FOR_INTERRUPTS();
	}
	PG_RETURN_INT64(GetCurrentTimestamp() - start);
}

/* sift._test_lease_stat(what text): a counter of the registries (list_install_lost: the lists a backend made and found another had published first) */
Datum
sift_test_lease_stat(PG_FUNCTION_ARGS)
{
	char	   *what = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SiftShmManifestStats st;

	sift_shm_manifest_stats(&st);
	if (strcmp(what, "list_install_lost") == 0)
		PG_RETURN_INT64((int64) st.list_install_lost);
	elog(ERROR, "sift test: unknown statistic \"%s\"", what);
	PG_RETURN_NULL();
}

#endif
