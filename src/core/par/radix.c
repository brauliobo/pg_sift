/*
 * core/par/radix.c - the partition streams of core/par/radix.h.
 */
#include "core/par/radix.h"

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#define BLOCK_FIRST		4096

void
sift_radix_init(SiftRadixOut *o, SiftArena *a)
{
	memset(o, 0, sizeof(*o));
	o->arena = a;
}

/* len bytes at the end of the stream of p, in a block of the arena (a new one when the tail has no room) */
static void *
stream_reserve(SiftRadixOut *o, uint32 p, uint32 len)
{
	SiftRadixStream *st = &o->st[p];
	SiftRadixBlock *b = st->tail;

	if (b == NULL || b->cap - b->used < len)
	{
		const uint32 cap = Max(len, b == NULL ? BLOCK_FIRST : Min(SIFT_RADIX_BLOCK_MAX, b->cap * 2));
		SiftRadixBlock *nb = sift_arena_alloc(o->arena, sizeof(SiftRadixBlock) + cap, SIFT_ARENA_ALIGN);

		if (nb == NULL)
			return NULL;
		nb->next = NULL;
		nb->used = 0;
		nb->cap = cap;
		if (b == NULL)
			st->head = nb;
		else
			b->next = nb;
		st->tail = b = nb;
	}
	b->used += len;
	return (uint8 *) (b + 1) + b->used - len;
}

/* A full line of a stream, past the caches where the host has the instruction: the threads of phase 2 read it from other cores, long after it was written */
static inline void
line_copy(void *dst, const void *src)
{
#ifdef __SSE2__
	if (((uintptr_t) dst & 15) == 0)
	{
		for (int i = 0; i < SIFT_RADIX_LINE / 16; i++)
			_mm_stream_si128((__m128i *) dst + i, _mm_load_si128((const __m128i *) src + i));
		return;
	}
#endif
	memcpy(dst, src, SIFT_RADIX_LINE);
}

void
sift_radix_publish(void)
{
#ifdef __SSE2__
	_mm_sfence();
#endif
}

void *
sift_radix_reserve_flush(SiftRadixOut *o, uint32 p, uint32 len)
{
	const uint32 fill = o->fill[p];
	void	   *dst;

	if (fill > 0)
	{
		dst = stream_reserve(o, p, fill);
		if (dst == NULL)
			return NULL;
		if (fill == SIFT_RADIX_LINE)
			line_copy(dst, o->line[p]);
		else
			memcpy(dst, o->line[p], fill);
		o->fill[p] = 0;
	}
	if (len > SIFT_RADIX_LINE)
	{
		dst = stream_reserve(o, p, len);
		o->st[p].nrec += dst != NULL;
		return dst;
	}
	o->fill[p] = len;
	o->st[p].nrec++;
	return o->line[p];
}

bool
sift_radix_scan(const SiftRadixOut *o, uint32 p, SiftRadixChunkFn fn, void *arg)
{
	for (const SiftRadixBlock *b = o->st[p].head; b != NULL; b = b->next)
		if (b->used > 0 && !fn(arg, (const uint8 *) (b + 1), b->used))
			return false;
	return o->fill[p] == 0 || fn(arg, o->line[p], o->fill[p]);
}
