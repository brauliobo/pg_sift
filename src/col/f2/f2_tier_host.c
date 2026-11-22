/*
 * col/f2/f2_tier_host.c - a part in object storage as the readers of col/f2/part_format.c see it: its header image and its stream, and what a commit that changes the parts of a partition holds
 * and releases.
 */
#include "postgres.h"

#include "storage/lmgr.h"
#include "utils/rel.h"

#include "col/f2/f2_tier_host.h"
#include "col/f2/part_format.h"
#include "core/damage.h"
#include "core/part_lock.h"

void
sift_tier_host_open(Relation rel, const SiftPartDesc *d, char **image, uint32 *image_len, SiftTierRemote **remote)
{
	int			n;
	const SiftTierPartRef *refs = sift_tier_refs(rel, &n);
	const SiftTierPartRef *ref = refs != NULL ? sift_tier_refs_find(refs, n, d->part_gen) : NULL;
	SiftTierPolicy policy;
	SiftTierErr err = {0};
	SiftTierStatus st;
	char		key[160];

	if (ref == NULL)
		SIFT_DAMAGE_DATA(rel, errmsg("sift index \"%s\": part %llu is in object storage and the tier directory has no ref for it", RelationGetRelationName(rel),
									 (unsigned long long) d->part_gen));
	sift_tier_policy_by_id(rel, ref->policy_id, &policy);
	sift_tier_object_key(key, sizeof(key), policy.scope_id, ref->part_id, ref->part_gen);
	st = sift_tier_remote_open(sift_tier_backend(&policy), sift_tier_cache(&policy), key, ref, remote, &err);
	if (st != SIFT_TIER_OK)
		sift_tier_raise(st, &err, "cannot open a part in object storage");
	*image = (char *) (*remote)->image;
	*image_len = (*remote)->hdr.header_len;
}

void
sift_tier_host_read(Relation rel, SiftTierRemote *remote, uint64 off, uint64 len, void *dst)
{
	SiftTierErr err = {0};
	SiftTierStatus st = sift_tier_remote_read(remote, off, len, dst, &err);

	if (st != SIFT_TIER_OK)
		sift_tier_raise(st, &err, "cannot read a part in object storage");
}

bool
sift_tier_lock_compaction(Relation rel, uint64 part_id)
{
	return ConditionalLockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
}

void
sift_tier_unlock_compaction(Relation rel, uint64 part_id)
{
	UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
}

void
sift_tier_lock_part(Relation rel, uint64 part_id)
{
	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
}

void
sift_tier_unlock_part(Relation rel, uint64 part_id)
{
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
}

const SiftPartDesc *
sift_tier_find_part(const SiftPartDesc *parts, int nparts, uint64 gen)
{
	for (int i = 0; i < nparts; i++)
		if (parts[i].part_gen == gen)
			return &parts[i];
	return NULL;
}

static SiftExtent *
extents_add(SiftExtent *v, int *n, int *cap, SiftExtent e)
{
	if (*n == *cap)
	{
		*cap = *cap == 0 ? 16 : *cap * 2;
		v = v == NULL ? palloc(*cap * sizeof(SiftExtent)) : repalloc(v, *cap * sizeof(SiftExtent));
	}
	v[(*n)++] = e;
	return v;
}

/* The packed pages the dropped parts are slots of that no part which stays is a slot of: each once */
static SiftExtent *
packed_pages_add(Relation rel, uint64 part_id, const SiftPartDesc *parts, int nparts, const uint64 *gens, int ngens, SiftExtent *v, int *n, int *cap)
{
	SiftPartRef *leaving = palloc(Max(ngens, 1) * sizeof(SiftPartRef));
	int			nleaving = 0;

	for (int i = 0; i < ngens; i++)
	{
		const SiftPartDesc *d = sift_tier_find_part(parts, nparts, gens[i]);

		if (d != NULL && (d->flags & SIFT_PART_FLAG_PACKED_SLOT))
			leaving[nleaving++] = (SiftPartRef) {(uint32) part_id, d->part_gen};
	}
	for (int i = 0; i < nleaving; i++)
	{
		const SiftPartDesc *d = sift_tier_find_part(parts, nparts, leaving[i].part_gen);
		bool		seen = false;

		for (int k = 0; k < i; k++)
			seen |= sift_tier_find_part(parts, nparts, leaving[k].part_gen)->header_blk == d->header_blk;
		if (!seen && sift_packed_unreferenced(rel, d->header_blk, leaving, nleaving))
			v = extents_add(v, n, cap, (SiftExtent) {d->header_blk, 1});
	}
	pfree(leaving);
	return v;
}

int
sift_tier_release_extents(Relation rel, uint64 part_id, const SiftPartDesc *parts, int nparts, const uint64 *gens, int ngens, bool dropped, SiftExtent **out)
{
	SiftExtent *v = NULL;
	int			n = 0,
				cap = 0;

	for (int i = 0; i < ngens; i++)
	{
		const SiftPartDesc *d = sift_tier_find_part(parts, nparts, gens[i]);

		if (d == NULL)
			continue;
		if (!(d->ext_flags & SIFT_PART_EXT_REMOTE) && !(d->flags & SIFT_PART_FLAG_PACKED_SLOT))
		{
			int			nb;
			SiftExtent *blocks = sift_part_blocks(sift_part_open_desc(rel, d, (uint32) part_id), &nb);

			for (int k = 0; k < nb; k++)
				v = extents_add(v, &n, &cap, blocks[k]);
		}
		if (dropped && d->dv_blk != InvalidBlockNumber)
			v = extents_add(v, &n, &cap, (SiftExtent) {d->dv_blk, d->dv_nblocks});
	}
	if (dropped)
		v = packed_pages_add(rel, part_id, parts, nparts, gens, ngens, v, &n, &cap);
	*out = v;
	return n;
}
