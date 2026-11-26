/*
 * core/shm/manifest_pub.c - the writers of the slots of the registry of the manifests (core/shm/manifest.h): hold, publish and invalidate. A writer takes claim_lock, so a slot has one writer at a time.
 *
 * A commit that frees blocks holds the slot of its index before Block 0 changes (sift_shm_manifest_hold): from then on the registry serves no snapshot of the index, and a meta of an older version is
 * not published over the slot, so a reader that comes after the commit reads Block 0 and never the blocks the commit freed, whether the publication that follows is late or fails.
 */
#include "postgres.h"

#include "col/dsmreg.h"
#include "core/cachebudget.h"
#include "core/shm/budget.h"
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

/* The snapshot a slot holds goes: the area is attached first, so that the snapshot can be freed (claim_lock held) */
static void
slot_drop_snap(Table *t, SiftShmIndexSlot *s)
{
	dsa_area   *a;

	if (pg_atomic_read_u64(&s->current_snap_dp) == InvalidDsaPointer)
		return;
#ifdef SIFT_TESTING
	if (sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "manifest_drop_snap") != NULL)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift test: the snapshot of the slot cannot be dropped (manifest_drop_snap)")));	/* check-errors: allow */
#endif
	a = sift_shm_area_attach(t, false);
	if (a == NULL)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift: the area of the manifest snapshots cannot be attached"),
						errhint("Shared memory is short; the snapshots are held until a session that can attach the area drops them.")));
	sift_shm_snap_retire(t, a, sift_shm_slot_swap_snap(s, InvalidDsaPointer));
}

/* Whether the slot of the writer is no slot to write the meta to (claim_lock held): there is none, the index is quarantined in it, or it holds, valid or held, a newer version of the index */
static bool
slot_refuses(const SiftShmIndexSlot *s, const SiftShmIndexMeta *meta)
{
	if (s == NULL)
		return true;
	if (!RelFileLocatorEquals(s->meta.locator, meta->locator))
		return false;
	return (s->meta.state_flags & SIFT_SHM_SLOT_QUARANTINED) ||
		((s->meta.state_flags & (SIFT_SHM_SLOT_VALID | SIFT_SHM_SLOT_HELD)) && s->meta.manifest_version > meta->manifest_version);
}

/* The slot goes to the index of the locator: the pins it counted were another index's */
static void
slot_adopt(SiftShmIndexSlot *s, const RelFileLocator *locator)
{
	if (RelFileLocatorEquals(s->meta.locator, *locator))
		return;
	pg_atomic_write_u64(&s->snap_hits, 0);
	pg_atomic_write_u64(&s->snap_misses, 0);
}

void
sift_shm_manifest_hold(const RelFileLocator *locator, uint64 version)
{
	Table	   *t;
	SiftShmIndexSlot *s;
	SiftShmIndexMeta m = {.locator = *locator,.manifest_version = version,.state_flags = SIFT_SHM_SLOT_HELD};

	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
		return;
	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = slot_claim(t, locator);
	if (!slot_refuses(s, &m))
	{
		slot_adopt(s, locator);
		sift_shm_meta_write(s, &m);
		pg_atomic_write_u64(&s->last_used_tick, pg_atomic_fetch_add_u64(&t->clock, 1));
	}
	LWLockRelease(&t->claim_lock);
}

bool
sift_shm_manifest_publish(const SiftShmIndexMeta *meta, const SiftShmSnapSpec *spec)
{
	Table	   *t;
	dsa_area   *a = NULL;
	dsa_pointer dp = InvalidDsaPointer;
	SiftShmIndexSlot *s;
	SiftShmIndexMeta m = *meta;

	if ((meta->state_flags & SIFT_SHM_SLOT_PRIVATE) != 0 || !sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
		return false;
	if (spec != NULL && (a = sift_shm_area_attach(t, true)) != NULL)
		dp = sift_shm_snap_build(t, a, &meta->locator, spec);
	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = slot_claim(t, &meta->locator);
	if (slot_refuses(s, meta))
	{
		LWLockRelease(&t->claim_lock);
		if (dp != InvalidDsaPointer)
			sift_shm_snap_free(a, dsa_get_address(a, dp));
		return false;
	}
	slot_adopt(s, &meta->locator);
	PG_TRY();
	{
		slot_drop_snap(t, s);
	}
	PG_CATCH();
	{
		if (dp != InvalidDsaPointer)
			sift_shm_snap_free(a, dsa_get_address(a, dp));
		PG_RE_THROW();
	}
	PG_END_TRY();
	m.state_flags = SIFT_SHM_SLOT_VALID | (dp != InvalidDsaPointer ? SIFT_SHM_SLOT_HAS_SNAP : 0);
	sift_shm_meta_write(s, &m);
	pg_atomic_write_u64(&s->last_used_tick, pg_atomic_fetch_add_u64(&t->clock, 1));
	if (dp != InvalidDsaPointer)
		sift_shm_slot_swap_snap(s, dp);
	LWLockRelease(&t->claim_lock);
	return spec == NULL || dp != InvalidDsaPointer;
}

void
sift_shm_manifest_invalidate(const RelFileLocator *locator, bool quarantine)
{
	Table	   *t;
	SiftShmIndexSlot *s;

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
		sift_shm_meta_write(s, &m);
		slot_drop_snap(t, s);
	}
	LWLockRelease(&t->claim_lock);
}
