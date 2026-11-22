/*
 * col/v1/batch_int.h - the code maps of col/v1/batch.c: for each dictionary of the source batches of a copy, the code of each of its strings in the dictionary of the destination.
 */
#ifndef SIFT_COL_BATCH_INT_H
#define SIFT_COL_BATCH_INT_H

#include "core/host.h"

#include "core/hash.h"
#include "col/v1/enc.h"

/* The codes of the dictionary of one source batch in the dictionary of dst: one lookup of a string for each code, not for each row (-1: not asked yet) */
typedef struct CodeMap
{
	const SiftCDict *from;
	int64	   *to;
} CodeMap;

/* the maps of a leaf: found by a scan while there are few, by a table of their places (open addressing, 1 + the index; 0: free) after */
typedef struct CodeMapTab
{
	CodeMap    *maps;
	int			n;
	int			cap;
	uint32	   *ht;
	uint32		mask;
} CodeMapTab;

#define CODE_MAP_SCAN	16

static inline uint32
code_map_slot(const CodeMapTab *t, const SiftCDict *from)
{
	return (uint32) (sift_fib64((uintptr_t) from) >> 32) & t->mask;
}

static inline void
code_map_put(CodeMapTab *t, int i)
{
	uint32		h = code_map_slot(t, t->maps[i].from);

	while (t->ht[h] != 0)
		h = (h + 1) & t->mask;
	t->ht[h] = (uint32) i + 1;
}

static inline void
code_map_rehash(CodeMapTab *t)
{
	t->mask = (uint32) t->cap * 2 - 1;
	t->ht = t->ht ? repalloc(t->ht, ((Size) t->mask + 1) * sizeof(uint32)) : palloc(((Size) t->mask + 1) * sizeof(uint32));
	memset(t->ht, 0, ((Size) t->mask + 1) * sizeof(uint32));
	for (int i = 0; i < t->n; i++)
		code_map_put(t, i);
}

static inline int64 *
code_map(CodeMapTab *t, const SiftCDict *from)
{
	CodeMap    *m;
	bool		grown = false;

	if (t->ht == NULL)
	{
		for (int i = 0; i < t->n; i++)
			if (t->maps[i].from == from)
				return t->maps[i].to;
	}
	else
		for (uint32 h = code_map_slot(t, from); t->ht[h] != 0; h = (h + 1) & t->mask)
			if (t->maps[t->ht[h] - 1].from == from)
				return t->maps[t->ht[h] - 1].to;
	if (t->n == t->cap)
	{
		t->cap = Max(8, t->cap * 2);
		t->maps = t->maps ? repalloc(t->maps, t->cap * sizeof(CodeMap)) : palloc(t->cap * sizeof(CodeMap));
		grown = true;
	}
	m = &t->maps[t->n++];
	m->from = from;
	m->to = palloc(Max(from->n, 1) * sizeof(int64));
	memset(m->to, 0xff, Max(from->n, 1) * sizeof(int64));
	if (t->n > CODE_MAP_SCAN && (t->ht == NULL || grown))
		code_map_rehash(t);
	else if (t->ht != NULL)
		code_map_put(t, t->n - 1);
	return m->to;
}

static inline void
code_map_free(CodeMapTab *t)
{
	for (int i = 0; i < t->n; i++)
		pfree(t->maps[i].to);
	if (t->maps)
		pfree(t->maps);
	if (t->ht)
		pfree(t->ht);
}

#endif							/* SIFT_COL_BATCH_INT_H */
