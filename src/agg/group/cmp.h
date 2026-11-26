/*
 * agg/group/cmp.h - the table of the compact group table of SiftAggV6 (agg/group/compact.h), shared by the table of integer keys (v6_group_compact.c), the table of one text key
 * (v6_group_str.c) and the threads of the parallel aggregation (agg/par/group.c): the one table, whatever memory it lives in. A slot is 2, 4 or 8 words. The count of the rows of a group is the
 * word after its key words, or, when cnt_hi is set, the upper half of key word 1: a packed key of two words whose null flags are all clear, and the one text key (word 0: the hash and the place in
 * the arena of its string, word 1: the length of the string), which then take half the words.
 *
 * The memory of a table is the arena of a thread (arena set: nothing is freed, and a call that needs more than the arena has says so by false or NULL, with the table as it was), or the context of
 * the backend (arena NULL: the table grows as far as the caller's budget lets it).
 */
#ifndef SIFT_AGG_V6_GROUP_CMP_H
#define SIFT_AGG_V6_GROUP_CMP_H

#include "agg/kern/kern.h"
#include "agg/group/compact.h"
#include "agg/group/fast.h"
#include "core/par/arena.h"

#define CMP_MAX_WORDS	8
#define CMP_NO_ID		UINT32_MAX

/* The strings a text key has had, each once, numbered in the order they came: a group's key holds the number */
typedef struct IStr
{
	uint64		hash;
	uint32		off;
	uint32		len;
} IStr;

typedef struct Intern
{
	uint32	   *ht;				/* the number of a string + 1, 0 for an empty entry; a power of 2 of them */
	uint32		cap;
	uint32		n;
	uint32		str_cap;
	IStr	   *str;
	char	   *arena;
	Size		used;
	Size		arena_cap;
} Intern;

typedef struct SiftCompact
{
	SiftArena  *arena;			/* where the table lives, or NULL: cx */
	MemoryContext cx;
	int			kw;				/* words of the key: 1 for one integer, 2 for a packed key */
	int			sw;				/* words of a slot: 2, 4 or 8 */
	int			nsum;
	int			sum0;			/* the word of the first sum */
	bool		cnt_hi;			/* the count is the upper half of word 1 (the key of word 1 is its lower half) */
	int			cw;				/* the word of the count, and the bit of it where the count starts, and what a row adds */
	int			cshift;
	uint64		cinc;
	uint64		last_mask;		/* the bits of the last key word that are the key: all, or the lower half when the count has the upper */
	bool		strm;			/* one text key held by the table itself: a slot is its hash, its place in the arena and its length, and its count */
	int8	   *word;			/* of each aggregate: the sum word that is its sum, or -1 */
	SiftPackSpec pack;			/* a set of fixed-width keys: the key columns in the two words of its packed key */
	int			strk;			/* the text key's place among the keys of the set, or -1: its word has the number of the string, and the null flags of every key column from bit 32 */
	uint64	   *rowk;			/* a text among other keys: the key words of the rows of a batch, kw each */
	Intern		in;
	uint32	   *code_id;		/* the number of the string of each code of the dictionary of the part named by map_gen (CMP_NO_ID: not seen) */
	uint64	   *code_ent;		/* strm: of each code, the hash of its string, and from bit 32 its place in the arena + 1 once it has a group (0: not seen) */
	uint32		code_cap;
	uint32		map_n;
	uint64		map_gen;
	uint64	   *tab;
	uint64		cap;
	uint64		mask;
	uint64		count;
	uint64		rows;
	uint64		null_group[CMP_MAX_WORDS];	/* kw 1: the group of the rows whose key is NULL */
	bool		has_gid;		/* the statement has COUNT(DISTINCT): each group has a number, the word gidw of its slot, in the order the groups came */
	int			gidw;
	uint32		ngid;
	uint64	   *dv;				/* the count of each group of each COUNT(DISTINCT), ngid for each, once the scan has ended */
	const uint64 **by_gid;		/* the slot of each group, as far as by_n of them: made when the keys of the groups are asked by number */
	uint32		by_n;
	bool		flush_after;	/* a COUNT(DISTINCT) of the batch went to the exact set: the table is flushed when the batch is done */
} SiftCompact;

static inline uint64
cmp_count(const SiftCompact *c, const uint64 *s)
{
	return s[c->cw] >> c->cshift;
}

static inline void
cmp_count_up(const SiftCompact *c, uint64 *s)
{
	s[c->cw] += c->cinc;
}

/* The rows of a count, and the sums, into the group of a slot: what a partial aggregate of the same group adds */
static inline void
cmp_fold(const SiftCompact *c, uint64 *s, uint64 count, const uint64 *sum)
{
	s[c->cw] += count << c->cshift;
	for (int m = 0; m < c->nsum; m++)
		s[c->sum0 + m] += sum[m];
}

/* ---- memory ---- */

/* n bytes at align, zeroed when zero; NULL when the arena has no room */
static inline void *
cmp_alloc(const SiftCompact *c, Size n, Size align, bool zero)
{
	if (c->arena != NULL)
		return zero ? sift_arena_alloc0(c->arena, n, align) : sift_arena_alloc(c->arena, n, align);
	return MemoryContextAllocAligned(c->cx, n, align, MCXT_ALLOC_HUGE | (zero ? MCXT_ALLOC_ZERO : 0));
}

/* An arena keeps what it gave: it is rewound, not freed */
static inline void
cmp_free(const SiftCompact *c, void *p)
{
	if (c->arena == NULL)
		pfree(p);
}

/* ---- the layout, and the table ---- */

static inline int
cmp_slot_words(int need)
{
	return need <= 2 ? 2 : need <= 4 ? 4 : 8;
}

/*
 * The layout of the slots of the table of c->kw key words (strk: the text key's place, or -1), c->nsum sums and, with has_gid, a group number: c->sw and the words of the parts of a slot. cnt_hi: the
 * count takes the upper half of key word 1, when the keys are two words with a text in neither and word 1 has no bit above 31 (a packed key whose null flags are clear, key1_u32). false when the
 * slot would pass CMP_MAX_WORDS.
 */
extern bool cmp_layout(SiftCompact *c, bool key1_u32);
/* The table of cap slots (a power of 2), empty; false when the arena has no room */
extern bool cmp_init(SiftCompact *c, uint64 cap);
/* The memory the table holds: slots, strings and their numbers */
extern uint64 cmp_bytes(const SiftCompact *c);
/*
 * What the table would hold at the peak of taking bytes more bytes of strings and n more strings, the arrays that grow held old and new at once while they are copied; SIZE_MAX when an array
 * would pass MaxAllocSize (the offsets of the strings are 32 bits)
 */
extern Size cmp_grow_peak(const SiftCompact *c, Size bytes, uint32 n);
/* Room for n more groups at the load limit of 5 in 8, so that the next n rows grow nothing; false when the groups would need more than budget bytes (the tables of both sizes are held while the groups move) or the arena has no room */
extern bool cmp_reserve(SiftCompact *c, uint64 n, uint64 budget);
extern void cmp_free_all(SiftCompact *c);

/* The hash of the key words of a group */
static inline uint64
cmp_key_hash(const SiftCompact *c, const uint64 *k)
{
	uint64		h;

	if (c->kw == 1)
		return sift_aggk6_hash1(k[0]);
	if (c->kw == 2 && c->strk < 0)
		return sift_pack_hash(&(SiftInlineKey16) {.u64 = {k[0], c->cnt_hi ? (uint32) k[1] : k[1]}});
	h = sift_aggk6_hash1(k[c->kw - 1]);
	for (int i = c->kw - 2; i >= 0; i--)
		h = sift_aggk6_hash1(k[i] ^ h);
	return h;
}

/* The slot of the key, made (its key words and a count of 0, which the caller raises) when the key is new: the table has room for it */
static inline uint64 *
cmp_find(SiftCompact *c, uint64 h, const uint64 *key)
{
	for (uint64 i = h & c->mask;; i = (i + 1) & c->mask)
	{
		uint64	   *s = c->tab + i * c->sw;
		int			w = 0;

		if (cmp_count(c, s) == 0)
		{
			memcpy(s, key, c->kw * sizeof(uint64));
			c->count++;
			return s;
		}
		while (w < c->kw - 1 && s[w] == key[w])
			w++;
		if (w == c->kw - 1 && ((s[w] ^ key[w]) & c->last_mask) == 0)
			return s;
	}
}

/* ---- the numbers of the strings (a text key among other keys, or with sums) ---- */

/* The number of the string whose hash is h (the same function of the bytes for every call on a table), which is new when it has none yet; CMP_NO_ID when the arena has no room (the numbers are as they were) */
extern uint32 cmp_intern_h(SiftCompact *c, uint64 h, const char *p, uint32 len);

static inline uint32
cmp_intern(SiftCompact *c, const char *p, uint32 len)
{
	return cmp_intern_h(c, sift_aggk6_hash_bytes(p, len), p, len);
}

/* Room for n more strings of bytes bytes in all: their numbers, and the places they take, so that cmp_intern of that many cannot fail */
extern bool cmp_intern_reserve(SiftCompact *c, uint32 n, Size bytes);

/* ---- one text key (strm) ---- */

/* a hash is never 0: the batch marks the rows with a NULL key by 0 */
static inline uint32
cmp_h32(uint64 h)
{
	return (uint32) h != 0 ? (uint32) h : 1;
}

/* The hash of a string, as the batches hash it (agg/kern/kern.h): what a row of any part finds the group of the same string by */
static inline uint64
cmp_str_hash(const char *p, uint32 len)
{
	return sift_aggk6_hash1(sift_aggk6_hash_lenpfx(p, len));
}

static inline uint32
sl_h32(const uint64 *s)
{
	return (uint32) s[0];
}

static inline uint32
sl_off(const uint64 *s)
{
	return (uint32) (s[0] >> 32);
}

static inline uint32
sl_len(const uint64 *s)
{
	return (uint32) s[1];
}

static inline uint64 *
sstr_slot(const SiftCompact *c, uint64 i)
{
	return c->tab + i * 2;
}

/* The slot of the string, empty when the string is new */
static inline uint64 *
sstr_find(const SiftCompact *c, uint32 h32, const char *p, uint32 len)
{
	for (uint64 i = h32 & c->mask;; i = (i + 1) & c->mask)
	{
		uint64	   *s = sstr_slot(c, i);

		if (cmp_count(c, s) == 0 || (sl_h32(s) == h32 && sl_len(s) == len && memcmp(c->in.arena + sl_off(s), p, len) == 0))
			return s;
	}
}

/* The slot of the string of a code, whose place in the arena is known */
static inline uint64 *
sstr_find_off(const SiftCompact *c, uint32 h32, uint32 off)
{
	for (uint64 i = h32 & c->mask;; i = (i + 1) & c->mask)
	{
		uint64	   *s = sstr_slot(c, i);

		if (sl_h32(s) == h32 && sl_off(s) == off && cmp_count(c, s) != 0)
			return s;
	}
}

/* The bytes of a new string in the arena of the strings: false when the memory has no room */
extern bool sstr_put(SiftCompact *c, const char *p, uint32 len);

/* The group of the string, made when it is new (count 0: the caller raises it); NULL when the memory has no room, and the table is as it was */
static inline uint64 *
sstr_upsert(SiftCompact *c, uint32 h32, const char *p, uint32 len)
{
	uint64	   *s = sstr_find(c, h32, p, len);

	if (cmp_count(c, s) == 0)
	{
		const uint64 off = c->in.used;

		if (!sstr_put(c, p, len))
			return NULL;
		s[0] = h32 | (off << 32);
		s[1] = len;
		c->count++;
	}
	return s;
}

static inline void
sstr_slot_str(const SiftCompact *c, const uint64 *slot, const char **p, uint32 *len)
{
	*p = c->in.arena + sl_off(slot);
	*len = sl_len(slot);
}

/* The groups of the table again in a table of twice the slots */
extern void sstr_move(SiftCompact *c, const uint64 *old, uint64 ocap);

/* The rows sel[0 .. n-1] of the text vector kv into the table, which has room for n more groups */
extern void sstr_batch(SiftCompact *c, SiftV6Group *g, const SiftVec *kv, uint16 n);

#endif							/* SIFT_AGG_V6_GROUP_CMP_H */
