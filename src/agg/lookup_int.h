/*
 * agg/lookup_int.h - the rows of a lookup join, loaded by key (agg/lookup.c)
 */
#ifndef SIFT_AGG_LOOKUP_INT_H
#define SIFT_AGG_LOOKUP_INT_H

#include "access/xlogdefs.h"
#include "storage/relfilelocator.h"

#include "agg/agg_fwd_int.h"

/* the rows of a joined table, hashed by their key columns; the columns the expressions read are kept as Datums */
typedef struct MapTab
{
	int			nkey;
	int			ncol;
	bool		ktext[SIFT_AGG_MAX_JKEYS];
	uint32		nrow;
	uint32		cap;
	const char **kptr;			/* [row * nkey + k] text keys */
	uint32	   *klen;
	int64	   *kint;			/* integer keys */
	Datum	   *col;			/* [row * ncol + c] */
	bool	   *cnull;
	uint32	   *hv;
	uint32	   *next;			/* row + 1 of the next row of the bucket */
	uint32		nslot;
	uint32	   *head;
	MemoryContext cxt;			/* where the rows live: the scan's, or the statement's for a table its arms share (map_load_shared) */
	bool		filtered;		/* only rows that hold the ON clause's own conditions were loaded (map_load) */
	bool		keep;			/* the side is preserved: the rows no tuple matched are output rows of NULL leaves */
	bool	   *nokey;			/* keep: the rows with a NULL key, which no tuple matches and are not in the chains */
	bool	   *hit;			/* keep: the rows some tuple matched */
} MapTab;

static inline uint32
map_hash(const MapTab *m, const char **ptr, const uint32 *len, const int64 *iv)
{
	uint32		h = 0x9E3779B9;

	for (int k = 0; k < m->nkey; k++)
		h = hash_combine(h, m->ktext[k] ? hash_bytes((const unsigned char *) ptr[k], len[k]) :
						 murmurhash32((uint32) (iv[k] ^ (iv[k] >> 32))));
	return h;
}

static inline bool
map_key_eq(const MapTab *m, uint32 row, const char **ptr, const uint32 *len, const int64 *iv)
{
	for (int k = 0; k < m->nkey; k++)
	{
		size_t		i = (size_t) row * m->nkey + k;

		if (m->ktext[k] ? (m->klen[i] != len[k] || memcmp(m->kptr[i], ptr[k], len[k]) != 0) : m->kint[i] != iv[k])
			return false;
	}
	return true;
}

/* the rows whose key equals the one given; palloc'd in the current context */
static inline int
map_find(const MapTab *m, const char **ptr, const uint32 *len, const int64 *iv, uint32 **rows)
{
	uint32		h = map_hash(m, ptr, len, iv);
	int			n = 0;

	*rows = palloc(Max(m->nrow, 1) * sizeof(uint32));
	for (uint32 r = m->head[h & (m->nslot - 1)]; r != 0; r = m->next[r - 1])
		if (m->hv[r - 1] == h && map_key_eq(m, r - 1, ptr, len, iv))
			(*rows)[n++] = r - 1;
	return n;
}

typedef struct MapShared
{
	uint32		nkey;
	uint32		ncol;
	uint32		nrow;
	uint32		nslot;
	bool		ktext[SIFT_AGG_MAX_JKEYS];
	bool		filtered;
	Size		klen;			/* offsets from the start of the segment */
	Size		kint;
	Size		koff;			/* text keys: offset in the data of the bytes, MAP_NOOFF for a NULL */
	Size		cval;			/* a column: the value itself, or the offset in the data of a by-reference one */
	Size		cnull;
	Size		hv;
	Size		next;
	Size		head;
	Size		data;
	Size		size;
} MapShared;

/* what a table is, page by page, when its rows are kept for another query (lookcache.c) */
typedef struct LookFinger
{
	RelFileLocator rel;
	BlockNumber nblocks;
	XLogRecPtr *lsn;
} LookFinger;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* lookup.c */
void map_row(AggExec *ex, MapTab *m, TupleTableSlot *slot, const SiftAggJoin *j, const SiftAggCombo *cb, ExprState *own);
void map_loaded(AggExec *ex, int nrows, instr_time t0);
MapTab *map_load(AggExec *ex, int ci);
bool map_unique(const MapTab *m);
bool slot_reads_leaf(const SiftAggCombo *cb, int sl);

/* lookshare.c */
Size map_share_size(const MapTab *m, const SiftAggJoin *j, MapShared *l);
void map_share_write(const MapTab *m, const SiftAggJoin *j, const MapShared *l, char *dst);
MapTab *map_share_attach(const char *src, const SiftAggJoin *j, MemoryContext cxt);

/* lookcache.c */
LookFinger *lookcache_finger(Relation rel);
MapTab *lookcache_find(const char *key, const LookFinger *f, MemoryContext mcxt);
bool lookcache_keep(const char *key, const LookFinger *before, const LookFinger *after, MapTab *m, MemoryContext cx, MemoryContext mcxt);

#pragma GCC visibility pop

#endif
