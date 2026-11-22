/*
 * agg/vk/cache.c - the cache of decoded column chunks that stays on the device between queries
 * One device buffer cut in 4 MiB slabs. A decoded chunk (a column of a granule: its values and its validity) is bump-allocated inside a
 * slab and found by its key; a slab is evicted whole, least recently used first, never while a batch that uses it is in flight: the
 * epoch of the slab is bumped and every entry of the older epoch is gone at once. A part is immutable, so an entry is never stale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

#define ENT_PER_SLAB	1024

static uint64_t
key_hash(const SiftVkKey *k)
{
	const uint64_t *w = (const uint64_t *) k;
	uint64_t	h = 0x9E3779B97F4A7C15ull;

	for (size_t i = 0; i < sizeof(*k) / 8; i++)
	{
		h ^= w[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
		h *= 0xff51afd7ed558ccdull;
		h ^= h >> 33;
	}
	return h;
}

static bool
key_eq(const SiftVkKey *a, const SiftVkKey *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static bool
ent_live(const SiftVk *vk, const Ent *e)
{
	return e->state == 1 && vk->slab[e->slab].epoch == e->epoch;
}

bool
make_cache(SiftVk *vk, uint64_t bytes)
{
	uint64_t	budget = heap_budget_free(vk) / 4 * 3,
				lim = vk->props.limits.maxStorageBufferRange;
	uint64_t	want = bytes;
	uint32_t	n;

	if (lim > ((uint64_t) 4 << 30) - SLAB_BYTES)
		lim = ((uint64_t) 4 << 30) - SLAB_BYTES;
	if (want > budget)
		want = budget;
	if (want > lim)
		want = lim;
	n = (uint32_t) (want / SLAB_BYTES);
	while (n >= MIN_SLABS)
	{
		if (buf_new(vk, &vk->arena, (uint64_t) n * SLAB_BYTES,
					VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, K_DEVICE))
			break;
		n /= 2;
	}
	if (n < MIN_SLABS)
	{
		snprintf(vk->err, sizeof(vk->err), "\"%.100s\" has no room for a cache of %u MiB", vk->props.deviceName, (unsigned) (bytes >> 20));
		return false;
	}
	vk->nslab = n;
	vk->slab = calloc(n, sizeof(Slab));
	vk->nent = 4096;
	while ((uint64_t) vk->nent < (uint64_t) n * ENT_PER_SLAB)
		vk->nent <<= 1;
	vk->ent = calloc(vk->nent, sizeof(Ent));
	if (!vk->slab || !vk->ent)
	{
		snprintf(vk->err, sizeof(vk->err), "out of memory");
		return false;
	}
	account(vk, (int64_t) n * (int64_t) sizeof(Slab) + (int64_t) vk->nent * (int64_t) sizeof(Ent));
	vk->cur = -1;
	vk->clock = 1;
	vk->caps.cache_bytes = (uint64_t) n * SLAB_BYTES;
	return true;
}

Ent *
cache_find(SiftVk *vk, const SiftVkKey *k)
{
	uint32_t	mask = vk->nent - 1,
				i = (uint32_t) key_hash(k) & mask;

	for (uint32_t n = 0; n < vk->nent; n++, i = (i + 1) & mask)
	{
		Ent		   *e = &vk->ent[i];

		if (e->state == 0)
			return NULL;
		if (ent_live(vk, e) && key_eq(&e->key, k))
			return e;
	}
	return NULL;
}

void
cache_touch(SiftVk *vk, Ent *e)
{
	Slab	   *s = &vk->slab[e->slab];

	s->stamp = ++vk->clock;
	s->pin = vk->batch;
}

void
cache_remove(SiftVk *vk, Ent *e)
{
	e->state = 2;
}

/* a slab with room for words: the current one, an empty one, or the least recently used that no batch in flight uses (evicted) */
static int64_t
slab_for(SiftVk *vk, uint32_t words)
{
	int64_t		best = -1;

	if (vk->cur >= 0 && vk->slab[vk->cur].used + words <= SLAB_WORDS)
		return vk->cur;
	for (uint32_t i = 0; i < vk->nslab; i++)
	{
		Slab	   *s = &vk->slab[i];

		if (s->used == 0 && s->pin <= vk->done_batch)
			return (int64_t) i;
		if (s->pin <= vk->done_batch && (best < 0 || s->stamp < vk->slab[best].stamp))
			best = (int64_t) i;
	}
	if (best >= 0)
	{
		vk->slab[best].epoch++;
		vk->slab[best].used = 0;
		vk->evict_gen++;
	}
	return best;
}

/* an entry for key with a region of words (an even number) in the cache; NULL when every slab is in use by a batch in flight */
Ent *
cache_insert(SiftVk *vk, const SiftVkKey *k, uint32_t words, uint32_t *woff)
{
	int64_t		si = slab_for(vk, words);
	uint32_t	mask = vk->nent - 1,
				i = (uint32_t) key_hash(k) & mask;
	Slab	   *s;
	Ent		   *e = NULL;

	if (si < 0)
		return NULL;
	vk->cur = si;
	s = &vk->slab[si];
	for (uint32_t n = 0; n < vk->nent; n++, i = (i + 1) & mask)
		if (vk->ent[i].state != 1 || !ent_live(vk, &vk->ent[i]))
		{
			e = &vk->ent[i];
			break;
		}
	if (!e)
		return NULL;
	*woff = (uint32_t) si * SLAB_WORDS + s->used;
	s->used += words;
	s->stamp = ++vk->clock;
	s->pin = vk->batch;
	memset(e, 0, sizeof(*e));
	e->key = *k;
	e->slab = (uint32_t) si;
	e->epoch = s->epoch;
	e->state = 1;
	return e;
}

double
sift_vk_resident_share(SiftVk *vk, const SiftVkKey *keys, uint32_t n)
{
	uint32_t	hit = 0;

	for (uint32_t i = 0; i < n; i++)
		hit += cache_find(vk, &keys[i]) != NULL;
	return n ? (double) hit / n : 0.0;
}

bool
sift_vk_set_cache(SiftVk *vk, uint64_t bytes)
{
	Buf			old = vk->arena;
	Slab	   *oslab = vk->slab;
	Ent		   *oent = vk->ent;
	uint32_t	on = vk->nslab,
				onent = vk->nent;
	int64_t		oacc = (int64_t) on * (int64_t) sizeof(Slab) + (int64_t) onent * (int64_t) sizeof(Ent);

	if (vk->inflight && !settle_all(vk))
		return false;
	free_tabs(vk);
	vk->evict_gen++;
	memset(&vk->arena, 0, sizeof(vk->arena));
	vk->slab = NULL;
	vk->ent = NULL;
	account(vk, -oacc);
	if (!make_cache(vk, bytes) || !bind_buffers(vk))
	{
		buf_free(vk, &vk->arena);
		free(vk->slab);
		free(vk->ent);
		vk->arena = old;
		vk->slab = oslab;
		vk->ent = oent;
		vk->nslab = on;
		vk->nent = onent;
		account(vk, oacc);
		bind_buffers(vk);
		return false;
	}
	buf_free(vk, &old);
	free(oslab);
	free(oent);
	return true;
}

#endif							/* SIFT_VULKAN */
