/*
 * agg/col_int.h - what the pieces of the column source (col.c, col_filter.c, col_read.c, col_recheck.c) share
 */
#ifndef SIFT_AGG_COL_INT_H
#define SIFT_AGG_COL_INT_H

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/kern.h"
#include "agg/pre.h"
#include "agg/sel.h"
#include "agg/wide.h"
#include "core/popcount.h"
#include "col/col.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "col/am.h"
#include "col/enc.h"
#include "col/pg.h"
#include "agg/colsrc.h"
#include "col/shred_pg.h"

typedef struct ColLeafBuf
{
	size_t		cap;			/* elements the buffers hold */
	void	   *data;
	uint8	   *isnull;
	uint64	   *ovf;			/* numeric leaves: the NaNs of the heap */
} ColLeafBuf;

/*
 * The recheck of the rows that hold, in a leaf the query reads, a value the leaf cannot store (docs/COL-DIRTY-UNITS.md): `live` takes them out of
 * the stored unit and keeps their TIDs, the next unit is these rows read again from the heap as the query's SQL reads them.
 */
typedef struct ColRecheck
{
	Relation	heap;
	Relation	index;
	Snapshot	snap;
	MemoryContext life;			/* of the source */
	SiftShredRecheck *rc;
	int			nleaf;
	int			leaf[SIFT_COLSRC_MAX_LEAVES];	/* the leaves with a mode */
	uint64		gen;			/* the unit (ColSource.ugen) the rows were collected for */
	uint64	   *flag;			/* its flagged rows, live or not */
	uint64	   *cand;			/* the live ones: they are read again unless the executor clears them (col_recheck_rows) */
	uint32		nrow;
	bool		taken;			/* cand has rows: recheck_pending makes their TIDs, to be returned as the next unit */
	uint32		sb;
	uint32		npend;
	ItemPointerData *tid;
	bool		active;			/* the unit the source is at is a recheck unit */
	int64		nrechecked;		/* rows read again, in all */
	int64		nunits;			/* the units they were in */
	MemoryContext cxt;			/* the rows of the recheck unit, reset by the next one */
	MemoryContext tmp;			/* one row */
	ErrorData **err;			/* per row of the recheck unit: the error SQL raises for it (NULL: none) */
	IndexInfo  *info;
	EState	   *estate;
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
} ColRecheck;

typedef struct ColSource
{
	ColRecheck *rck;			/* NULL: no leaf is rechecked */
	SiftColSource *cs;
	SiftColUnit unit;
	SiftSrcLayout *lay;
	MemoryContext cxt;
	SiftSrcDict *dict;			/* per leaf: the dictionary of the last read */
	ColLeafBuf *buf;			/* per leaf */
	bool	   *tol;			/* per leaf: its values the index cannot hold are read as NULL (tolerate_ovf) */
	Relation	heap;
	Relation	index;
	Snapshot	snapshot;
	SiftAggSel	sel;			/* the match set the units are restricted to */
	int			nflt;			/* the filters set_filters accepted, as engine predicates */
	SiftColPred *fpred;
	SiftColPred *spred;			/* the text tests of prune_str, as column predicates */
	int			nspred;
	const SiftSrcPrune *zfor;	/* the prune entries of the last begin, and the zone trees made of those that are junctions */
	SiftColPrune *zone;
	int			nzone;
	SiftColWTest *wtest;		/* the conjuncts on wide leaves of set_wide */
	int			nwtest;
	SiftAggWide *wide;			/* per leaf: the dictionary of a wide leaf in the unit (wgen: the unit it was made for) */
	uint64	   *wgen;
	uint64		ugen;			/* the unit the source is at, counted */
	uint64		wseq;			/* dictionaries made, for their ids */
	SiftColValue *fvals;		/* the constants of one filter_str call */
	int			fcap;
	SiftAggPre	pre;			/* the histogram or rollup of the last unit_hist / unit_rollup */
	SiftSrcDict rdict[SIFT_SRC_MAX_RDIMS];	/* the dictionaries of the dims of the last unit_rollup */
} ColSource;

#define COL_MAX_STR_PRUNE	16
#define COL_MAX_WIDE		8		/* the wide conjuncts of a scan (SIFT_AGG_MAX_WIDE) */

extern bool col_overflow(void *impl, const SiftSrcUnit *unit, int leaf);
extern void col_set_recheck(void *impl, int leaf, int mode, const char *key, Oid cast);
extern void col_overflow_raise(void *impl, const SiftSrcUnit *unit, const uint64 *rows);
extern void recheck_close(ColRecheck *k);
extern void recheck_unpin(ColRecheck *k);
extern void recheck_collect(ColSource *src, uint32 nrow, uint64 *bits);
extern uint64 *col_recheck_rows(void *impl, const SiftSrcUnit *unit);
extern bool recheck_pending(ColSource *src);
extern bool recheck_next(ColSource *src, SiftSrcUnit *unit);
extern void col_prune_str(void *impl, const SiftSrcStrTest *t, int nt);
extern bool col_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows);
extern int col_set_wide(void *impl, const SiftSrcWideTest *t, int nt);
extern bool col_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows);
extern uint64 col_set_filters(void *impl, const SiftSrcFilter *f, int nf);
extern bool col_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern void col_tolerate_ovf(void *impl, int leaf);
extern bool col_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
extern void col_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags);
extern void col_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits);
extern void col_counters(const void *impl, SiftSrcCounters *c);
extern int64 col_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf);
extern void col_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec);
extern void col_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
extern void col_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);
extern void col_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);
extern void col_live(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern void col_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern bool col_vet_needed(void *impl, const SiftSrcUnit *unit);

#endif							/* SIFT_AGG_COL_INT_H */
