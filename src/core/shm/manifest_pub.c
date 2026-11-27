/*
 * core/shm/manifest_pub.c - the writers of the slots of the registry of the manifests (core/shm/manifest.h): hold, publish and invalidate. A slot has one writer at a time, by its build_lock; the writer of an index
 * the registry already holds takes that lock alone, and the one that gives an index a slot (or takes one from another index) takes claim_lock to choose it.
 *
 * A commit that frees blocks holds the slot of its index before Block 0 changes (sift_shm_manifest_hold): from then on the registry serves no snapshot of the index, and a meta of an older version is
 * not published over the slot, so a reader that comes after the commit reads Block 0 and never the blocks the commit freed, whether the publication that follows is late or fails.
 */
#include "postgres.h"

#include "storage/ipc.h"

#include "col/dsmreg.h"
#include "core/cachebudget.h"
#include "core/shm/budget.h"
#include "core/shm/lease.h"
#include "core/shm/manifest_int.h"

typedef enum Steady
{
	STEADY_NO,					/* the index has no slot that is live (or none that is not being changed by a writer that chooses slots): claim_lock decides */
	STEADY_REFUSED,
	STEADY_WRITTEN
} Steady;

/* What a slot costs to give up: a dead one nothing, a live one its cache; a quarantined one is not given up, it keeps the index from being trusted */
static int
slot_value(SiftShmIndexSlot *s)
{
	return (s->meta.state_flags & SIFT_SHM_SLOT_DEAD) ? 0 : 1;
}

/* The slot of the locator in the probe window, or NULL (claim_lock held) */
static SiftShmIndexSlot *
slot_find(Table *t, const RelFileLocator *loc)
{
	for (int i = 0; i < SIFT_SHM_MANIFEST_PROBES; i++)
	{
		SiftShmIndexSlot *s = sift_shm_slot_at(t, loc, i);

		if (s->meta.state_flags == SIFT_SHM_SLOT_EMPTY)
			break;
		if (RelFileLocatorEquals(s->meta.locator, *loc))
			return s;
	}
	return NULL;
}

/* The slot of the locator for a writer (claim_lock held): the one it has, an empty one, or the one a full probe window gives up (a dead one first, then the least recently used); NULL when the window holds nothing but quarantined indexes */
static SiftShmIndexSlot *
slot_claim(Table *t, const RelFileLocator *loc)
{
	SiftShmIndexSlot *victim = NULL;

	for (int i = 0; i < SIFT_SHM_MANIFEST_PROBES; i++)
	{
		SiftShmIndexSlot *s = sift_shm_slot_at(t, loc, i);

		if (s->meta.state_flags == SIFT_SHM_SLOT_EMPTY || RelFileLocatorEquals(s->meta.locator, *loc))
			return s;
		if (s->meta.state_flags & SIFT_SHM_SLOT_QUARANTINED)
			continue;
		if (victim == NULL || slot_value(s) < slot_value(victim) ||
			(slot_value(s) == slot_value(victim) && pg_atomic_read_u64(&s->last_used_tick) < pg_atomic_read_u64(&victim->last_used_tick)))
			victim = s;
	}
	if (victim != NULL && slot_value(victim) > 0)
		sift_shm_note_eviction(SIFT_SHM_MANIFEST, 1);
	return victim;
}

/* The area a writer retires the snapshot of the slot in, attached before anything changes so that the snapshot can go (claim_lock held); NULL where the slot has no snapshot */
static dsa_area *
area_for_drop(Table *t, SiftShmIndexSlot *s)
{
	dsa_area   *a;

	if (pg_atomic_read_u64(&s->current_snap_dp) == InvalidDsaPointer)
		return NULL;
#ifdef SIFT_TESTING
	if (sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "manifest_drop_snap") != NULL)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift test: the snapshot of the slot cannot be dropped (manifest_drop_snap)")));	/* check-errors: allow */
#endif
	a = sift_shm_area_attach(t, false);
	if (a == NULL)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift: the area of the manifest snapshots cannot be attached"),
						errhint("Shared memory is short; the snapshots are held until a session that can attach the area drops them.")));
	return a;
}

/* A snapshot a writer took out of a slot is retired in the area the writer held attached when it took it: the last backend to leave the area frees only the snapshots the slots still hold */
static void
retire_if_any(Table *t, dsa_area *a, dsa_pointer dp)
{
	Assert(dp == InvalidDsaPointer || a != NULL);
	if (dp != InvalidDsaPointer)
		sift_shm_snap_retire(t, a, dp);
}

/* A snapshot of the version the slot already holds one of: the readers that load a version at once would replace each other's, and drop the lists the first one installed */
static bool
same_snapshot(const SiftShmIndexSlot *s, const SiftShmIndexMeta *meta)
{
	return (s->meta.state_flags & meta->state_flags & SIFT_SHM_SLOT_HAS_SNAP) && s->meta.manifest_version == meta->manifest_version;
}

/* Whether the slot of the writer is no slot to write the meta to (build_lock held): there is none, the index is quarantined in it, or it holds, valid or held, a newer version of the index or a snapshot of this one */
static bool
slot_refuses(const SiftShmIndexSlot *s, const SiftShmIndexMeta *meta)
{
	if (s == NULL)
		return true;
	if (!RelFileLocatorEquals(s->meta.locator, meta->locator))
		return false;
	return (s->meta.state_flags & SIFT_SHM_SLOT_QUARANTINED) ||
		((s->meta.state_flags & (SIFT_SHM_SLOT_VALID | SIFT_SHM_SLOT_HELD)) && (s->meta.manifest_version > meta->manifest_version || same_snapshot(s, meta)));
}

/* The slot goes to the index of the locator: the pins it counted were another index's */
static void
slot_adopt(SiftShmIndexSlot *s, const RelFileLocator *locator)
{
	if (RelFileLocatorEquals(s->meta.locator, *locator))
		return;
	pg_atomic_write_u64(&s->snap_hits, 0);
	pg_atomic_write_u64(&s->snap_misses, 0);
	pg_atomic_write_u64(&s->building, 0);
}

/* A meta with no snapshot of the version the slot has the snapshot of (the counters of a VACUUM, the flags of a TTL): the snapshot is of the version, and stays */
static bool
keeps_snapshot(const SiftShmIndexSlot *s, const SiftShmIndexMeta *m)
{
	return (m->state_flags & (SIFT_SHM_SLOT_VALID | SIFT_SHM_SLOT_HAS_SNAP)) == SIFT_SHM_SLOT_VALID && (s->meta.state_flags & SIFT_SHM_SLOT_HAS_SNAP) &&
		RelFileLocatorEquals(s->meta.locator, m->locator) && s->meta.manifest_version == m->manifest_version;
}

/*
 * The meta, and the snapshot dp where swap, go to the slot unless it refuses them; the check and the write are one hold of its build_lock (held), so a writer that holds that lock alone
 * and commits a newer version in between is not undone. The snapshot the slot had goes to *old
 */
static Steady
slot_put(Table *t, SiftShmIndexSlot *s, const SiftShmIndexMeta *meta, bool swap, dsa_pointer dp, dsa_pointer *old)
{
	SiftShmIndexMeta m = *meta;

	*old = InvalidDsaPointer;
	if (slot_refuses(s, &m))
		return STEADY_REFUSED;
	slot_adopt(s, &m.locator);
	if (keeps_snapshot(s, &m))
		m.state_flags |= SIFT_SHM_SLOT_HAS_SNAP;
	else if (swap)
		*old = sift_shm_slot_set_snap(s, dp);
	sift_shm_meta_write(s, &m);
	pg_atomic_write_u64(&s->last_used_tick, sift_shm_tick(t));
	return STEADY_WRITTEN;
}

/* The slot is dead or quarantined (flags) for the index of the locator, its meta read and written in one hold of its build_lock */
static void
slot_mark(Table *t, SiftShmIndexSlot *s, const RelFileLocator *locator, uint32 flags)
{
	SiftShmIndexMeta m;

	LWLockAcquire(&s->build_lock, LW_EXCLUSIVE);
	m = RelFileLocatorEquals(s->meta.locator, *locator) ? s->meta : (SiftShmIndexMeta) {.locator = *locator};
	m.state_flags = flags;
	slot_adopt(s, locator);
	sift_shm_meta_write(s, &m);
	pg_atomic_write_u64(&s->last_used_tick, sift_shm_tick(t));
	LWLockRelease(&s->build_lock);
}

/* The slot of the locator, found without a lock: what a caller reads of it is read again under its build_lock */
static SiftShmIndexSlot *
slot_probe(Table *t, const RelFileLocator *loc)
{
	for (int i = 0; i < SIFT_SHM_MANIFEST_PROBES; i++)
	{
		SiftShmIndexSlot *s = sift_shm_slot_at(t, loc, i);

		if (s->meta.state_flags == SIFT_SHM_SLOT_EMPTY)
			break;
		if (RelFileLocatorEquals(s->meta.locator, *loc))
			return s;
	}
	return NULL;
}

/* A writer of an index that the registry holds (valid or held, not quarantined or dead) takes the build_lock of its slot and nothing else; the snapshot it replaces is retired once the lock is let go */
static Steady
write_steady(Table *t, dsa_area *a, const SiftShmIndexMeta *m, bool swap, dsa_pointer dp)
{
	SiftShmIndexSlot *s = slot_probe(t, &m->locator);
	dsa_pointer old;
	Steady		steady;

#ifdef SIFT_TESTING
	if (sift_test_dsm_fail != NULL)
		return STEADY_NO;
#endif
	if (s == NULL)
		return STEADY_NO;
	LWLockAcquire(&s->build_lock, LW_EXCLUSIVE);
	if (!RelFileLocatorEquals(s->meta.locator, m->locator) || !(s->meta.state_flags & (SIFT_SHM_SLOT_VALID | SIFT_SHM_SLOT_HELD)) ||
		(s->meta.state_flags & SIFT_SHM_SLOT_QUARANTINED) || (swap && a == NULL && pg_atomic_read_u64(&s->current_snap_dp) != InvalidDsaPointer))
	{
		LWLockRelease(&s->build_lock);
		return STEADY_NO;
	}
	steady = slot_put(t, s, m, swap, dp, &old);
	LWLockRelease(&s->build_lock);
	retire_if_any(t, a, old);
	return steady;
}

void
sift_shm_manifest_hold(const RelFileLocator *locator, uint64 version)
{
	Table	   *t;
	SiftShmIndexSlot *s;
	dsa_pointer old;
	SiftShmIndexMeta m = {.locator = *locator,.manifest_version = version,.state_flags = SIFT_SHM_SLOT_HELD};

	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL || write_steady(t, NULL, &m, false, InvalidDsaPointer) != STEADY_NO)
		return;
	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = slot_claim(t, locator);
	if (s != NULL)
	{
		LWLockAcquire(&s->build_lock, LW_EXCLUSIVE);
		(void) slot_put(t, s, &m, false, InvalidDsaPointer, &old);
		LWLockRelease(&s->build_lock);
	}
	LWLockRelease(&t->claim_lock);
}

/* The slot of the index is chosen under claim_lock; the snapshot it had is retired after the lock is let go */
static Steady
publish_claimed(Table *t, dsa_area *a, const SiftShmIndexMeta *m, dsa_pointer dp)
{
	SiftShmIndexSlot *s;
	dsa_pointer old;
	dsa_area   *drop_area;
	Steady		steady;

	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = slot_claim(t, &m->locator);
	if (slot_refuses(s, m))
	{
		LWLockRelease(&t->claim_lock);
		return STEADY_REFUSED;
	}
	/* the build_lock is taken with the area attached wherever the slot has a snapshot to drop: a writer that holds only a build_lock may publish one after area_for_drop looked */
	PG_TRY();
	{
		drop_area = area_for_drop(t, s);
		for (;;)
		{
			LWLockAcquire(&s->build_lock, LW_EXCLUSIVE);
			if (drop_area != NULL || a != NULL || pg_atomic_read_u64(&s->current_snap_dp) == InvalidDsaPointer)
				break;
			LWLockRelease(&s->build_lock);
			drop_area = area_for_drop(t, s);
		}
	}
	PG_CATCH();
	{
		if (dp != InvalidDsaPointer)
			sift_shm_snap_free(a, dsa_get_address(a, dp));
		PG_RE_THROW();
	}
	PG_END_TRY();
	steady = slot_put(t, s, m, true, dp, &old);
	LWLockRelease(&s->build_lock);
	LWLockRelease(&t->claim_lock);
	retire_if_any(t, drop_area != NULL ? drop_area : a, old);
	return steady;
}

/* The reader that is to make the snapshot of the version: the first one to claim it since the slot lost its snapshot. An index with no slot yet has none to claim: claim_lock gives it one */
static bool
build_claimed(Table *t, const SiftShmIndexMeta *meta)
{
	SiftShmIndexSlot *s = slot_probe(t, &meta->locator);
	uint64		b;

	if (s == NULL)
		return true;
	b = pg_atomic_read_u64(&s->building);
	while (b < meta->manifest_version)
		if (pg_atomic_compare_exchange_u64(&s->building, &b, meta->manifest_version))
			return true;
	return false;
}

/* The build this backend claimed and has not installed: given back when the snapshot is not installed, or at an exit that no PG_FINALLY sees (FATAL) */
static struct
{
	Table	   *t;				/* NULL: none */
	RelFileLocator locator;
	uint64		version;
	bool		exit_set;
}			claim;

/* A reader that claimed the build of the version and did not install it gives the claim back: the next reader makes the snapshot */
static void
build_release(int code pg_attribute_unused(), Datum arg pg_attribute_unused())
{
	SiftShmIndexSlot *s = claim.t != NULL ? slot_probe(claim.t, &claim.locator) : NULL;
	uint64		b = claim.version;

	if (s != NULL)
		(void) pg_atomic_compare_exchange_u64(&s->building, &b, 0);
	claim.t = NULL;
}

static void
claim_note(Table *t, const SiftShmIndexMeta *meta)
{
	if (!claim.exit_set)
		before_shmem_exit(build_release, (Datum) 0);
	claim.exit_set = true;
	claim.locator = meta->locator;
	claim.version = meta->manifest_version;
	claim.t = t;
}

static Steady
publish_built(Table *t, const SiftShmIndexMeta *meta, const SiftShmSnapSpec *spec, dsa_pointer *dp)
{
	dsa_area   *a = sift_shm_area_attach(t, spec != NULL);
	Steady		steady;
	SiftShmIndexMeta m = *meta;

	if (spec != NULL && a != NULL)
		*dp = sift_shm_snap_build(t, a, &meta->locator, spec);
	m.state_flags = SIFT_SHM_SLOT_VALID | (*dp != InvalidDsaPointer ? SIFT_SHM_SLOT_HAS_SNAP : 0);
	steady = write_steady(t, a, &m, true, *dp);
	if (steady == STEADY_NO)
		steady = publish_claimed(t, a, &m, *dp);
	if (steady == STEADY_REFUSED && *dp != InvalidDsaPointer)
		sift_shm_snap_free(a, dsa_get_address(a, *dp));
	return steady;
}

bool
sift_shm_manifest_publish(const SiftShmIndexMeta *meta, const SiftShmSnapSpec *spec)
{
	Table	   *t;
	dsa_pointer dp = InvalidDsaPointer;
	volatile bool installed = false;

	if ((meta->state_flags & SIFT_SHM_SLOT_PRIVATE) != 0 || !sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
		return false;
	if (spec != NULL && !build_claimed(t, meta))
		return false;
	if (spec != NULL)
		claim_note(t, meta);
	PG_TRY();
	{
		installed = publish_built(t, meta, spec, &dp) == STEADY_WRITTEN && (spec == NULL || dp != InvalidDsaPointer);
	}
	PG_FINALLY();
	{
		if (!installed)
			build_release(0, (Datum) 0);
		claim.t = NULL;
	}
	PG_END_TRY();
	return installed;
}

void
sift_shm_manifest_invalidate(const RelFileLocator *locator, bool quarantine)
{
	Table	   *t;
	SiftShmIndexSlot *s;
	dsa_area   *a = NULL;
	dsa_pointer old = InvalidDsaPointer;

	sift_lease_invalidate(locator);
	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
		return;
	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = quarantine ? slot_claim(t, locator) : slot_find(t, locator);
	if (s != NULL)
	{
		slot_mark(t, s, locator, quarantine ? SIFT_SHM_SLOT_QUARANTINED : SIFT_SHM_SLOT_DEAD);
		a = area_for_drop(t, s);
		if (a != NULL)
			old = sift_shm_slot_swap_snap(s, InvalidDsaPointer);
	}
	LWLockRelease(&t->claim_lock);
	retire_if_any(t, a, old);
}

#define DROP_BATCH	64

/* The slots of the indexes of the database from slot `from` on are dead, up to DROP_BATCH of their snapshots taken out into old (claim_lock held); the slot to go on from */
static int
drop_db_slots(Table *t, Oid db, int from, dsa_area **a, dsa_pointer *old, int *nold)
{
	int			i;

	for (i = from; i < SIFT_SHM_MANIFEST_SLOTS && *nold < DROP_BATCH; i++)
	{
		SiftShmIndexSlot *s = &t->slot[i];
		uint32		flags = s->meta.state_flags;

		if (flags == SIFT_SHM_SLOT_EMPTY || (flags & SIFT_SHM_SLOT_DEAD) || s->meta.locator.dbOid != db)
			continue;
		slot_mark(t, s, &s->meta.locator, SIFT_SHM_SLOT_DEAD);
		if (*a == NULL)
			*a = area_for_drop(t, s);
		if (*a != NULL)
			old[(*nold)++] = sift_shm_slot_swap_snap(s, InvalidDsaPointer);
	}
	return i;
}

void
sift_shm_manifest_invalidate_db(Oid db)
{
	Table	   *t;
	dsa_area   *a = NULL;
	dsa_pointer old[DROP_BATCH];
	int			at = 0;

	sift_lease_invalidate_db(db);
	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
		return;
	while (at < SIFT_SHM_MANIFEST_SLOTS)
	{
		int			nold = 0;

		LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
		at = drop_db_slots(t, db, at, &a, old, &nold);
		LWLockRelease(&t->claim_lock);
		for (int i = 0; i < nold; i++)
			retire_if_any(t, a, old[i]);
	}
}
