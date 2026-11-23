/*
 * col/f2/scan/scan_int.h - what the files of the scan of an index of format major 2 (col/f2/f2_scan.c, fetch.c) share: the collected addresses of a scan and how it is run.
 */
#ifndef SIFT_COL_F2_SCAN_INT_H
#define SIFT_COL_F2_SCAN_INT_H

#include "postgres.h"

#include "access/relscan.h"
#include "executor/tuptable.h"
#include "nodes/tidbitmap.h"
#include "utils/tuplesort.h"

#include "col/am/preds.h"
#include "col/f2/f2.h"
#include "agg/filter_adapt.h"
#include "col/f2/f2_lazy.h"
#include "col/f2/f2_marks.h"
#include "col/f2/f2_pred.h"
#include "col/f2/delvec.h"
#include "col/f2/f2_sink.h"
#include "col/f2/pend_del.h"
#include "col/granule/granule_filter.h"
#include "col/skip/skip_index.h"
#include "core/tidbridge.h"

#ifdef SIFT_TESTING
#define F2_STAT_ADD(field, n)	(sift_f2_scan_stats.field += (n))
#else
#define F2_STAT_ADD(field, n)	((void) 0)
#endif
#define F2_STAT(field)	F2_STAT_ADD(field, 1)

#define F2_TID_BATCH	4096

/* The addresses of a bitmap scan go to the bitmap (which bounds itself by work_mem) a batch at a time: nothing of the scan is held */
typedef struct F2TbmSink
{
	TIDBitmap  *tbm;
	int			ne;
	int			nm;
	ItemPointerData exact[F2_TID_BATCH];
	ItemPointerData maybe[F2_TID_BATCH];
} F2TbmSink;

typedef struct SiftF2Stream SiftF2Stream;

typedef struct F2Scan
{
	SiftColScanHeader hdr;		/* first: the access method routes the scan by it */
	SiftF2Sink	sink;			/* not NULL: the rows go to it, and nothing is collected */
	void	   *sink_arg;
	SiftF2PartSink part_sink;	/* a sink that is offered each part before it is read (SiftF2PartSink) */
	const SiftPartDesc *part;	/* the part being scanned; NULL for a slab */
	const struct SiftPointCands *cands;	/* the rows of the part being scanned that the point directory names for a predicate: the others are not looked at */
	bool		tids_unneeded;	/* the sink will not read the addresses of the rows of this part */
	const int  *sink_leaves;	/* the leaves the sink reads of a granule: loaded with those of the predicates */
	int			nsink_leaves;
	bool		sink_maybe;		/* the sink takes the rows no predicate could decide on the stored values */
	const bool *sink_dict;		/* per sink leaf: taken as dictionary codes when the chunk has them */
	MemoryContext cx;
	SiftColPred *preds;
	SiftPredSet *psets;			/* the constants of each IN predicate, hashed when the list is long (f2_pred.h) */
	int			npreds;
	SiftF2KeySet keys;
	SiftF2TimeWin time;
	bool		possible;
	bool		prepared;
	bool		collected;
	bool		recheck_all;	/* a qual the predicates only approximate (a NaN, a numeric out of the range of its leaf) */
	TupleDesc	itupdesc;		/* the attributes of the index: what the returned columns of an index-only scan are typed by */
	HeapTuple	htup;			/* the tuple of an index-only scan: the returned attributes, every other NULL (a heap tuple, for an attribute that has no limit of an index tuple's 8 kB) */
	Bitmapset  *want;			/* an index-only scan: the attributes its plan node reads (project), else every one the index can return */
	bool		project;
	int			nret;			/* the attributes an index-only scan returns, and the leaf of each: their values are kept with each address */
	int			ret_att[INDEX_MAX_KEYS];
	int			ret_leaf[INDEX_MAX_KEYS];
	MemoryContext rowcx;		/* the values of one row of the sort, reset after each */
	struct SiftF2Stream *stream;	/* the addresses of an unordered scan that returns no column, until they outgrow work_mem (scan/stream.c) */
	Tuplesortstate *sort;		/* the addresses of an ordered scan, with the values of an index-only scan, in a sort that spills past work_mem; its key is the address and the flag of recheck */
	TupleTableSlot *put;
	TupleTableSlot *get;
	int64		ntids;			/* the addresses the scan found */
	F2TbmSink  *tbm;			/* a bitmap scan: the bitmap it fills */
	SiftCfiltPred *cf;			/* the predicates that can run on a chunk as it is stored: cf_ok says which */
	bool	   *cf_ok;
	SiftLazyFilter lf;			/* the predicates on the chunks in the order the scan learned over its granules, and what they read */
	SiftKeyPlan keyplan;		/* the ranges of the whole key the predicates leave (col/f2/key_plan.h) */
	bool		compiled;
	const SiftTidBridge *bridge;	/* a hybrid scan: the granules it misses are skipped and the rows not in it dropped (hybrid_exec.c counts them) */
	uint64	   *bridge_seen;
	uint64	   *bridge_skipped;
	uint64	   *bridge_parts_skipped;
	const struct SiftScopeRouter *router;	/* a scan for the facts of one scope key: the granules the summary of the scope does not name for it are skipped (f2_scope_route.h) */
	float4	   *bscores;		/* the scores of the rows of the granule or slab last probed against the bridge */
	const SiftColPred *ext_preds;	/* the predicates of a hybrid scan, appended to the index quals' (bridge.c) */
	int			n_ext_preds;
	bool		heap_decide;	/* a sink that takes undecided rows (sift_f2_scan_sink_maybe): the few rows a bridge leaves in a granule are left to the heap, their columns not decoded */
	bool		survey;			/* count the granules the zones and the skipping indexes leave, read none (sift_f2_scan_survey) */
	uint64		survey_granules;
	struct SiftCdiScan *cd;		/* the column delta chain of the partition being read (col/f2/f2_col_delta_idx.h), NULL until a part with one is */
	const uint64 *skip_gens;	/* a scan of the rest of a relation: the parts (by generation, ascending) a projection stands for and the scan leaves out */
	int			nskip_gens;
	const struct SiftPlanSnapshot *pinned_plan;	/* a scan of the rest of a relation a projection executor pins: the manifest it read, so that the served parts and the rest are one partition of one version */
	struct MorselCx *morsel;	/* a participant of a parallel scan: what it keeps between the runs of granules it takes (f2_par.c) */
} F2Scan;

/* What a scan knows of the predicates for one part: the dictionaries of its strings, the skipping indexes, the predicates that run on its chunks */
typedef struct PartPreds
{
	SiftSkipReader *skip;
	SiftSkipQuery **sq;
	SiftCfiltPred *cf;			/* a copy of the scan's, with the dictionary of the part tested once */
	const SiftVariantExcSummary *exc;	/* the leaves of the part that hold a value the chunks cannot judge, with those a column patch rewrites */
	int			nexc;
} PartPreds;

extern const SiftLfOps sift_f2_granule_filter_ops;
/* The rows of the granule that pass every predicate the chunks could tell, and the predicates they decided */
extern uint32 sift_f2_filter_chunks(Relation rel, F2Scan *s, uint32 g, SiftLazyGranule *lg, const SiftGranuleView *v, const PartPreds *pp, const bool *exceptions, bool *decided, uint64 *active);

typedef enum Verdict
{
	V_NO,
	V_YES,
	V_MAYBE
} Verdict;

#define SIFT_F2_HEAP_DECIDE_ROWS	64

/* col/f2/scan/verdict.c */
extern Verdict sift_f2_heap_verdict(const F2Scan *s, const bool *decided);
extern bool sift_f2_scan_few_rows(const uint64 *active, int n);
extern Verdict sift_f2_row_verdict(const SiftColSchema *schema, const F2Scan *s, const ColView *views, const bool *decided, int r);
extern void sift_f2_sink_verdict(const F2Scan *s, Verdict v, uint64 *live, uint64 *maybe, int r);

/* col/f2/scan/bridge.c */
extern bool sift_f2_scan_bridge_part_skip(F2Scan *s, const SiftPartitionEntry *e, const SiftPartDesc *d);
extern bool sift_f2_scan_bridge_granule_miss(const F2Scan *s, uint32 g, const SiftTidIndexEntry *tix);
extern int	sift_f2_scan_bridge_probe(Relation rel, F2Scan *s, uint32 g, SiftLazyGranule *lg, const SiftGranuleView *v, const SiftDvGran *dg, const SiftPendDelIndex *px, bool pend_hit,
									  Snapshot snap, uint64 *active);
extern uint64 *sift_f2_scan_slab_bridge(F2Scan *s, const uint64 *tids, int n);
extern int	sift_f2_scan_append_ext(F2Scan *s, int kept);

extern void sift_f2_scan_prepare(IndexScanDesc scan, F2Scan *s);
extern void sift_f2_scan_collect(IndexScanDesc scan, F2Scan *s);
extern void sift_f2_scan_sort_put(F2Scan *s, const SiftColSchema *schema, uint64 tid, bool maybe, const ColView *views, int r);
extern void sift_f2_scan_sort_end(F2Scan *s);
extern HeapTuple sift_f2_returned_tuple(IndexScanDesc scan, F2Scan *s);

/* A row handed to a sink: decided yes, or undecided for a sink that takes those; any other sink counts only what the stored values decided, so it is an error for it */
static inline void
sink_verdict(const F2Scan *s, Verdict v, uint64 *live, uint64 *maybe, int r)
{
	if (v == V_MAYBE && !s->sink_maybe)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a qual of the aggregate cannot be decided on the stored values"),
						errhint("Run the statement with sift.enable_agg = off: the heap decides such a qual.")));
	live[r >> 6] |= (uint64) (v == V_YES) << (r & 63);
	maybe[r >> 6] |= (uint64) (v == V_MAYBE) << (r & 63);
}

#endif
