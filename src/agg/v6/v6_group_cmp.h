/*
 * agg/v6/v6_group_cmp.h - the table of the compact group table of SiftAggV6 (agg/v6/v6_group_compact.h), shared by the table of integer keys (v6_group_compact.c) and the table of one text key
 * (v6_group_str.c). A slot is 2, 4 or 8 words. The count of the rows of a group is the word after its key words, or, when cnt_hi is set, the upper half of key word 1: a packed key of two words
 * whose null flags are all clear, and the one text key (word 0: the hash and the place in the arena of its string, word 1: the length of the string), which then take half the words.
 */
#ifndef SIFT_AGG_V6_GROUP_CMP_H
#define SIFT_AGG_V6_GROUP_CMP_H

#include "agg/v6/v6_group_compact.h"
#include "agg/v6/v6_group_fast.h"

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

/* The rows sel[0 .. n-1] of the text vector kv into the table, which has room for n more groups */
extern void sstr_batch(SiftCompact *c, SiftV6Group *g, const SiftVec *kv, uint16 n);
/* The string of the group of a slot, which is not the group of the NULL key */
extern void sstr_slot_str(const SiftCompact *c, const uint64 *slot, const char **p, uint32 *len);
/* The groups of the table again in a table of twice the slots */
extern void sstr_move(SiftCompact *c, const uint64 *old, uint64 ocap);

#endif							/* SIFT_AGG_V6_GROUP_CMP_H */
