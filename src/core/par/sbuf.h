/*
 * core/par/sbuf.h - the StringInfo calls of an encoder that runs on the backend or on a pool thread (core/par/arena.h). On the backend they are the StringInfo
 * functions; in a scope the buffer grows in the arena of the scope (the buffer it leaves behind is garbage the caller rewinds), so the thread never reaches palloc.
 * A buffer is initialised and grown on one side only: the one a worker fills is the worker's, and the leader copies its bytes out before it rewinds the arena.
 */
#ifndef SIFT_SBUF_H
#define SIFT_SBUF_H

#include "core/par/arena.h"

#define SIFT_SB_MIN		256

static inline void
sift_sb_reset(StringInfo s)
{
	s->len = 0;
	s->cursor = 0;
	s->data[0] = '\0';
}

static inline void
sift_sb_init(StringInfo s)
{
	if (likely(!sift_arena_scoped()))
	{
		initStringInfo(s);
		return;
	}
	s->data = sift_alloc(SIFT_SB_MIN);
	s->maxlen = SIFT_SB_MIN;
	sift_sb_reset(s);
}

/* Room for n more bytes and the NUL that ends them */
static inline void
sift_sb_need(StringInfo s, int n)
{
	if (likely(!sift_arena_scoped()))
		enlargeStringInfo(s, n);
	else if (unlikely(s->len + n >= s->maxlen))
	{
		int			cap = Max(2 * s->maxlen, s->len + n + 1);
		char	   *grown = sift_alloc(cap);

		memcpy(grown, s->data, s->len + 1);
		s->data = grown;
		s->maxlen = cap;
	}
}

static inline void
sift_sb_append(StringInfo s, const void *src, int n)
{
	sift_sb_need(s, n);
	memcpy(s->data + s->len, src, n);
	s->len += n;
	s->data[s->len] = '\0';
}

static inline void
sift_sb_spaces(StringInfo s, int n)
{
	sift_sb_need(s, n);
	memset(s->data + s->len, ' ', n);
	s->len += n;
	s->data[s->len] = '\0';
}

static inline void
sift_sb_char(StringInfo s, char c)
{
	sift_sb_append(s, &c, 1);
}

#endif							/* SIFT_SBUF_H */
