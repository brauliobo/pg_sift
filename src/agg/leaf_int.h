/*
 * agg/leaf_int.h - the output tables and range tables of the key and leaf values, and the expressions of the leaves (agg/leaf.c)
 */
#ifndef SIFT_AGG_LEAF_INT_H
#define SIFT_AGG_LEAF_INT_H

#include "agg/agg_fwd_int.h"

typedef struct OutTable
{
	Oid			collid;
	bool		image;			/* values are the same when their bytes are (generic DISTINCT), not when the type's equality says so */
	bool		byval;
	int16		len;
	FmgrInfo	hashfn;
	FmgrInfo	eqfn;
	uint32		n;				/* ids 1..n */
	uint32		cap;
	Datum	   *val;
	uint32	   *hv;
	uint32		nslot;
	uint32	   *slot;			/* id or 0 */
	uint64		bytes;			/* the by-reference values held */
} OutTable;

typedef struct RangeTab
{
	int32		lo;
	int32		hi;				/* covered values lo..hi; lo > hi: none */
	void	   *tab;			/* esize bytes per value, the entry of lo first */
	int			esize;
	char	   *mem;			/* the allocation: it holds the values plo..phi, so that growing a range is not a copy */
	int32		plo;
	int32		phi;
} RangeTab;

/* fill(arg, v, entry) is called for each value newly covered, ascending */
typedef void (*FillFn) (void *arg, int32 v, void *entry);

typedef struct DictSlot
{
	const void *id;
	uint64	  **lut;			/* per predicate id: bit set of n + 1 entries (bit n: the NULL input) */
	uint32	  **kmap;			/* per key: n + 1 key ids */
	struct DNum *num;			/* wide numeric dictionaries: the integer and scale of each entry, decoded when a row first adds it (scale WNUM_UNSET before) */
} DictSlot;

/* distinct numbers to ids 1.. in order of appearance (open addressing) */
typedef struct IdMap
{
	MemoryContext cxt;
	uint32		n;
	uint32		cap;
	sift_i128  *val;
	uint32		nslot;
	uint32	   *slot;			/* id or 0 */
} IdMap;

/* values the table would cover after adding [lo, hi] */
static inline int64
rt_span(const RangeTab *r, int32 lo, int32 hi)
{
	bool		had = r->lo <= r->hi;

	return (int64) (had ? Max(r->hi, hi) : hi) - (had ? Min(r->lo, lo) : lo) + 1;
}

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* leaf.c */
void ot_init(OutTable *t, Oid eqop, Oid typid, Oid collid);
void ot_init_image(OutTable *t, Oid typid);
uint32 ot_id(OutTable *t, Datum d);
void rt_init(RangeTab *r, int esize);
void rt_cover(RangeTab *r, int32 lo, int32 hi, FillFn fill, void *arg);
Datum leaf_datum(Oid typid, int64 iv);
Datum text_datum(Oid typid, int32 typmod, const char *bytes, uint32 len);
bool eval_bool(AggExec *ex, ExprState *e, Datum in, bool in_null);
DictSlot *dict_slot(AggExec *ex, const SiftSrcDict *d);
uint32 key_value_id(AggExec *ex, int ki, Datum in, bool in_null);
uint32 *dict_kmap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ki);
uint32 *dict_amap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ai);
uint32 kmap_resolve(AggExec *ex, const SiftSrcDict *d, int ki, uint32 c);
uint32 amap_resolve(AggExec *ex, const SiftSrcDict *d, int ai, uint32 c);

/* The maps of a dictionary give an entry its id when a tuple first reads it: a dictionary has far more entries (every distinct text of the column, 100,000 and more) than a scan reads. */
#define DMAP_UNSET	UINT32_MAX

static inline uint32
kmap_id(AggExec *ex, uint32 *map, const SiftSrcDict *d, int ki, uint32 c)
{
	return unlikely(map[c] == DMAP_UNSET) ? (map[c] = kmap_resolve(ex, d, ki, c)) : map[c];
}

static inline uint32
amap_id(AggExec *ex, uint32 *map, const SiftSrcDict *d, int ai, uint32 c)
{
	return unlikely(map[c] == DMAP_UNSET) ? (map[c] = amap_resolve(ex, d, ai, c)) : map[c];
}

/* the entries of the codes that no tuple has read yet get their ids, before a kernel reads the map for all of them */
static inline void
kmap_fill(AggExec *ex, uint32 *map, const SiftSrcDict *d, int ki, const uint32 *codes, int n)
{
	for (int i = 0; i < n; i++)
		if (unlikely(map[codes[i]] == DMAP_UNSET))
			map[codes[i]] = kmap_resolve(ex, d, ki, codes[i]);
}
const SiftSrcVec *get_vec(AggExec *ex, int leaf);
uint32 *elem_offs(AggExec *ex, int leaf);
uint64 *member_elems(AggExec *ex, int j, const uint32 **offs);
void idmap_init(IdMap *m);
uint32 idmap_id(IdMap *m, sift_i128 v);
const sift_i128 *eval_nx(AggExec *ex, const SiftAggNum *p, int nsel, const uint32 *iq, const uint32 *ir, uint8 *isnull, const uint64 *alive);
const uint32 *unit_parent(AggExec *ex);
Datum scaled_numeric(__int128 v, int scale);
Datum scaled_numeric_exact(__int128 v, int scale);

#pragma GCC visibility pop

#endif
