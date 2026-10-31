/*
 * bm25/analyze_memo.h - the memo of a stemmer: every word it was given and what it answered (words follow Zipf's law and the vocabulary of a
 * corpus is small, while a stem costs hundreds of instructions and a dictionary lookup thousands).
 *
 * Entries [uint16 inlen][uint16 outlen][in][out] in an arena, found through an open-addressing table of offsets (+ 1). outlen
 * MEMO_STOP is a word the stemmer dropped (its answer was negative). Both are dropped when the arena passes MEMO_MAX, so a vocabulary without
 * end stays bounded: per process, 8 MB of entries and at most 4 MB of table.
 */
#ifndef SIFT_ANALYZE_MEMO_H
#define SIFT_ANALYZE_MEMO_H

#include "core/host.h"

#include "bm25/analyze.h"

#define MEMO_MAX		(8 * 1024 * 1024)
#define MEMO_TAB0		4096
#define MEMO_STOP		0xFFFF

typedef struct StemMemo
{
	MemoryContext mcxt;
	char	   *arena;
	uint32		used;
	uint32		cap;
	uint32	   *tab;
	uint32		tabmask;
	uint32		nent;
	uint32		max;			/* arena bytes before the memo is dropped; 0: MEMO_MAX */
} StemMemo;

/* A stemmer rewrites w[0..n) in place and returns the new length, negative for a word it drops */
typedef int (*MemoStemFn) (void *ctx, char *w, int n);

static void
memo_clear(StemMemo *m)
{
	m->used = 0;
	m->nent = 0;
	if (m->tab)
		memset(m->tab, 0, (m->tabmask + 1) * sizeof(uint32));
}

static void
memo_grow_tab(StemMemo *m)
{
	uint32		size = m->tab ? (m->tabmask + 1) * 2 : MEMO_TAB0;

	if (m->tab)
		pfree(m->tab);
	m->tab = MemoryContextAllocZero(m->mcxt, size * sizeof(uint32));
	m->tabmask = size - 1;
	for (uint32 off = 0; off < m->used;)
	{
		const uint16 *h = (const uint16 *) (m->arena + off);
		uint32		i = (uint32) sift_term_hash((const char *) (h + 2), h[0]) & m->tabmask;

		while (m->tab[i])
			i = (i + 1) & m->tabmask;
		m->tab[i] = off + 1;
		off += 4 + h[0] + (h[1] == MEMO_STOP ? 0 : h[1]);
	}
}

/* fn(ctx, w, n) through the memo; the answer in w, as fn gives it */
static inline int
memo_stem(StemMemo *m, MemoStemFn fn, void *ctx, char *w, int n)
{
	uint32		i;
	int			r;
	char		in[SIFT_MAX_TERM_BYTES + 16];
	uint16		hd[2];

	if (!m->tab)
		memo_grow_tab(m);
	i = (uint32) sift_term_hash(w, n) & m->tabmask;
	for (; m->tab[i]; i = (i + 1) & m->tabmask)
	{
		const uint16 *h = (const uint16 *) (m->arena + m->tab[i] - 1);

		if (h[0] == n && memcmp(h + 2, w, n) == 0)
		{
			if (h[1] == MEMO_STOP)
				return -1;
			memcpy(w, (const char *) (h + 2) + n, h[1]);
			return h[1];
		}
	}
	memcpy(in, w, n);
	r = fn(ctx, w, n);
	if (m->used + 4 + n + Max(r, 0) > m->cap)
	{
		if (m->cap >= (m->max ? m->max : MEMO_MAX))
		{
			memo_clear(m);
			i = (uint32) sift_term_hash(in, n) & m->tabmask;
			while (m->tab[i])
				i = (i + 1) & m->tabmask;
		}
		else
		{
			m->cap = m->cap ? m->cap * 2 : 64 * 1024;
			m->arena = m->arena ? repalloc(m->arena, m->cap) : MemoryContextAlloc(m->mcxt, m->cap);
		}
	}
	hd[0] = n;
	hd[1] = r < 0 ? MEMO_STOP : r;
	memcpy(m->arena + m->used, hd, 4);
	memcpy(m->arena + m->used + 4, in, n);
	if (r > 0)
		memcpy(m->arena + m->used + 4 + n, w, r);
	m->tab[i] = m->used + 1;
	m->used += 4 + n + Max(r, 0);
	if (++m->nent * 2 > m->tabmask + 1)
		memo_grow_tab(m);
	return r;
}

#endif							/* SIFT_ANALYZE_MEMO_H */
