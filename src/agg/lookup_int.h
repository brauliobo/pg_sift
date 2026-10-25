/*
 * agg/lookup_int.h - the rows of a lookup join, loaded by key (agg/lookup.c)
 */
#ifndef SIFT_AGG_LOOKUP_INT_H
#define SIFT_AGG_LOOKUP_INT_H

#include "access/xlogdefs.h"
#include "portability/instr_time.h"
#include "storage/relfilelocator.h"

#include "agg/agg_fwd_int.h"

/* the distinct values of one key column of a table, ids 1..: a row holds the id of its value (0: NULL) */
typedef struct KeyDict
{
	uint32		n;
	uint32		cap;
	uint32		nslot;
	uint32	   *slot;			/* the id in the hash slot, 0 free */
	const char **ptr;			/* text keys: the bytes of an id */
	uint32	   *len;
	uint32	   *hv;
	int64	   *ival;			/* integer keys: the value of an id */
} KeyDict;

/* a column the expressions read: by value the Datum of each row, by reference the distinct values (val, by id) and the id of the value of each row (0: NULL) */
typedef struct MapCol
{
	bool		byval;
	int16		len;
	Datum	   *val;
	uint32	   *id;
	bool	   *isnull;			/* by value: per row, NULL until a row is NULL */
	uint32		n;				/* by reference: the distinct values, found by a table that only the load needs */
	uint32		cap;
	uint32		nslot;
	uint32	   *slot;
	uint32	   *hv;
} MapCol;

/* the rows of a joined table, hashed by the ids of their key values; the columns the expressions read are kept once per distinct value */
typedef struct MapTab
{
	int			nkey;
	int			ncol;
	bool		ktext[SIFT_AGG_MAX_JKEYS];
	bool		knull[SIFT_AGG_MAX_JKEYS];	/* a row whose key is NULL is in the chain of the NULL id 0 (SiftAggJoin.keynull) */
	uint32		nrow;
	uint32		cap;
	KeyDict		kd[SIFT_AGG_MAX_JKEYS];
	uint32	   *kid;			/* [row * nkey + k] */
	MapCol	   *mcol;			/* [ncol] */
	uint32	   *next;			/* row + 1 of the next row of the bucket */
	uint32		nslot;
	uint32	   *head;
	MemoryContext cxt;			/* where the rows live: the scan's, or the statement's for a table its arms share (map_load_shared) */
	bool		filtered;		/* only rows that hold the ON clause's own conditions were loaded (map_load) */
	bool		keep;			/* the side is preserved: the rows no tuple matched are output rows of NULL leaves; a row with a NULL key is in no chain */
	bool	   *hit;			/* keep: the rows some tuple matched */
	ExprState  *kx[SIFT_AGG_MAX_JKEYS];	/* while the rows are loaded: the key expressions of the table's columns (SiftAggJoin.tkey), evaluated in kcx for each row */
	ExprContext *kcx;
} MapTab;

/* does a NULL key of the row equal nothing, so that it is in no chain? */
static inline bool
map_key_dead(const MapTab *m, const uint32 *kid)
{
	for (int k = 0; k < m->nkey; k++)
		if (kid[k] == 0 && !m->knull[k])
			return true;
	return false;
}

/* the integer key k of a row */
static inline int64
map_kint(const MapTab *m, uint32 row, int k)
{
	uint32		id = m->kid[(size_t) row * m->nkey + k];

	return id == 0 ? 0 : m->kd[k].ival[id - 1];
}

/* the column c of a row */
static inline Datum
map_col(const MapTab *m, uint32 row, int c, bool *isnull)
{
	const MapCol *mc = &m->mcol[c];

	if (mc->byval)
	{
		*isnull = mc->isnull != NULL && mc->isnull[row];
		return mc->val[row];
	}
	*isnull = mc->id[row] == 0;
	return *isnull ? (Datum) 0 : mc->val[mc->id[row] - 1];
}

typedef struct MapShared
{
	uint32		nkey;
	uint32		ncol;
	uint32		nrow;
	uint32		nslot;
	bool		ktext[SIFT_AGG_MAX_JKEYS];
	bool		knull[SIFT_AGG_MAX_JKEYS];
	bool		filtered;
	bool		keep;			/* the side is preserved: each participant keeps its own hit[] */
	Size		kid;			/* offsets from the start of the segment */
	Size		next;
	Size		head;
	struct
	{
		uint32		n;			/* the distinct values of a key and the table that finds them */
		uint32		nslot;
		Size		slot;
		Size		len;
		Size		hv;
		Size		off;		/* text keys: offset in the data of the bytes of each value */
		Size		ival;
	}			key[SIFT_AGG_MAX_JKEYS];
	struct
	{
		bool		byval;
		bool		hasnull;
		int16		len;
		uint32		n;			/* by reference: the distinct values */
		Size		val;		/* by value: a Datum per row */
		Size		isnull;
		Size		id;			/* by reference: the value of each row */
		Size		off;		/* ... and the offset in the data of each value */
	}			col[SIFT_AGG_MAX_JCOLS];
	Size		data;
	Size		size;
} MapShared;

/* the columns of a joined table that a MapTab holds, by column number and type */
typedef struct LookCols
{
	int			n;
	int			att[SIFT_AGG_MAX_JCOLS];
	Oid			type[SIFT_AGG_MAX_JCOLS];
} LookCols;

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
uint32 map_key_id(const MapTab *m, int k, const char *ptr, uint32 len, int64 iv);
int map_find_ids(const MapTab *m, const uint32 *kid, uint32 **rows);
int map_find(const MapTab *m, const char **ptr, const uint32 *len, const int64 *iv, uint32 **rows);
void map_bind(AggExec *ex, int param, const MapTab *m, uint32 row, int c);
void map_row(AggExec *ex, MapTab *m, TupleTableSlot *slot, const SiftAggJoin *j, const SiftAggCombo *cb, ExprState *own);
bool map_load_cols_possible(Relation rel, const SiftAggJoin *j);
bool map_load_cols(AggExec *ex, MapTab *m, const SiftAggJoin *j, const SiftAggCombo *cb, ExprState *own, Relation rel);
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
bool lookcache_refused(const char *key, Relation rel);
MapTab *lookcache_find(const char *key, const LookFinger *f, const LookCols *need, LookCols *have, MemoryContext mcxt);
bool lookcache_keep(const char *key, const LookFinger *before, const LookFinger *after, MapTab *m, const LookCols *cols, MemoryContext cx, MemoryContext mcxt);
bool cols_cover(const LookCols *have, const LookCols *need);

/* lookshm.c */
MapTab *lookshm_find(const char *key, const LookFinger *f, const LookCols *need, LookCols *have, MemoryContext mcxt);
bool lookshm_keep(const char *key, const LookFinger *f, MapTab *m, const LookCols *cols, bool *large);
bool lookshm_claim(const char *key, MemoryContext mcxt);
void lookshm_unclaim(const char *key);
Size lookshm_bytes(void);

#pragma GCC visibility pop

#endif
