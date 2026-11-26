/*
 * test/unit/core/shm/manifest.c - the registry of the manifests of core/shm/manifest.c and manifest_pub.c over an area of the heap: the readers that load one version of a manifest at once make
 * one snapshot of it, and a second publication of that version neither makes nor replaces one; a retired snapshot costs its retire O(1) however many a reader's epoch holds back, and the ones a
 * reader holds do not keep the newer ones from being freed; a database that is dropped gives back the slots and the snapshots of its indexes.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "core/shm/manifest.c"
#include "core/shm/manifest_pub.c"

#include "test/unit/core/shm/stub_lwlock.h"

int			sift_shared_cache_kb = 1024;
char	   *sift_test_dsm_fail;
volatile uint32 InterruptHoldoffCount;
ErrorContextCallback *error_context_stack;
sigjmp_buf *PG_exception_stack;

static uint64 allocs;
static uint64 addresses;

void *
sift_dsm_try(const char *name, Size size, SiftDsmInit init)
{
	return stub_segment(size, init);
}

void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
}

bool
RecoveryInProgress(void)
{
	return false;
}

MemoryContext
sift_cache_create_context(const char *name)
{
	return CurrentMemoryContext;
}

bool
sift_shm_reserve_shared(SiftShmPoolId pool, Size bytes)
{
	return true;
}

void
sift_shm_release_shared(SiftShmPoolId pool, Size bytes)
{
}

void
sift_shm_note_eviction(SiftShmPoolId pool, uint64 count)
{
}

void
sift_lease_invalidate(const RelFileLocator *loc)
{
}

void
sift_lease_invalidate_db(Oid db)
{
}

/* The area is the heap: a dsa_pointer is the address */
dsa_area *
dsa_create_ext(int tranche_id, size_t init_segment_size, size_t max_segment_size)
{
	return (dsa_area *) &allocs;
}

dsa_handle
dsa_get_handle(dsa_area *a)
{
	return 1;
}

dsa_area *
dsa_attach(dsa_handle handle)
{
	return (dsa_area *) &allocs;
}

void
dsa_pin_mapping(dsa_area *a)
{
}

dsa_pointer
dsa_allocate_extended(dsa_area *a, size_t size, int flags)
{
	allocs++;
	return (dsa_pointer) (uintptr_t) calloc(1, size);
}

void
dsa_free(dsa_area *a, dsa_pointer dp)
{
	free((void *) (uintptr_t) dp);
}

void *
dsa_get_address(dsa_area *a, dsa_pointer dp)
{
	addresses++;
	return (void *) (uintptr_t) dp;
}

static const uint64 entries[3][4];

static SiftShmIndexMeta
meta_of(Oid db, RelFileNumber rel, uint64 version)
{
	return (SiftShmIndexMeta) {.locator = {.spcOid = 1663,.dbOid = db,.relNumber = rel},.state_flags = SIFT_SHM_SLOT_VALID,.manifest_version = version};
}

static SiftShmSnapSpec
spec_of(uint64 version)
{
	return (SiftShmSnapSpec) {.manifest_version = version,.npartitions = 3,.part_entries = entries,.part_entry_size = sizeof(entries[0])};
}

static dsa_pointer
snap_of(const RelFileLocator *loc)
{
	SiftShmIndexSlot *s = slot_probe(table, loc);

	return s == NULL ? InvalidDsaPointer : pg_atomic_read_u64(&s->current_snap_dp);
}

/* N readers that missed version 7 each publish the snapshot they read: the first makes it, the others neither make one nor replace it */
static void
test_one_snapshot_per_version(void)
{
	SiftShmIndexMeta m = meta_of(5, 20000, 7);
	SiftShmSnapSpec spec = spec_of(7);
	dsa_pointer first;
	uint64		before;

	SIFT_CHECK(sift_shm_manifest_publish(&m, NULL));
	before = allocs;
	SIFT_CHECK(sift_shm_manifest_publish(&m, &spec));
	first = snap_of(&m.locator);
	for (int reader = 1; reader < 32; reader++)
		(void) sift_shm_manifest_publish(&m, &spec);
	SIFT_CHECKF(allocs - before == 1, "32 readers of one version made %llu snapshots", (unsigned long long) (allocs - before));
	SIFT_CHECKF(snap_of(&m.locator) == first, "a later reader of the same version replaced the snapshot that was published");
}

/* A writer that misses the single-flight claim (another builder of the version took it, then the snapshot was published) is refused by the slot */
static void
test_slot_refuses_same_version(void)
{
	SiftShmIndexMeta m = meta_of(5, 20001, 3);
	SiftShmSnapSpec spec = spec_of(3);
	dsa_pointer first;
	SiftShmIndexSlot *s;

	SIFT_CHECK(sift_shm_manifest_publish(&m, NULL));
	SIFT_CHECK(sift_shm_manifest_publish(&m, &spec));
	first = snap_of(&m.locator);
	s = slot_probe(table, &m.locator);
	pg_atomic_write_u64(&s->building, 0);
	(void) sift_shm_manifest_publish(&m, &spec);
	SIFT_CHECKF(snap_of(&m.locator) == first, "a snapshot of the version the slot already holds replaced it");
}

static dsa_pointer
fresh_snap(void)
{
	RelFileLocator loc = {.spcOid = 1663,.dbOid = 9,.relNumber = 1};
	SiftShmSnapSpec spec = spec_of(1);

	return sift_shm_snap_build(table, area, &loc, &spec);
}

enum { RETIRED = 2000 };

static void
test_retire_is_fifo(void)
{
	dsa_pointer held;
	uint32		live;
	uint64		before;

	sift_shm_manifest_announce_epoch();
	held = fresh_snap();
	pg_atomic_fetch_add_u32(&((SiftShmManifestSnap *) dsa_get_address(area, held))->refcount, 1);
	sift_shm_snap_retire(table, area, held);
	live = pg_atomic_read_u32(&table->snaps_live);
	before = addresses;
	for (int i = 0; i < RETIRED; i++)
		sift_shm_snap_retire(table, area, fresh_snap());
	SIFT_CHECKF(addresses - before < 8 * RETIRED, "%d retires behind an epoch read %llu snapshots", RETIRED, (unsigned long long) (addresses - before));
	SIFT_CHECKF(pg_atomic_read_u32(&table->snaps_live) == live + RETIRED, "a reader's epoch holds the retired snapshots back");
	pg_atomic_write_u64(&table->epoch[MyProcNumber].epoch, 0);
	sift_shm_snap_retire(table, area, fresh_snap());
	SIFT_CHECKF(pg_atomic_read_u32(&table->snaps_live) == live, "once the epoch is left the retired snapshots nobody holds are freed (%u live, %u expected)",
				pg_atomic_read_u32(&table->snaps_live), live);
	snap_decref(area, held);
	SIFT_CHECKF(pg_atomic_read_u32(&table->snaps_live) == live - 1, "the held snapshot goes with its last reader");
	SIFT_CHECKF(table->retired_head == InvalidDsaPointer && table->retired_tail == InvalidDsaPointer, "the retired list is empty");
}

static void
test_invalidate_db(void)
{
	SiftShmIndexMeta a = meta_of(11, 30000, 2),
				b = meta_of(11, 30001, 2),
				c = meta_of(12, 30000, 2);
	SiftShmSnapSpec spec = spec_of(2);
	SiftShmIndexMeta out;
	uint32		live;

	live = pg_atomic_read_u32(&table->snaps_live);
	for (int i = 0; i < 3; i++)
	{
		SiftShmIndexMeta *m = i == 0 ? &a : i == 1 ? &b : &c;

		SIFT_CHECK(sift_shm_manifest_publish(m, NULL) && sift_shm_manifest_publish(m, &spec));
	}
	SIFT_CHECK(pg_atomic_read_u32(&table->snaps_live) == live + 3);
	sift_shm_manifest_invalidate_db(11);
	SIFT_CHECKF(!sift_shm_manifest_peek(&a.locator, &out) && !sift_shm_manifest_peek(&b.locator, &out), "the indexes of a dropped database have no slot");
	SIFT_CHECKF(sift_shm_manifest_peek(&c.locator, &out), "an index of another database keeps its slot");
	SIFT_CHECKF(pg_atomic_read_u32(&table->snaps_live) == live + 1, "the snapshots of the dropped database are freed (%u live)", pg_atomic_read_u32(&table->snaps_live) - live);
}

int
main(void)
{
	SIFT_CHECK(sift_shm_registry() != NULL && sift_shm_area_attach(table, true) != NULL);
	test_one_snapshot_per_version();
	test_slot_refuses_same_version();
	test_retire_is_fifo();
	test_invalidate_db();
	return sift_check_summary();
}
