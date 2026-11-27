/*
 * test/unit/core/shm/manifest.c - the registry of the manifests of core/shm/manifest.c and manifest_pub.c over an area of the heap: the readers that load one version of a manifest at once make
 * one snapshot of it, and a second publication of that version neither makes nor replaces one; a retired snapshot costs its retire O(1) however many a reader's epoch holds back, and the ones a
 * reader holds do not keep the newer ones from being freed; a database that is dropped gives back the slots and the snapshots of its indexes. A writer that holds only the build_lock of a slot
 * and commits a newer version between the claim of a slot and its write is not undone; a meta of the version a slot has the snapshot of keeps that snapshot; a builder, a reader beyond the
 * epoch slots and a backend that installs a list give back what they hold when they fail (ERROR) or exit (FATAL); a builder whose area has no room for the snapshot, or that exits while it
 * builds it, gives back the claim of the build, so that the next reader of the version makes it. A writer that has not attached the area takes no snapshot out of a slot: another backend that
 * publishes one between its claim and its write, and leaves, does not tear the area down under its retire.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include <sys/mman.h>

#include "test/common/bench.h"
#include "test/common/pgshim.h"
#include "port/atomics.h"

/* What another backend does right after a count of this one reaches 0, before this one reads anything more */
static void (*on_zero) (void);

static uint32
sub_fetch_then(volatile pg_atomic_uint32 *ptr, int32 sub)
{
	uint32		left = (pg_atomic_sub_fetch_u32) (ptr, sub);
	void		(*hook) (void) = on_zero;

	if (left == 0 && hook != NULL)
	{
		on_zero = NULL;
		hook();
	}
	return left;
}

#define pg_atomic_sub_fetch_u32(ptr, sub) sub_fetch_then((ptr), (sub))

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
static int64 reserved;
static bool reserve_fails;
static pg_on_exit_callback exit_cb[4];
static int	nexit;
static bool exit_in_alloc;
static bool alloc_fails;
static sigjmp_buf exit_jmp;

/* What errfinish does in the server: the error goes to the innermost PG_TRY */
void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

static void
throw_error(void)
{
	pg_re_throw();
}

/* Whether fn raised an ERROR, caught as the caller of the code under test would catch it */
static bool
raises(void (*fn) (void *), void *arg)
{
	sigjmp_buf	jb;
	sigjmp_buf *outer = PG_exception_stack;

	if (sigsetjmp(jb, 0) != 0)
	{
		PG_exception_stack = outer;
		return true;
	}
	PG_exception_stack = &jb;
	fn(arg);
	PG_exception_stack = outer;
	return false;
}

/* What proc_exit does on FATAL: the exit callbacks run, last registered first, and no PG_CATCH does */
static void
exit_now(void)
{
	exit_in_alloc = false;
	while (nexit > 0)
		exit_cb[--nexit] (1, (Datum) 0);
	siglongjmp(exit_jmp, 1);
}

void *
sift_dsm_try(const char *name, Size size, SiftDsmInit init)
{
	return stub_segment(size, init);
}

void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
	SIFT_CHECK(nexit < lengthof(exit_cb));
	exit_cb[nexit++] = function;
}

void
AbortOutOfAnyTransaction(void)
{
}

void
dsa_detach(dsa_area *a)
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
	return AllocSetContextCreate(CurrentMemoryContext, "test manifest area", ALLOCSET_SMALL_SIZES);
}

bool
sift_shm_reserve_shared(SiftShmPoolId pool, Size bytes)
{
	if (reserve_fails)
	{
		reserve_fails = false;
		throw_error();
	}
	reserved += bytes;
	return true;
}

void
sift_shm_release_shared(SiftShmPoolId pool, Size bytes)
{
	reserved -= bytes;
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

/* The area is mappings of their own: a dsa_pointer is the address, and a read of a freed snapshot faults */
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
	char	   *raw;

	if (exit_in_alloc)
		exit_now();
	if (alloc_fails)
	{
		alloc_fails = false;
		return InvalidDsaPointer;
	}
	allocs++;
	size += MAXIMUM_ALIGNOF;
	raw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	SIFT_CHECK(raw != MAP_FAILED);
	*(size_t *) raw = size;
	return (dsa_pointer) (uintptr_t) (raw + MAXIMUM_ALIGNOF);
}

void
dsa_free(dsa_area *a, dsa_pointer dp)
{
	char	   *raw = (char *) (uintptr_t) dp - MAXIMUM_ALIGNOF;

	munmap(raw, *(size_t *) raw);
}

void *
dsa_get_address(dsa_area *a, dsa_pointer dp)
{
	SIFT_CHECKF(a != NULL, "a snapshot is read outside an area this backend has attached");
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
other_reclaims(void)
{
	reclaim_now(table, area, false);
}

/* The last reader of a retired snapshot lets go of its count and another backend frees the snapshot at once: the reader does not read it again */
static void
test_last_decref_reads_no_freed_snap(void)
{
	dsa_pointer held = fresh_snap();
	uint32		live;

	pg_atomic_fetch_add_u32(&((SiftShmManifestSnap *) dsa_get_address(area, held))->refcount, 1);
	sift_shm_snap_retire(table, area, held);
	live = pg_atomic_read_u32(&table->snaps_live);
	on_zero = other_reclaims;
	snap_decref(area, held);
	SIFT_CHECKF(on_zero == NULL && pg_atomic_read_u32(&table->snaps_live) == live - 1, "the retired snapshot goes with its last reader");
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

static void
test_peek_miss_preserves_out(void)
{
	SiftShmIndexMeta m = meta_of(13, 40000, 5);
	SiftShmIndexMeta out;

	m.scope_dir_blk = InvalidBlockNumber;
	m.nscopes = 0;
	out = m;
	SIFT_CHECK(sift_shm_manifest_publish(&m, NULL));
	sift_shm_manifest_hold(&m.locator, 6);
	SIFT_CHECKF(!sift_shm_manifest_peek(&m.locator, &out), "a held slot is not returned by peek");
	SIFT_CHECKF(out.scope_dir_blk == InvalidBlockNumber && out.manifest_version == 5, "a peek miss leaves the caller's meta untouched");
	sift_shm_manifest_invalidate(&m.locator, false);
	SIFT_CHECKF(!sift_shm_manifest_peek(&m.locator, &out), "an invalidated slot is not returned by peek");
	SIFT_CHECKF(out.scope_dir_blk == InvalidBlockNumber && out.manifest_version == 5, "an empty-slot peek miss leaves the caller's meta untouched");
}


/* The writer of version 3, which holds only the build_lock of the slot, commits while the writer of version 2 has chosen the slot under claim_lock and not written it yet */
static SiftShmIndexMeta newer;
static dsa_pointer newer_dp;

static void
newer_commits(void)
{
	char	   *fail = sift_test_dsm_fail;

	sift_test_dsm_fail = NULL;
	SIFT_CHECK(write_steady(table, area, &newer, true, newer_dp) == STEADY_WRITTEN);
	sift_test_dsm_fail = fail;
}

/* The claimed writer of version 2 (sift_test_dsm_fail sends every writer past write_steady) runs with version 3 committed between its claim and its write */
static void
race_older(RelFileNumber rel, bool hold)
{
	SiftShmIndexMeta m = meta_of(14, rel, 1);
	SiftShmSnapSpec spec = spec_of(1),
				newer_spec = spec_of(3);
	SiftShmIndexMeta out;
	bool		found;

	SIFT_CHECK(sift_shm_manifest_publish(&m, NULL) && sift_shm_manifest_publish(&m, &spec));
	newer = meta_of(14, rel, 3);
	newer.state_flags |= SIFT_SHM_SLOT_HAS_SNAP;
	newer_dp = sift_shm_snap_build(table, area, &newer.locator, &newer_spec);
	stub_hook_lock = &slot_probe(table, &m.locator)->build_lock;
	stub_hook = newer_commits;
	sift_test_dsm_fail = "none";
	m.manifest_version = 2;
	if (hold)
		sift_shm_manifest_hold(&m.locator, 2);
	else
		(void) sift_shm_manifest_publish(&m, NULL);
	sift_test_dsm_fail = NULL;
	SIFT_CHECKF(stub_hook == NULL, "the newer writer came between");
	found = sift_shm_manifest_peek(&m.locator, &out);
	SIFT_CHECKF(found && out.manifest_version == 3, "a %s of version 2 overwrote version 3 committed after its claim (peek %s, version %llu)",
				hold ? "hold" : "publication", found ? "finds it" : "misses", (unsigned long long) out.manifest_version);
	SIFT_CHECKF(snap_of(&m.locator) == newer_dp, "the snapshot of version 3 was retired by a writer of version 2");
}

static void
test_claimed_writer_rechecks(void)
{
	race_older(20100, false);
	race_older(20101, true);
}

/* A meta of the version the slot has the snapshot of (the counters of the visibility map, the flags of a TTL) is written, and the snapshot stays */
static void
test_meta_keeps_snapshot(void)
{
	SiftShmIndexMeta m = meta_of(15, 20200, 5);
	SiftShmSnapSpec spec = spec_of(5);
	SiftShmIndexMeta out;
	dsa_pointer first;

	SIFT_CHECK(sift_shm_manifest_publish(&m, NULL) && sift_shm_manifest_publish(&m, &spec));
	first = snap_of(&m.locator);
	m.total_rows = 77;
	SIFT_CHECK(sift_shm_manifest_publish(&m, NULL));
	SIFT_CHECKF(sift_shm_manifest_peek(&m.locator, &out) && out.total_rows == 77, "the meta of the version is written");
	SIFT_CHECKF(snap_of(&m.locator) == first && (out.state_flags & SIFT_SHM_SLOT_HAS_SNAP), "a meta of the version dropped the snapshot of the slot");
}

static SiftShmIndexMeta build_meta;

static void
publish_build(void *arg)
{
	SiftShmSnapSpec spec = spec_of(build_meta.manifest_version);

	(void) sift_shm_manifest_publish(&build_meta, &spec);
}

/* A reader that claimed the build of a version and fails gives the claim back: the next reader builds it */
static void
test_failed_build_released(void)
{
	SiftShmSnapSpec spec = spec_of(4);

	build_meta = meta_of(16, 20300, 4);
	SIFT_CHECK(sift_shm_manifest_publish(&build_meta, NULL));
	reserve_fails = true;
	SIFT_CHECKF(raises(publish_build, NULL), "the build fails");
	SIFT_CHECKF(sift_shm_manifest_publish(&build_meta, &spec) && snap_of(&build_meta.locator) != InvalidDsaPointer,
				"a build that failed keeps the next reader of the version from making its snapshot");
}

static void
fn_throws(const SiftShmManifestSnap *snap, void *arg)
{
	throw_error();
}

static void
run_throwing(void *arg)
{
	const SiftShmIndexMeta *m = arg;

	(void) sift_shm_snap_with_epoch(&m->locator, m->manifest_version, fn_throws, NULL);
}

/* The snapshot of a version of an index of its own */
static SiftShmManifestSnap *
published_snap(SiftShmIndexMeta *m)
{
	SiftShmSnapSpec spec = spec_of(m->manifest_version);

	SIFT_CHECK(sift_shm_manifest_publish(m, NULL) && sift_shm_manifest_publish(m, &spec));
	return dsa_get_address(area, snap_of(&m->locator));
}

/* A reader beyond the epoch slots pins the snapshot by its refcount: one whose callback fails gives the count back */
static void
test_pinned_reader_error(void)
{
	SiftShmIndexMeta m = meta_of(17, 20400, 3);
	SiftShmManifestSnap *snap = published_snap(&m);
	uint32		before = pg_atomic_read_u32(&snap->refcount);
	bool		raised;

	MyProcNumber = SIFT_SHM_EPOCH_SLOTS + 1;
	raised = raises(run_throwing, &m);
	MyProcNumber = 0;
	SIFT_CHECK(raised);
	SIFT_CHECKF(pg_atomic_read_u32(&snap->refcount) == before, "a reader that failed holds the snapshot for ever (refcount %u, %u before)", pg_atomic_read_u32(&snap->refcount), before);
}

/* A backend that exits (FATAL) while it copies a list gives back the slot of the list and the bytes it took: the area outlives it (another backend is attached) */
static void
test_list_install_exit(void)
{
	SiftShmIndexMeta m = meta_of(18, 20500, 3);
	SiftShmManifestSnap *snap = published_snap(&m);
	pg_atomic_uint64 *slot = part_list_slot(snap, 1);
	int64		before = reserved;
	uint8		list[64] = {0};

	table->area_users++;
	exit_in_alloc = true;
	if (sigsetjmp(exit_jmp, 0) == 0)
		(void) sift_shm_snap_part_list_install(snap, 1, list, sizeof(list));
	SIFT_CHECKF(!exit_in_alloc && area == NULL, "the backend exited in the copy");
	SIFT_CHECKF(pg_atomic_read_u64(slot) == InvalidDsaPointer, "the list of a backend that exited is installing for ever");
	SIFT_CHECKF(reserved == before, "a backend that exited in the copy of a list keeps %lld bytes of the ledger", (long long) (reserved - before));
}

/* The area has no room for the snapshot (DSA_ALLOC_NO_OOM): the build is given back, not held for the version */
static void
test_oom_build_released(void)
{
	SiftShmIndexMeta m = meta_of(19, 20600, 5);
	SiftShmSnapSpec spec = spec_of(5);

	SIFT_CHECK(sift_shm_manifest_publish(&m, NULL));
	alloc_fails = true;
	SIFT_CHECK(!sift_shm_manifest_publish(&m, &spec));
	SIFT_CHECKF(sift_shm_manifest_publish(&m, &spec) && snap_of(&m.locator) != InvalidDsaPointer,
				"a build the area had no room for keeps the next reader of the version from making its snapshot");
}

/* A builder that exits (FATAL) in the middle of the build gives the claim back */
static void
test_build_exit_released(void)
{
	SiftShmSnapSpec spec;

	build_meta = meta_of(20, 20700, 6);
	spec = spec_of(6);
	claim.exit_set = false;		/* the exit of test_list_install_exit ran the callback of this process */
	SIFT_CHECK(sift_shm_manifest_publish(&build_meta, NULL));
	table->area_users++;
	exit_in_alloc = true;
	if (sigsetjmp(exit_jmp, 0) == 0)
		publish_build(NULL);
	SIFT_CHECK(!exit_in_alloc);
	SIFT_CHECK(sift_shm_area_attach(table, true) != NULL);
	SIFT_CHECKF(sift_shm_manifest_publish(&build_meta, &spec) && snap_of(&build_meta.locator) != InvalidDsaPointer,
				"a builder that exited keeps the next reader of the version from making its snapshot");
}

/* Backend B, which makes the area: the snapshot of version 2 it publishes, and its exit once claim_lock is free (area_leave takes it) */
static SiftShmIndexMeta other_meta;
static dsa_pointer other_dp;
static dsa_area *const other_area = (dsa_area *) &allocs;

static bool
claim_held(void)
{
	for (int i = 0; i < stub_nheld; i++)
		if (stub_held[i] == &table->claim_lock)
			return true;
	return false;
}

static void
other_leaves(void)
{
	if (claim_held())
	{
		stub_hook_lock = &table->area_lock;
		stub_hook = other_leaves;
		return;
	}
	if (--table->area_users == 0)
		table->area_handle = DSA_HANDLE_INVALID;
}

static void
other_publishes(void)
{
	char	   *fail = sift_test_dsm_fail;
	SiftShmSnapSpec spec = spec_of(2);

	table->area_handle = dsa_get_handle(other_area);
	table->area_users++;
	other_meta = meta_of(21, 20800, 2);
	other_meta.state_flags |= SIFT_SHM_SLOT_HAS_SNAP;
	other_dp = sift_shm_snap_build(table, other_area, &other_meta.locator, &spec);
	sift_test_dsm_fail = NULL;
	SIFT_CHECK(write_steady(table, other_area, &other_meta, true, other_dp) == STEADY_WRITTEN);
	sift_test_dsm_fail = fail;
	stub_hook_lock = &table->area_lock;
	stub_hook = other_leaves;
}

/* A writer that has no area (a meta with no snapshot) claims the slot; B publishes a snapshot before the write and leaves after claim_lock is let go: the writer that takes B's snapshot out of the slot retires it in an area it holds */
static void
test_unattached_writer_retires_in_area(void)
{
	SiftShmIndexMeta m = meta_of(21, 20800, 1);
	SiftShmIndexMeta out;

	SIFT_CHECK(area == NULL && table->area_handle == DSA_HANDLE_INVALID && sift_shm_manifest_publish(&m, NULL));
	stub_hook_lock = &slot_probe(table, &m.locator)->build_lock;
	stub_hook = other_publishes;
	sift_test_dsm_fail = "none";
	m.manifest_version = 3;
	(void) sift_shm_manifest_publish(&m, NULL);
	sift_test_dsm_fail = NULL;
	SIFT_CHECKF(stub_hook_lock == &table->area_lock, "B published between the claim and the write");
	stub_hook = NULL;
	SIFT_CHECKF(sift_shm_manifest_peek(&m.locator, &out) && out.manifest_version == 3 && snap_of(&m.locator) == InvalidDsaPointer, "the meta of version 3 replaced B's snapshot");
	SIFT_CHECKF(area != NULL, "the writer took a snapshot out of the slot without the area attached");
	if (table->area_handle != DSA_HANDLE_INVALID)
		table->area_users--;
}

static void
publish_meta(void *arg)
{
	(void) sift_shm_manifest_publish(arg, NULL);
}

/* A writer whose slot holds a snapshot it cannot drop fails (ERROR) and lets go of claim_lock: a caller that catches the error claims slots again */
static void
test_failed_drop_releases_claim(void)
{
	SiftShmIndexMeta m = meta_of(22, 20900, 3);
	bool		raised;

	(void) published_snap(&m);
	m.manifest_version = 4;
	sift_test_dsm_fail = "manifest_drop_snap";
	pgshim_error_hook = throw_error;
	raised = raises(publish_meta, &m);
	pgshim_error_hook = NULL;
	sift_test_dsm_fail = NULL;
	SIFT_CHECK(raised);
	SIFT_CHECKF(!claim_held(), "a writer that failed to drop the snapshot of its slot holds claim_lock");
}

int
main(void)
{
	SIFT_CHECK(sift_shm_registry() != NULL);
	test_unattached_writer_retires_in_area();
	SIFT_CHECK(sift_shm_area_attach(table, true) != NULL);
	test_one_snapshot_per_version();
	test_slot_refuses_same_version();
	test_retire_is_fifo();
	test_last_decref_reads_no_freed_snap();
	test_invalidate_db();
	test_peek_miss_preserves_out();
	test_claimed_writer_rechecks();
	test_meta_keeps_snapshot();
	test_oom_build_released();
#ifndef __SANITIZE_THREAD__
	/* one thread; ThreadSanitizer does not find the jmp_buf of a PG_TRY that the ERROR goes to */
	test_failed_build_released();
	test_pinned_reader_error();
	test_list_install_exit();
	test_build_exit_released();
	test_failed_drop_releases_claim();
#endif
	return sift_check_summary();
}
