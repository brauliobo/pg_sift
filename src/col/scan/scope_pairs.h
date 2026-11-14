/*
 * col/scan/scope_pairs.h - the tables a patch looks the pairs of a row's member up in (col/scan/scope_delta.c): the member keys as a bitmap or a byte table over their range, and the first pair of each member
 */
#ifndef SIFT_SCOPE_PAIRS_H
#define SIFT_SCOPE_PAIRS_H

#include "col/scope.h"

/* The first pair of the member in the pairs (ascending by member), n if it has none */
static inline uint32
first_pair(const SiftScopePair *p, uint32 n, int64 member)
{
	uint32		lo = 0,
				hi = n;

	SIFT_BISECT(uint32, m, lo, hi, p[m].member < member);
	return lo;
}

/* the member keys as a bitmap over their range, which most rows of a buffer or a unit miss: a row is tested in a few instructions before its pairs are looked for */
typedef struct Members
{
	int64		lo;
	uint64		span;
	uint64	   *bits;			/* NULL: the range is too wide for a bitmap, every row looks for its pairs */
	uint8	   *flags;			/* when the range is short, a byte for each key of it: a test of a row is one load */
} Members;

#define MEMBERS_BITS_MAX	((uint64) 1 << 24)
#define MEMBERS_BYTES_MAX	((uint64) 1 << 16)

/* the ascending member keys that the n entries one stride apart from base start with */
static inline Members
members_of(const void *base, Size stride, uint32 n)
{
	int64		first = *(const int64 *) base,
				last = *(const int64 *) ((const char *) base + (n - 1) * stride);
	Members		m = {first, (uint64) last - (uint64) first + 1, NULL, NULL};

	if (m.span > MEMBERS_BITS_MAX)
		return m;
	m.bits = palloc0((m.span + 63) / 64 * sizeof(uint64));
	if (m.span <= MEMBERS_BYTES_MAX)
		m.flags = palloc0(m.span);
	for (uint32 i = 0; i < n; i++)
	{
		uint64		at = (uint64) *(const int64 *) ((const char *) base + i * stride) - (uint64) m.lo;

		sift_col_bit_set(m.bits, at);
		if (m.flags != NULL)
			m.flags[at] = 1;
	}
	return m;
}

static inline bool
members_has(const Members *m, int64 member)
{
	uint64		off = (uint64) member - (uint64) m->lo;

	return off < m->span && (m->bits == NULL || sift_col_bit_test(m->bits, off));
}

/* The first pair of each member of the pairs by its distance from the least member, when the members span few: a row finds its pairs in a read, else (first NULL) by a bisection */
typedef struct PairFirsts
{
	int64		lo;
	uint64		span;
	uint32	   *first;			/* the first pair of member lo + i, PG_UINT32_MAX when it has none */
	Members		mem;			/* without that table: the members as a bitmap, which most rows miss */
} PairFirsts;

#define PAIR_FIRSTS_SPAN_MAX	((uint64) 1 << 20)

static inline PairFirsts
pairs_firsts(const SiftScopePair *p, uint32 n)
{
	PairFirsts	f = {p[0].member, (uint64) p[n - 1].member - (uint64) p[0].member + 1, NULL};

	if (f.span > PAIR_FIRSTS_SPAN_MAX)
	{
		f.mem = members_of(p, sizeof(SiftScopePair), n);
		return f;
	}
	f.first = palloc(f.span * sizeof(uint32));
	memset(f.first, 0xFF, f.span * sizeof(uint32));
	for (uint32 i = n; i-- > 0;)
		f.first[(uint64) p[i].member - (uint64) f.lo] = i;
	return f;
}

/* The first pair of the member (n, or a pair of another member, when it has none) */
static inline uint32
pairs_of_member(const PairFirsts *f, const SiftScopePair *p, uint32 n, int64 member)
{
	uint64		off = (uint64) member - (uint64) f->lo;

	if (f->first == NULL)
		return members_has(&f->mem, member) ? first_pair(p, n, member) : n;
	return off < f->span && f->first[off] != PG_UINT32_MAX ? f->first[off] : n;
}

/* helpers of scope_delta.c that the entity-segment reader (scope_segrows.c) shares */
extern int	sift_scope_fk_leaf(Relation index, int scope);
extern const int64 *sift_scope_batch_members(SiftKeyMap *mm, const SiftColBatch *b, int fk);
extern void sift_scope_delta_kept(Relation index, int scope, const int *leaves, int nleaves, bool *kept);

#endif
