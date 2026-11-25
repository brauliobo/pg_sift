/*
 * agg/col_int.h - what the pieces of the column source (col.c, col_filter.c, col_read.c, col_recheck.c) share
 */
#ifndef SIFT_AGG_COL_INT_H
#define SIFT_AGG_COL_INT_H

#include "nodes/execnodes.h"

#include "agg/pre.h"
#include "col/v1/src_zone.h"
#include "agg/chunkfilter.h"
#include "col/v1/src_sel.h"
#include "agg/wide.h"
#include "col/v1/v1_reader_int.h"
#include "col/col.h"
#include "col/shred_pg.h"

/*
 * The recheck of the rows that hold, in a leaf the query reads, a value the leaf cannot store: `live` takes them out of
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
	bool		haskey[SIFT_COLSRC_MAX_LEAVES];	/* ... that are the key of doc ? 'key' (SRK_HASKEY): the rows to read again are those of a document that is no object */
	bool		read[SIFT_COLSRC_MAX_LEAVES];	/* ... that the query reads, beside a key test or alone: the rows to read again are those that hold a value the leaf cannot store */
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
	SiftV1Idx	rd;				/* the unit reader: first, a unit's priv points at it */
	ColRecheck *rck;			/* NULL: no leaf is rechecked */
	bool	   *numsum;			/* per leaf: a numeric leaf only summed, read as one record for each row (set_numsum) */
	SiftAggFilters flt;			/* the filters set_filters accepted, in rcx (reset by the next set_filters) */
	MemoryContext rcx;
	SiftColPred *spred;			/* the text tests of prune_str, as column predicates */
	int			nspred;
	int		   *spmap;			/* per spred: the test of prune_str it stands for */
	int			spoff;			/* the position of the first spred among the predicates of a begin (col_preds) */
	SiftColWTest *wtest;		/* the conjuncts on wide leaves of set_wide */
	int			nwtest;
	SiftColPred *wpred;			/* the predicates that prune the units by the conjuncts of wtest that can: a wide integer test as it is, a containment test as SIFT_COLOP_SHAPE (col_preds) */
	int			nwpred;
	SiftAggPre	pre;			/* the histogram or rollup of the last unit_hist / unit_rollup */
} ColSource;
#define COL_MAX_WIDE		8		/* the wide conjuncts of a scan (SIFT_AGG_MAX_WIDE) */

extern bool col_overflow(void *impl, const SiftSrcUnit *unit, int leaf);
extern void col_super(void *impl, bool on);
extern uint32 col_super_align(void *impl, uint32 sb);
extern bool col_unit_dset(void *impl, const SiftSrcUnit *unit, int cal, int leaf, int64 cell, SiftSrcPresence *p);
extern void col_super_tests(void *impl, const SiftSrcPrune *z, int nz, int dim, int hdim, uint64 need);
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
extern bool col_rescan(void *impl);
extern void col_tolerate_ovf(void *impl, int leaf);
extern void col_set_raw(void *impl, int leaf);
extern void col_set_numsum(void *impl, int leaf);

extern bool col_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
extern void col_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags);
extern void col_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits);
extern void col_counters(const void *impl, SiftSrcCounters *c);
extern void col_wide_dict(SiftV1Idx *r, int leaf, SiftSrcWide *w, const SiftColVec *v, uint32 n);
extern void col_live(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern bool col_gorder_compact(void *impl, int i, int leaf);
extern bool col_gorder_clean(void *impl, int i);
extern void *col_gorder_groups_open(void *impl, int gleaf, int kleaf, const int64 *only, bool desc, MemoryContext cxt);
extern bool col_gorder_groups_next(void *cursor, int64 *group);
extern bool col_gorder_groups(void *impl, int i, int leaf, uint32 *ngroups);
extern void col_gorder_group_at(void *impl, int i, int leaf, uint32 gi, int64 *group, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne);
extern bool sift_col_scope_stats(Relation index, SiftSrcStats *st);
typedef struct ColZones
{
	const SiftSrcPrune *z;
	int			nz;
} ColZones;

extern bool col_key_range(Relation index, const SiftSrcPrune *z, int nz, SiftKeyRange *out);
extern Size col_view_size(void *impl);
extern void col_view_export(void *impl, void *buf);
extern void *col_open_flags(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags, const void *view, const SiftKeyRange *keys);
extern void *sift_col_open_scope(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view);
struct SiftScopeRead;
extern void sift_col_open_scope_delta(void *impl, const struct SiftScopeRead *rd);
extern const char *sift_col_conn_proof(void *impl, int leaf, uint64 *lineage, uint32 *xmin, bool *clean);
extern void sift_col_miss(Relation index, SiftSrcStats *st);

#endif							/* SIFT_AGG_COL_INT_H */
