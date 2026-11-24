/*
 * core/par/radix.c - the partition streams of core/par/radix.h.
 */
#include "core/par/radix.h"

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
