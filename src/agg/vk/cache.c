/*
 * agg/vk/cache.c - the cache of column chunks on the device
 * The entries (a hash by the chunk key), the slabs they live in, the staging of a chunk for upload, the abandon of a batch and the size of the cache.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

static uint32_t
key_hash(const SiftVkKey *k)
{
	uint64_t	h = k->rel * 0x9E3779B97F4A7C15ull;

	h ^= (k->gen + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
	h ^= ((uint64_t) k->unit << 20 | k->leaf) * 0x165667B19E3779F9ull;
	h ^= h >> 29;
	h *= 0xBF58476D1CE4E5B9ull;
	return (uint32_t) (h >> 32);
}

static bool
key_eq(const SiftVkKey *a, const SiftVkKey *b)
{
	return a->rel == b->rel && a->gen == b->gen && a->unit == b->unit && a->leaf == b->leaf;
}

static bool
ent_live(const SiftVk *vk, const Ent *e)
{
	return e->state == 1 && vk->slab[e->slab].epoch == e->epoch;
}

static Ent *
ent_find(SiftVk *vk, const SiftVkKey *k)
{
	uint32_t	h = key_hash(k) & (vk->nent - 1);

	for (uint32_t i = 0; i < vk->nent; i++, h = (h + 1) & (vk->nent - 1))
	{
		Ent		   *e = &vk->ent[h];

		if (e->state == 0)
			return NULL;
		if (e->state == 1 && key_eq(&e->key, k))
			return ent_live(vk, e) ? e : NULL;
	}
	return NULL;
}

static bool
ent_rebuild(SiftVk *vk, uint32_t nent)
{
	Ent		   *old = vk->ent;
	uint32_t	nold = vk->nent;
	Ent		   *n = calloc(nent, sizeof(Ent));

	if (!n)
		return false;
	vk->ent = n;
	vk->nent = nent;
	vk->used = 0;
	for (uint32_t i = 0; i < nold; i++)
		if (old && ent_live(vk, &old[i]))
		{
			uint32_t	h = key_hash(&old[i].key) & (nent - 1);

			while (n[h].state)
				h = (h + 1) & (nent - 1);
			n[h] = old[i];
			vk->used++;
		}
	free(old);
	return true;
}

static bool
ent_put(SiftVk *vk, const SiftVkKey *k, uint32_t slab, uint32_t woff)
{
	uint32_t	h;

	if ((vk->used + 1) * 2 > vk->nent)
	{
		/* stale entries (evicted slabs) go away on the way; the table doubles only when it is still half full of live ones */
		if (!ent_rebuild(vk, vk->nent))
			return false;
		if ((vk->used + 1) * 2 > vk->nent && !ent_rebuild(vk, vk->nent * 2))
			return false;
	}
	h = key_hash(k) & (vk->nent - 1);
	while (vk->ent[h].state == 1)
		h = (h + 1) & (vk->nent - 1);
	if (vk->ent[h].state == 0)
		vk->used++;
	vk->ent[h] = (Ent) {*k, slab, vk->slab[slab].epoch, woff, 1};
	return true;
}

static void
ent_drop(SiftVk *vk, const SiftVkKey *k)
{
	Ent		   *e = ent_find(vk, k);

	if (e)
		e->state = 2;
}

/* A slab with room for n words for the batch in preparation: the one being filled, an empty one, or the least recently used one that the batch does not use */
static int64_t
slab_for(SiftVk *vk, uint32_t n)
{
	int64_t		best = -1;

	if (vk->cur >= 0 && vk->slab[vk->cur].used + n <= SLAB_WORDS)
		return vk->cur;
	for (uint32_t i = 0; i < vk->nslab; i++)
	{
		const Slab *s = &vk->slab[i];

		if (s->used == 0)
		{
			best = i;
			break;
		}
		if (s->pin != vk->batch && (best < 0 || s->stamp < vk->slab[best].stamp))
			best = i;
	}
	if (best < 0)
		return -1;
	vk->slab[best].used = 0;
	vk->slab[best].epoch++;
	vk->cur = best;
	return best;
}

bool
sift_vk_lookup(SiftVk *vk, const SiftVkKey *k, uint32_t *woff)
{
	Ent		   *e = ent_find(vk, k);
	Slab	   *s;

	if (!e)
		return false;
	s = &vk->slab[e->slab];
	s->stamp = ++vk->clock;
	s->pin = vk->batch;
	*woff = e->slab * SLAB_WORDS + e->woff;
	return true;
}

bool
sift_vk_stage(SiftVk *vk, const SiftVkKey *k, const void *words, uint32_t nwords, uint32_t *woff)
{
	int64_t		s;
	uint64_t	bytes = (uint64_t) nwords * 4;

	if (vk->lost || nwords > SLAB_WORDS || !settle(vk))
		return false;
	if (vk->up + bytes > UP_CAP)
		return false;
	if (vk->ncopy == vk->capcopy)
	{
		uint32_t	n = vk->capcopy ? vk->capcopy * 2 : 256;
		Copy	   *c = realloc(vk->copy, n * sizeof(Copy));
		SiftVkKey  *p = realloc(vk->pend, n * sizeof(SiftVkKey));

		if (c)
			vk->copy = c;
		if (p)
			vk->pend = p;
		if (!c || !p)
			return false;
		vk->capcopy = n;
	}
	s = slab_for(vk, nwords);
	if (s < 0 || !ent_put(vk, k, (uint32_t) s, vk->slab[s].used))
		return false;
	vk->slab[s].stamp = ++vk->clock;
	vk->slab[s].pin = vk->batch;
	*woff = (uint32_t) s * SLAB_WORDS + vk->slab[s].used;
	memcpy((char *) vk->stage.map + vk->up, words, bytes);
	vk->copy[vk->ncopy++] = (Copy) {vk->up, (uint64_t) *woff * 4, bytes};
	vk->pend[vk->npend++] = *k;
	vk->up += (bytes + 15) & ~(uint64_t) 15;
	vk->slab[s].used += (nwords + 3) & ~3u;
	return true;
}

void
sift_vk_abandon(SiftVk *vk)
{
	for (uint32_t i = 0; i < vk->npend; i++)
		ent_drop(vk, &vk->pend[i]);
	vk->npend = vk->ncopy = 0;
	vk->up = 0;
	vk->batch++;
}

/* A cache of about `bytes` (halved until the device has room); the caller owns the previous one, which stays when this fails */
/* A cache of about `bytes` (halved until the device has room); the caller owns the previous one, which stays when this fails */
bool
make_cache(SiftVk *vk, uint64_t bytes)
{
	uint64_t	max = vk->props.limits.maxStorageBufferRange;

	if (max > 0xFFFFFFFFull * 4)
		max = 0xFFFFFFFFull * 4;
	if (bytes > max)
		bytes = max;
	if (bytes < (uint64_t) MIN_SLABS * SLAB_BYTES)
		bytes = (uint64_t) MIN_SLABS * SLAB_BYTES;
	vk->err[0] = '\0';
	for (uint32_t ns = (uint32_t) (bytes / SLAB_BYTES); ns >= MIN_SLABS; ns /= 2)
	{
		Slab	   *sl = calloc(ns, sizeof(Slab));
		VkDescriptorBufferInfo bi;
		VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vk->ds, .dstBinding = 0, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

		if (!sl)
		{
			snprintf(vk->err, sizeof(vk->err), "out of memory");
			return false;
		}
		if (!buf_new(vk, &vk->arena, (uint64_t) ns * SLAB_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, K_DEVICE))
		{
			free(sl);
			if (vk->lost)
				return false;
			continue;
		}
		bi = (VkDescriptorBufferInfo) {vk->arena.b, 0, VK_WHOLE_SIZE};
		w.pBufferInfo = &bi;
		vk->vkUpdateDescriptorSets(vk->dev, 1, &w, 0, NULL);
		vk->slab = sl;
		vk->nslab = ns;
		vk->cur = -1;
		free(vk->ent);
		vk->ent = NULL;
		vk->nent = vk->used = 0;
		if (!ent_rebuild(vk, 1024))
		{
			snprintf(vk->err, sizeof(vk->err), "out of memory");
			return false;
		}
		return true;
	}
	if (!vk->err[0])
		snprintf(vk->err, sizeof(vk->err), "no room for a cache of %llu MB on the device", (unsigned long long) (bytes >> 20));
	return false;
}

bool
sift_vk_set_cache(SiftVk *vk, uint64_t bytes)
{
	Buf			old = vk->arena;
	Slab	   *oslab = vk->slab;
	uint32_t	ons = vk->nslab;
	Ent		   *oent = vk->ent;
	uint32_t	onent = vk->nent,
				oused = vk->used;

	if (vk->lost || !settle(vk))
		return false;
	sift_vk_abandon(vk);
	vk->arena = (Buf) {0};
	vk->slab = NULL;
	vk->nslab = 0;
	if (!make_cache(vk, bytes))
	{
		/* make_cache touches the entry table only once the new buffer exists */
		vk->arena = old;
		vk->slab = oslab;
		vk->nslab = ons;
		if (!vk->ent)
		{
			vk->ent = oent;
			vk->nent = onent;
			vk->used = oused;
		}
		return false;
	}
	buf_free(vk, &old);
	free(oslab);
	return true;
}

#endif							/* SIFT_VULKAN */
