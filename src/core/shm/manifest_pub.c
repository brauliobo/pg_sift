/*
 * core/shm/manifest_pub.c - the writers of the slots of the registry of the manifests (core/shm/manifest.h): hold, publish and invalidate. A slot has one writer at a time, by its build_lock; the writer of an index
 * the registry already holds takes that lock alone, and the one that gives an index a slot (or takes one from another index) takes claim_lock to choose it.
 *
 * A commit that frees blocks holds the slot of its index before Block 0 changes (sift_shm_manifest_hold): from then on the registry serves no snapshot of the index, and a meta of an older version is
 * not published over the slot, so a reader that comes after the commit reads Block 0 and never the blocks the commit freed, whether the publication that follows is late or fails.
 */
#include "postgres.h"

#include "col/dsmreg.h"
#include "core/cachebudget.h"
#include "core/shm/budget.h"
#include "core/shm/lease.h"
#include "core/shm/manifest_int.h"

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

static void
retire_if_any(Table *t, dsa_area *a, dsa_pointer dp)
{
	if (dp != InvalidDsaPointer)
		sift_shm_snap_retire(t, a != NULL ? a : sift_shm_area_attach(t, false), dp);
}

/* A snapshot of the version the slot already holds one of: the readers that load a version at once would replace each other's, and drop the lists the first one installed */
static bool
same_snapshot(const SiftShmIndexSlot *s, const SiftShmIndexMeta *meta)
{
	return (s->meta.state_flags & meta->state_flags & SIFT_SHM_SLOT_HAS_SNAP) && s->meta.manifest_version == meta->manifest_version;
}

/* Whether the slot of the writer is no slot to write the meta to (claim_lock held): there is none, the index is quarantined in it, or it holds, valid or held, a newer version of the index or a snapshot of this one */
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

/* The meta of a slot, and the snapshot where swap is true, change together under its build_lock: the snapshot the slot had */
static dsa_pointer
slot_write(Table *t, SiftShmIndexSlot *s, const SiftShmIndexMeta *m, bool swap, dsa_pointer dp)
{
	dsa_pointer old = InvalidDsaPointer;

	LWLockAcquire(&s->build_lock, LW_EXCLUSIVE);
	if (swap)
		old = sift_shm_slot_set_snap(s, dp);
	sift_shm_meta_write(s, m);
	pg_atomic_write_u64(&s->last_used_tick, sift_shm_tick(t));
	LWLockRelease(&s->build_lock);
	return old;
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

typedef enum Steady
{
	STEADY_NO,					/* the index has no slot that is live (or none that is not being changed by a writer that chooses slots): claim_lock decides */
	STEADY_REFUSED,
	STEADY_WRITTEN
} Steady;

/* A writer of an index that the registry holds (valid or held, not quarantined or dead) takes the build_lock of its slot and nothing else; the snapshot it replaces is retired once the lock is let go */
static Steady
write_steady(Table *t, dsa_area *a, const SiftShmIndexMeta *m, bool swap, dsa_pointer dp)
{
	SiftShmIndexSlot *s = slot_probe(t, &m->locator);
	dsa_pointer old;

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
	if (slot_refuses(s, m))
	{
		LWLockRelease(&s->build_lock);
		return STEADY_REFUSED;
	}
	old = swap ? sift_shm_slot_set_snap(s, dp) : InvalidDsaPointer;
	sift_shm_meta_write(s, m);
	pg_atomic_write_u64(&s->last_used_tick, sift_shm_tick(t));
	LWLockRelease(&s->build_lock);
	retire_if_any(t, a, old);
	return STEADY_WRITTEN;
}

void
sift_shm_manifest_hold(const RelFileLocator *locator, uint64 version)
{
	Table	   *t;
	SiftShmIndexSlot *s;
	SiftShmIndexMeta m = {.locator = *locator,.manifest_version = version,.state_flags = SIFT_SHM_SLOT_HELD};

	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL || write_steady(t, NULL, &m, false, InvalidDsaPointer) != STEADY_NO)
		return;
	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = slot_claim(t, locator);
	if (!slot_refuses(s, &m))
	{
		slot_adopt(s, locator);
		(void) slot_write(t, s, &m, false, InvalidDsaPointer);
	}
	LWLockRelease(&t->claim_lock);
}

/* The slot of the index is chosen under claim_lock; the snapshot it had is retired after the lock is let go */
static Steady
publish_claimed(Table *t, dsa_area *a, const SiftShmIndexMeta *m, dsa_pointer dp)
{
	SiftShmIndexSlot *s;
	dsa_pointer old;
	dsa_area   *drop_area = NULL;

	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = slot_claim(t, &m->locator);
	if (slot_refuses(s, m))
	{
		LWLockRelease(&t->claim_lock);
		return STEADY_REFUSED;
	}
	PG_TRY();
	{
		drop_area = area_for_drop(t, s);
	}
	PG_CATCH();
	{
		if (dp != InvalidDsaPointer)
			sift_shm_snap_free(a, dsa_get_address(a, dp));
		PG_RE_THROW();
	}
	PG_END_TRY();
	slot_adopt(s, &m->locator);
	old = slot_write(t, s, m, true, dp);
	LWLockRelease(&t->claim_lock);
	retire_if_any(t, drop_area != NULL ? drop_area : a, old);
	return STEADY_WRITTEN;
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

bool
sift_shm_manifest_publish(const SiftShmIndexMeta *meta, const SiftShmSnapSpec *spec)
{
	Table	   *t;
	dsa_area   *a;
	dsa_pointer dp = InvalidDsaPointer;
	Steady		steady;
	SiftShmIndexMeta m = *meta;

	if ((meta->state_flags & SIFT_SHM_SLOT_PRIVATE) != 0 || !sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
		return false;
	if (spec != NULL && !build_claimed(t, meta))
		return false;
	a = sift_shm_area_attach(t, spec != NULL);
	if (spec != NULL && a != NULL)
		dp = sift_shm_snap_build(t, a, &meta->locator, spec);
	m.state_flags = SIFT_SHM_SLOT_VALID | (dp != InvalidDsaPointer ? SIFT_SHM_SLOT_HAS_SNAP : 0);
	steady = write_steady(t, a, &m, true, dp);
	if (steady == STEADY_NO)
		steady = publish_claimed(t, a, &m, dp);
	if (steady == STEADY_REFUSED && dp != InvalidDsaPointer)
		sift_shm_snap_free(a, dsa_get_address(a, dp));
	return steady == STEADY_WRITTEN && (spec == NULL || dp != InvalidDsaPointer);
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
		bool		mine = RelFileLocatorEquals(s->meta.locator, *locator);
		SiftShmIndexMeta m = mine ? s->meta : (SiftShmIndexMeta) {.locator = *locator};

		slot_adopt(s, locator);
		m.state_flags = quarantine ? SIFT_SHM_SLOT_QUARANTINED : SIFT_SHM_SLOT_DEAD;
		(void) slot_write(t, s, &m, false, InvalidDsaPointer);
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
		SiftShmIndexMeta m = s->meta;

		if (m.state_flags == SIFT_SHM_SLOT_EMPTY || (m.state_flags & SIFT_SHM_SLOT_DEAD) || m.locator.dbOid != db)
			continue;
		m.state_flags = SIFT_SHM_SLOT_DEAD;
		(void) slot_write(t, s, &m, false, InvalidDsaPointer);
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
