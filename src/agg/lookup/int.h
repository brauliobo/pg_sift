/*
 * agg/lookup/int.h - what the files of agg/lookup/ share
 * The walk records of the kept conditions, the key dictionaries (inline: the load and the probe
 * of a row call them), and the functions the files of agg/lookup/ call in one another.
 */
#ifndef SIFT_AGG_LOOKUP_DIR_INT_H
#define SIFT_AGG_LOOKUP_DIR_INT_H

#include "common/hashfn.h"

#include "agg/agg_int.h"

/* the order columns of the rows of a first-row lookup: the value of each (NULL: 0 and isnull) */
typedef struct FirstOrd
{
	MemoryContext cx;			/* where the values live, deleted once the rows are chosen */
	int64	   *val;
	bool	   *isnull;
	uint32		cap;
} FirstOrd;

/* a table kept for the queries that follow (lookcache.c): where its rows are loaded, the pages it was loaded from and the columns to load */
typedef struct KeptTry
{
	char	   *key;
	LookFinger *before;
	MemoryContext cx;
	LookCols	cols;
} KeptTry;

/* the id of the integer key value v among the values of d, 0 when it is not there; *at: the slot it is in or would go to */
static inline uint32
kd_int_find(const KeyDict *d, int64 v, uint32 *at)
{
	uint32		h;

	if (d->nslot == 0)
	{
		*at = 0;
		return 0;
	}
	h = murmurhash32((uint32) (v ^ (v >> 32))) & (d->nslot - 1);
	for (uint32 id; (id = d->slot[h]) != 0; h = (h + 1) & (d->nslot - 1))
		if (d->ival[id - 1] == v)
			return id;
	*at = h;
	return 0;
}

/* the id of the integer key value v, which is added to d (in cxt) when new */
static inline uint32
kd_int(KeyDict *d, int64 v, MemoryContext cxt)
{
	uint32		h,
				id;

	if (d->nslot == 0)
	{
		d->nslot = 64;
		d->cap = 32;
		d->slot = MemoryContextAllocZero(cxt, d->nslot * sizeof(uint32));
		d->ival = MemoryContextAlloc(cxt, d->cap * sizeof(int64));
	}
	if ((id = kd_int_find(d, v, &h)) != 0)
		return id;
	if (d->n == d->cap)
	{
		d->cap *= 2;
		d->ival = repalloc(d->ival, d->cap * sizeof(int64));
	}
	d->ival[d->n] = v;
	d->slot[h] = ++d->n;
	if (d->n * 2 > d->nslot)
	{
		pfree(d->slot);
		d->nslot *= 2;
		d->slot = MemoryContextAllocZero(cxt, d->nslot * sizeof(uint32));
		for (uint32 i = 1; i <= d->n; i++)
		{
			int64		x = d->ival[i - 1];

			for (h = murmurhash32((uint32) (x ^ (x >> 32))) & (d->nslot - 1); d->slot[h]; h = (h + 1) & (d->nslot - 1))
				;
			d->slot[h] = i;
		}
	}
	return d->n;
}

/* the same for a text key value */
static inline uint32
kd_text_find(const KeyDict *d, const char *b, uint32 len, uint32 hv, uint32 *at)
{
	uint32		h;

	if (d->nslot == 0)
	{
		*at = 0;
		return 0;
	}
	h = hv & (d->nslot - 1);
	for (uint32 id; (id = d->slot[h]) != 0; h = (h + 1) & (d->nslot - 1))
		if (d->hv[id - 1] == hv && d->len[id - 1] == len && memcmp(d->ptr[id - 1], b, len) == 0)
			return id;
	*at = h;
	return 0;
}

/* the id of a text key value, which is added to d (its bytes copied into cxt) when new */
static inline uint32
kd_text(KeyDict *d, const char *b, uint32 len, MemoryContext cxt)
{
	uint32		hv = hash_bytes((const unsigned char *) b, len),
				h,
				id;
	char	   *cp;

	if (d->nslot == 0)
	{
		d->nslot = 64;
		d->cap = 32;
		d->slot = MemoryContextAllocZero(cxt, d->nslot * sizeof(uint32));
		d->ptr = MemoryContextAlloc(cxt, d->cap * sizeof(char *));
		d->len = MemoryContextAlloc(cxt, d->cap * sizeof(uint32));
		d->hv = MemoryContextAlloc(cxt, d->cap * sizeof(uint32));
	}
	if ((id = kd_text_find(d, b, len, hv, &h)) != 0)
		return id;
	if (d->n == d->cap)
	{
		d->cap *= 2;
		d->ptr = repalloc(d->ptr, d->cap * sizeof(char *));
		d->len = repalloc(d->len, d->cap * sizeof(uint32));
		d->hv = repalloc(d->hv, d->cap * sizeof(uint32));
	}
	cp = MemoryContextAlloc(cxt, Max(len, 1));
	memcpy(cp, b, len);
	d->ptr[d->n] = cp;
	d->len[d->n] = len;
	d->hv[d->n] = hv;
	d->slot[h] = ++d->n;
	if (d->n * 2 > d->nslot)
	{
		pfree(d->slot);
		d->nslot *= 2;
		d->slot = MemoryContextAllocZero(cxt, d->nslot * sizeof(uint32));
		for (uint32 i = 1; i <= d->n; i++)
		{
			for (h = d->hv[i - 1] & (d->nslot - 1); d->slot[h]; h = (h + 1) & (d->nslot - 1))
				;
			d->slot[h] = i;
		}
	}
	return d->n;
}

static inline uint32
map_hash_ids(int nkey, const uint32 *kid)
{
	uint64		h = UINT64CONST(0x9E3779B97F4A7C15);

	for (int k = 0; k < nkey; k++)
	{
		h = (h ^ kid[k]) * UINT64CONST(0xD6E8FEB86659FD93);
		h ^= h >> 32;
	}
	h *= UINT64CONST(0x9E3779B97F4A7C15);
	return (uint32) (h >> 32);
}

extern void map_grow(MapTab *m);
extern void map_col_put(MapTab *m, int c, Datum d, bool cnull);
extern void first_order_row(const MapTab *m, const SiftAggFirst *f, FirstOrd *o, TupleTableSlot *slot);
extern void map_keep_first(MapTab *m, const SiftAggFirst *f, const FirstOrd *o);
extern void map_keep_agreeing(MapTab *m);
extern MapTab * map_load_table(AggExec *ex, int ci, MemoryContext cxt);
extern char * shared_map_key(AggExec *ex, int ci, bool cols);

extern void map_index_rows(MapTab *m);

extern MapTab * map_view(MapTab *m, const LookCols *have, const SiftAggJoin *j, MemoryContext mcxt);
extern MapTab * map_kept_find(AggExec *ex, int ci, Relation rel, MemoryContext cxt, bool own, KeptTry *t);
extern void map_kept_keep(Relation rel, const KeptTry *t, MapTab *m, MemoryContext cxt);
extern int map_keyed_dependent(AggExec *ex, int ci, int *att, Oid *typ);

extern Relation map_index(Relation rel, const SiftAggJoin *j, const Datum *val, ScanKeyData *keys, int *nkeys);
extern MapTab * map_new(const SiftAggJoin *j, bool filtered, MemoryContext cxt);

#endif							/* SIFT_AGG_LOOKUP_DIR_INT_H */
