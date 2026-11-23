/*
 * agg/vk/cache.c - the cache of decoded column chunks that stays on the device between queries
 * One device buffer cut in 4 MiB slabs. A decoded chunk (a column of a granule: its values and its validity) is bump-allocated inside a
 * slab and found by its key; a slab is evicted whole, least recently used first, never while a batch that uses it is in flight, and its
 * entries leave the table with it. The table is linear probing with deletion by shifting the entries after the hole back, so it holds no
 * tombstone and no entry of an evicted slab: a miss ends at the first empty cell, however many slabs went through the cache. The entries
 * of a slab are one list through the table (slab_prev and slab_next), which is what makes an eviction cost the entries of the slab. A part
 * is immutable, so an entry is never stale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

#define ENT_PER_SLAB	1024
#define ENT_START_SLAB	128
#define Min(a, b)		((a) < (b) ? (a) : (b))

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
	for (uint32_t i = 0; vk->slab && i < n; i++)
		vk->slab[i].ent_head = VK_ENT_NONE;
	/* MEM[MEM-VK-08]: vk ent host table (starts at pow2 of nslab*128*2 entries of 80B, doubles when 3/4 full up to nslab*1024*2; was always the largest: 5.2MiB at 256MiB, 84MiB at 4GiB) */
	vk->nent_max = 4096;
	while ((uint64_t) vk->nent_max < (uint64_t) n * ENT_PER_SLAB * 2)
		vk->nent_max <<= 1;
	vk->nent = 4096;
	while ((uint64_t) vk->nent < (uint64_t) n * ENT_START_SLAB * 2)
		vk->nent <<= 1;
	vk->used = 0;
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
	const uint32_t h = (uint32_t) key_hash(k),
				mask = vk->nent - 1;

	for (uint32_t i = h & mask;; i = (i + 1) & mask)
	{
		Ent		   *e = &vk->ent[i];

		if (!e->used)
			return NULL;
		if (e->hash == h && key_eq(&e->key, k))
			return e;
	}
}

void
cache_touch(SiftVk *vk, Ent *e)
{
	Slab	   *s = &vk->slab[e->slab];

	s->stamp = ++vk->clock;
	s->pin = vk->batch;
}

/* the entry at idx joins the list of the entries of its slab */
static void
slab_link(SiftVk *vk, uint32_t idx)
{
	Ent		   *e = &vk->ent[idx];
	Slab	   *s = &vk->slab[e->slab];

	e->slab_prev = VK_ENT_NONE;
	e->slab_next = s->ent_head;
	if (s->ent_head != VK_ENT_NONE)
		vk->ent[s->ent_head].slab_prev = idx;
	s->ent_head = idx;
	s->ent_count++;
}

static void
slab_unlink(SiftVk *vk, uint32_t idx)
{
	const Ent  *e = &vk->ent[idx];
	Slab	   *s = &vk->slab[e->slab];

	if (e->slab_prev != VK_ENT_NONE)
		vk->ent[e->slab_prev].slab_next = e->slab_next;
	else
		s->ent_head = e->slab_next;
	if (e->slab_next != VK_ENT_NONE)
		vk->ent[e->slab_next].slab_prev = e->slab_prev;
	s->ent_count--;
}

/* the entry that was at from is at to now: its neighbours in the list of its slab say so */
static void
slab_moved(SiftVk *vk, uint32_t from, uint32_t to)
{
	const Ent  *e = &vk->ent[to];

	if (e->slab_prev != VK_ENT_NONE)
		vk->ent[e->slab_prev].slab_next = to;
	else
		vk->slab[e->slab].ent_head = to;
	if (e->slab_next != VK_ENT_NONE)
		vk->ent[e->slab_next].slab_prev = to;
	(void) from;
}

/* The entry at hole leaves the table: each entry after it in its run that its home lies at or before the hole for moves back into it, so that no entry has an empty cell between its home and itself */
static void
cache_delete_slot(SiftVk *vk, uint32_t hole)
{
	const uint32_t mask = vk->nent - 1;
	uint32_t	cur = hole;

	slab_unlink(vk, hole);
	vk->used--;
	for (;;)
	{
		const Ent  *e;

		cur = (cur + 1) & mask;
		e = &vk->ent[cur];
		if (!e->used)
			break;
		if (((cur - e->hash) & mask) >= ((cur - hole) & mask))
		{
			vk->ent[hole] = *e;
			slab_moved(vk, cur, hole);
			hole = cur;
		}
	}
	vk->ent[hole].used = 0;
}

void
cache_remove(SiftVk *vk, Ent *e)
{
	cache_delete_slot(vk, (uint32_t) (e - vk->ent));
}

void
cache_remove_key(SiftVk *vk, const SiftVkKey *k)
{
	Ent		   *e = cache_find(vk, k);

	if (e)
		cache_remove(vk, e);
}

/* a slab that no entry names and no byte of is in use */
static void
slab_evict(SiftVk *vk, int64_t si)
{
	Slab	   *s = &vk->slab[si];

	while (s->ent_head != VK_ENT_NONE)
		cache_delete_slot(vk, s->ent_head);
	s->used = 0;
	s->gen++;
	vk->evict_gen++;
}

/* the slab no batch in flight uses that was used least recently, or -1 */
static int64_t
slab_lru(const SiftVk *vk)
{
	int64_t		best = -1;

	for (uint32_t i = 0; i < vk->nslab; i++)
	{
		const Slab *s = &vk->slab[i];

		if (s->pin <= vk->done_batch && (best < 0 || s->stamp < vk->slab[best].stamp))
			best = (int64_t) i;
	}
	return best;
}

/* a slab with room for words: the current one, the fullest other one the words fit in (a slab keeps its tail for the chunks that fit it), an empty one, or the least recently used that no batch in flight uses (evicted) */
/* MEM[MEM-VK-06]: slab_for bump allocator (current slab, then the best fit among the tails of the slabs, then an empty one, then one eviction; bumps only that slab's gen) */
static int64_t
slab_for(SiftVk *vk, uint32_t words)
{
	int64_t		best = -1,
				fit = -1;

	if (vk->cur >= 0 && vk->slab[vk->cur].used + words <= SLAB_WORDS)
		return vk->cur;
	for (uint32_t i = 0; i < vk->nslab; i++)
	{
		const Slab *s = &vk->slab[i];

		if (s->used > 0 && s->used + words <= SLAB_WORDS && (fit < 0 || s->used > vk->slab[fit].used))
			fit = (int64_t) i;
	}
	if (fit >= 0)
		return fit;
	for (uint32_t i = 0; i < vk->nslab; i++)
	{
		const Slab *s = &vk->slab[i];

		if (s->used == 0 && s->pin <= vk->done_batch)
			return (int64_t) i;
	}
	if ((best = slab_lru(vk)) >= 0)
		slab_evict(vk, best);
	return best;
}

/* the table twice as large: every entry moves to its place in it, and the lists of the entries of the slabs are made again */
static bool
ent_grow(SiftVk *vk)
{
	const uint32_t n = vk->nent * 2,
				mask = n - 1;
	Ent		   *old = vk->ent,
			   *grown = calloc(n, sizeof(Ent));

	if (!grown)
		return false;
	for (uint32_t i = 0; i < vk->nslab; i++)
	{
		vk->slab[i].ent_head = VK_ENT_NONE;
		vk->slab[i].ent_count = 0;
	}
	vk->ent = grown;
	for (uint32_t i = 0; i < vk->nent; i++)
	{
		uint32_t	j;

		if (!old[i].used)
			continue;
		for (j = old[i].hash & mask; grown[j].used; j = (j + 1) & mask)
			;
		grown[j] = old[i];
		slab_link(vk, j);
	}
	account(vk, (int64_t) (n - vk->nent) * (int64_t) sizeof(Ent));
	vk->nent = n;
	free(old);
	return true;
}

/* an entry for key with a region of words (an even number) in the cache; NULL when every slab is in use by a batch in flight */
Ent *
cache_insert(SiftVk *vk, const SiftVkKey *k, uint32_t words, uint32_t *woff)
{
	int64_t		si;
	uint32_t	h = (uint32_t) key_hash(k),
				mask = vk->nent - 1,
				i;
	Slab	   *s;
	Ent		   *e;

	while ((uint64_t) vk->used * 4 >= (uint64_t) vk->nent * 3)
	{
		int64_t		lru;

		if (vk->nent < vk->nent_max && ent_grow(vk))
		{
			mask = vk->nent - 1;
			continue;
		}
		if ((lru = slab_lru(vk)) < 0)
			return NULL;
		slab_evict(vk, lru);
	}
	if ((si = slab_for(vk, words)) < 0)
		return NULL;
	vk->cur = si;
	s = &vk->slab[si];
	for (i = h & mask; vk->ent[i].used; i = (i + 1) & mask)
		;
	e = &vk->ent[i];
	*woff = (uint32_t) si * SLAB_WORDS + s->used;
	s->used += words;
	s->stamp = ++vk->clock;
	s->pin = vk->batch;
	memset(e, 0, sizeof(*e));
	e->key = *k;
	e->hash = h;
	e->slab = (uint32_t) si;
	e->used = 1;
	vk->used++;
	slab_link(vk, i);
	return e;
}

double
sift_vk_resident_share(SiftVk *vk, const SiftVkKey *keys, uint32_t n)
{
	uint8_t		res[256];
	uint32_t	hit = 0;

	for (uint32_t base = 0; base < n; base += sizeof(res))
		hit += sift_vk_resident_batch(vk, keys + base, Min(n - base, (uint32_t) sizeof(res)), res);
	return n ? (double) hit / n : 0.0;
}

/* Every chunk the job left out (bytes NULL) is on the device */
bool
job_resident(SiftVk *vk, const SiftVkJob *job)
{
	for (uint32_t g = 0; g < job->ngran; g++)
		for (uint32_t c = 0; c < job->ncols; c++)
		{
			const SiftVkChunk *ch = job->gran[g].col[c];

			if (ch != NULL && ch->bytes == NULL && cache_find(vk, &ch->key) == NULL)
				return false;
		}
	return true;
}

uint32_t
sift_vk_resident_batch(SiftVk *vk, const SiftVkKey *keys, uint32_t n, uint8_t *resident)
{
	enum { AHEAD = 8 };
	uint32_t	hit = 0;

	if (vk->released)
		return (uint32_t) (memset(resident, 0, n), 0);
	for (uint32_t i = 0; i < n; i++)
	{
		if (i + AHEAD < n)
			__builtin_prefetch(&vk->ent[(uint32_t) key_hash(&keys[i + AHEAD]) & (vk->nent - 1)]);
		resident[i] = cache_find(vk, &keys[i]) != NULL;
		hit += resident[i];
	}
	return hit;
}

/* the cache and its tables given back: the arena, the slabs and the entries */
static void
release_cache(SiftVk *vk)
{
	if (vk->slab || vk->ent)
		account(vk, -((int64_t) vk->nslab * (int64_t) sizeof(Slab) + (int64_t) vk->nent * (int64_t) sizeof(Ent)));
	buf_free(vk, &vk->arena);
	free(vk->slab);
	free(vk->ent);
	vk->slab = NULL;
	vk->ent = NULL;
	vk->nslab = vk->nent = vk->used = 0;
	vk->cur = -1;
	vk->caps.cache_bytes = 0;
}

/* the cache made again at bytes, empty: no batch is in flight when this is called */
static bool
resize_cache(SiftVk *vk, uint64_t bytes)
{
	free_tabs(vk);
	vk->evict_gen++;
	release_cache(vk);
	return make_cache(vk, bytes) && bind_buffers(vk);
}

/* MEM[MEM-VK-07]: sift_vk_set_cache resize (the old arena is freed before the new one is sized and made: one arena of VRAM at a time, was old + new) */
bool
sift_vk_set_cache(SiftVk *vk, uint64_t bytes)
{
	const uint64_t was = vk->cache_want;

	vk->cache_want = bytes;
	if (vk->released)
		return true;
	if (vk->inflight && !settle_all(vk))
		return false;
	if (resize_cache(vk, bytes))
		return true;
	release_cache(vk);
	vk->cache_want = was;
	if (!make_cache(vk, was) || !bind_buffers(vk))
		vk->released = true;
	return false;
}

void
sift_vk_set_envelope(SiftVk *vk, uint64_t envelope, uint64_t fixed)
{
	vk->envelope = envelope;
	vk->fixed = fixed;
}

/*
 * The cache sized for a job whose hash table and result take `tables` bytes: the ceiling the device was asked for (caps.cache_bytes), or what the envelope leaves of it, never less than SIFT_VK_CACHE_MIN.
 * The arena is one buffer, so the cache that stays is its whole content least the slabs given back: the chunks of the job are sent again, and the next job whose table is smaller makes the cache the ceiling again.
 */
bool
sift_vk_fit_cache(SiftVk *vk, uint64_t tables)
{
	const uint64_t ceiling = vk->caps.cache_bytes,
				room = vk->envelope > vk->fixed + tables ? vk->envelope - vk->fixed - tables : 0;
	uint64_t	want = room < ceiling ? room : ceiling;
	uint32_t	n;

	if (vk->envelope == 0 || vk->released)
		return true;
	if (want < SIFT_VK_CACHE_MIN)
		want = Min(SIFT_VK_CACHE_MIN, ceiling);
	n = (uint32_t) (want / SLAB_BYTES);
	if (n == vk->nslab)
		return true;
	if (resize_cache(vk, (uint64_t) n * SLAB_BYTES))
	{
		vk->caps.cache_bytes = ceiling;
		return true;
	}
	release_cache(vk);
	if (!make_cache(vk, vk->cache_want) || !bind_buffers(vk))
		vk->released = true;
	return false;
}

/* the buffers of a device that has had no job for a while given back, and with cache its cache too, the pipelines and the device kept: the next job makes them again */
bool
sift_vk_release(SiftVk *vk, bool cache)
{
	if (!vk)
		return true;
	if (vk->inflight && !settle_all(vk))
		return false;
	slots_release(vk);
	vk->trimmed = true;
	if (cache && !vk->released)
	{
		free_tabs(vk);
		vk->evict_gen++;
		release_cache(vk);
		vk->released = true;
	}
	return true;
}

bool
revive(SiftVk *vk)
{
	if (vk->released && !make_cache(vk, vk->cache_want))
		return false;
	if (!slots_alloc(vk))
	{
		slots_release(vk);
		release_cache(vk);
		vk->released = true;
		return false;
	}
	vk->released = vk->trimmed = false;
	return true;
}

#endif							/* SIFT_VULKAN */
