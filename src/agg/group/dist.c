/*
 * agg/group/dist.c - the set of the distinct values of agg/group/dist.h.
 */
#include "core/host.h"

#include "agg/group/dist.h"
#include "agg/group/ht.h"
#include "agg/kern6.h"

#define DIST_MIN_CELLS	16
/* MEM[MEM-AGG-07]: DSet distinct string arena (a set starts at DIST_MIN_ARENA=128B and doubles to a chunk of DIST_CHUNK_MAX=512MiB, then adds a chunk; no huge allocation; was 4096B for every group and one huge array) */
#define DIST_MIN_ARENA	128
#define DIST_CHUNK_BITS	30
#define DIST_CHUNK_MAX	((uint64) 512 << 20)

typedef struct DistEntry
{
	uint64		hash;
	uint32		len;
	uint32		pad;
	uint8		bytes[FLEXIBLE_ARRAY_MEMBER];
} DistEntry;

SiftDistSet *
sift_dist_new(void)
{
	SiftDistSet *s = palloc0(sizeof(SiftDistSet));

	s->cxt = CurrentMemoryContext;
	s->cap = DIST_MIN_CELLS;
	s->cells = palloc0(sizeof(uint64) * s->cap);
	return s;
}

void
sift_dist_free(SiftDistSet *s)
{
	for (uint32 i = 0; i < s->nchunk; i++)
		pfree(s->chunk[i]);
	if (s->chunk)
		pfree(s->chunk);
	pfree(s->cells);
	pfree(s);
}

uint64
sift_dist_bytes(const SiftDistSet *s)
{
	return sizeof(SiftDistSet) + (uint64) s->cap * sizeof(uint64) + s->arena_bytes;
}

static inline const DistEntry *
entry_at(const SiftDistSet *s, uint64 cell)
{
	const uint64 at = cell - 1;

	return (const DistEntry *) (s->chunk[at >> DIST_CHUNK_BITS] + (at & (((uint64) 1 << DIST_CHUNK_BITS) - 1)));
}

static inline uint32
home_of(const SiftDistSet *s, uint64 cell_value, uint32 mask)
{
	return (uint32) (s->wide ? entry_at(s, cell_value)->hash : sift_aggk6_hash1(cell_value)) & mask;
}

static void
dist_resize(SiftDistSet *s, uint32 cap)
{
	const uint32 mask = cap - 1;
	uint64	   *cells = MemoryContextAllocZero(s->cxt, sizeof(uint64) * cap);

	for (uint32 i = 0; i < s->cap; i++)
	{
		uint32		at;

		if (s->cells[i] == 0)
			continue;
		for (at = home_of(s, s->cells[i], mask); cells[at] != 0; at = (at + 1) & mask)
			;
		cells[at] = s->cells[i];
	}
	pfree(s->cells);
	s->cells = cells;
	s->cap = cap;
}

static void
dist_grow(SiftDistSet *s)
{
	dist_resize(s, s->cap << 1);
}

static inline void
dist_reserve(SiftDistSet *s)
{
	if ((uint64) (s->used + 1) * 4 > (uint64) s->cap * 3)
		dist_grow(s);
}

bool
sift_dist_add(SiftDistSet *s, uint64 v)
{
	uint32		mask,
				i;

	Assert(!s->wide);
	if (v == 0)
	{
		const bool	fresh = !s->has_zero;

		s->has_zero = true;
		return fresh;
	}
	dist_reserve(s);
	mask = s->cap - 1;
	for (i = (uint32) sift_aggk6_hash1(v) & mask; s->cells[i] != 0; i = (i + 1) & mask)
		if (s->cells[i] == v)
			return false;
	s->cells[i] = v;
	s->used++;
	return true;
}

/* room for size bytes at the end of the last chunk: it doubles up to DIST_CHUNK_MAX, then the entry starts a chunk */
static void
arena_room(SiftDistSet *s, uint64 size)
{
	uint64		cap;

	if (s->nchunk > 0 && s->arena_used + size <= s->arena_cap)
		return;
	if (s->nchunk > 0 && s->arena_used + size <= DIST_CHUNK_MAX)
	{
		cap = Max(s->arena_cap, DIST_MIN_ARENA);
		while (cap < s->arena_used + size)
			cap <<= 1;
		cap = Min(cap, Max(DIST_CHUNK_MAX, s->arena_used + size));
		s->chunk[s->nchunk - 1] = repalloc(s->chunk[s->nchunk - 1], cap);
		s->arena_bytes += cap - s->arena_cap;
		s->arena_cap = cap;
		return;
	}
	cap = Max(size, DIST_MIN_ARENA);
	if (s->nchunk == s->chunk_cap)
	{
		s->chunk_cap = s->chunk_cap ? s->chunk_cap * 2 : 8;
		SIFT_REGROW_IN(s->cxt, s->chunk, s->chunk_cap * sizeof(uint8 *));
	}
	s->chunk[s->nchunk++] = MemoryContextAlloc(s->cxt, cap);
	s->arena_used = 0;
	s->arena_cap = cap;
	s->arena_bytes += cap;
}

static uint64
arena_put(SiftDistSet *s, uint64 hash, const void *bytes, uint32 len)
{
	const uint64 size = TYPEALIGN(8, offsetof(DistEntry, bytes) + (uint64) len);
	uint64		at;
	DistEntry  *e;

	arena_room(s, size);
	at = ((uint64) (s->nchunk - 1) << DIST_CHUNK_BITS) + s->arena_used;
	e = (DistEntry *) (s->chunk[s->nchunk - 1] + s->arena_used);
	e->hash = hash;
	e->len = len;
	e->pad = 0;
	memcpy(e->bytes, bytes, len);
	memset(e->bytes + len, 0, size - offsetof(DistEntry, bytes) - len);
	s->arena_used += size;
	return at;
}

bool
sift_dist_add_hashed(SiftDistSet *s, uint64 hash, const void *bytes, uint32 len)
{
	uint32		mask,
				i;

	Assert(s->wide || (s->used == 0 && !s->has_zero));
	s->wide = true;
	dist_reserve(s);
	mask = s->cap - 1;
	for (i = (uint32) hash & mask; s->cells[i] != 0; i = (i + 1) & mask)
	{
		const DistEntry *e = entry_at(s, s->cells[i]);

		if (e->hash == hash && e->len == len && memcmp(e->bytes, bytes, len) == 0)
			return false;
	}
	s->cells[i] = arena_put(s, hash, bytes, len) + 1;
	s->used++;
	return true;
}

bool
sift_dist_add_bytes(SiftDistSet *s, const void *bytes, uint32 len)
{
	return sift_dist_add_hashed(s, sift_hash_bytes(bytes, len), bytes, len);
}

uint64
sift_dist_count(const SiftDistSet *s)
{
	return (uint64) s->used + s->has_zero;
}

bool
sift_dist_next(const SiftDistSet *s, uint32 *pos, uint64 *v)
{
	Assert(!s->wide);
	for (; *pos < s->cap; (*pos)++)
		if (s->cells[*pos] != 0)
		{
			*v = s->cells[(*pos)++];
			return true;
		}
	if (*pos == s->cap)
	{
		(*pos)++;
		if (s->has_zero)
		{
			*v = 0;
			return true;
		}
	}
	return false;
}

static const DistEntry *
entry_next(const SiftDistSet *s, uint32 *pos)
{
	Assert(s->wide);
	for (; *pos < s->cap; (*pos)++)
		if (s->cells[*pos] != 0)
			return entry_at(s, s->cells[(*pos)++]);
	return NULL;
}

bool
sift_dist_next_bytes(const SiftDistSet *s, uint32 *pos, const uint8 **bytes, uint32 *len)
{
	const DistEntry *e = entry_next(s, pos);

	if (e == NULL)
		return false;
	*bytes = e->bytes;
	*len = e->len;
	return true;
}

void
sift_dist_merge(SiftDistSet *dst, const SiftDistSet *src)
{
	const uint64 want = ((uint64) dst->used + src->used) * 4 / 3 + 1;
	uint32		pos = 0;

	if (want > dst->cap && want <= UINT32_MAX / 2)
		dist_resize(dst, (uint32) pg_nextpower2_64(want));

	if (src->wide)
	{
		const DistEntry *e;

		while ((e = entry_next(src, &pos)) != NULL)
			sift_dist_add_hashed(dst, e->hash, e->bytes, e->len);
	}
	else
	{
		uint64		v;

		while (sift_dist_next(src, &pos, &v))
			sift_dist_add(dst, v);
	}
}
