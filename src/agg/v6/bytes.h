/*
 * agg/v6/bytes.h - a growing byte buffer, the form the keys of v6_distinct.h (the columns of a COUNT(DISTINCT) row) and of v6_argext.h (the columns of an ORDER BY) are built in.
 */
#ifndef SIFT_AGG_V6_BYTES_H
#define SIFT_AGG_V6_BYTES_H

#include "core/host.h"

typedef struct SiftBytes
{
	MemoryContext cx;
	uint8	   *buf;
	uint32		len;
	uint32		cap;
} SiftBytes;

static inline void
sift_bytes_init(SiftBytes *b, MemoryContext cx)
{
	memset(b, 0, sizeof(*b));
	b->cx = cx;
}

static inline void
sift_bytes_reset(SiftBytes *b)
{
	b->len = 0;
}

static inline void
sift_bytes_free(SiftBytes *b)
{
	if (b->buf != NULL)
		pfree(b->buf);
	memset(b, 0, sizeof(*b));
}

/* The room for more bytes: the pointer to write them at */
static inline uint8 *
sift_bytes_room(SiftBytes *b, uint32 more)
{
	if (b->len + more > b->cap)
	{
		b->cap = Max(Max(b->cap * 2, b->len + more), 64);
		if (b->buf == NULL)
			b->buf = MemoryContextAlloc(b->cx, b->cap);
		else
			b->buf = repalloc(b->buf, b->cap);
	}
	return b->buf + b->len;
}

static inline void
sift_bytes_put(SiftBytes *b, const void *p, uint32 n)
{
	if (n > 0)
		memcpy(sift_bytes_room(b, n), p, n);
	b->len += n;
}

static inline void
sift_bytes_put1(SiftBytes *b, uint8 c)
{
	*sift_bytes_room(b, 1) = c;
	b->len++;
}

#endif							/* SIFT_AGG_V6_BYTES_H */
