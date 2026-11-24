/*
 * agg/v6/scan_int.h - the state of the scan of an index of format major 2 or of a sift_table (scan.c): the columns, bins and derived vectors of one batch, and the context the scans of the parallel
 * participants and the folds share with it.
 */
#ifndef SIFT_COL_F2_V6_SCAN_INT_H
#define SIFT_COL_F2_V6_SCAN_INT_H

#include "postgres.h"

#include "agg/v6/v6.h"
#include "agg/v6/group_int.h"
#include "col/f2/f2_v6_scan.h"
#include "agg/v6/v6_bqual.h"
#include "col/f2/f2_v6_colstat.h"
#include "agg/v6/v6_datebin_win_pg.h"
#include "agg/v6/v6_expr_str.h"
#include "agg/v6/v6_pg_expr.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_rows_topn.h"
#include "agg/v6/v6_vet.h"
#include "col/f2/f2_scope_route.h"

typedef struct V6Col
{
	int			leaf;
	int			attno;
	int			scale;
	bool		json;
	Oid			typid;
	SiftV6Class cls;
	bool		nan_ok;			/* a numeric whose every use is the argument of an aggregate that folds a NaN (sift_v6_agg_takes_nan) */
	SiftVec		vec;
	uint64		valid[SIFT_VEC_WORDS];
	uint64		nan[SIFT_VEC_WORDS];
	bool		key_only;		/* a key and no argument: a text of it can stay the codes of its dictionary */
	bool		by_code;		/* the argument of COUNT(DISTINCT) and of nothing else: a part that codes the column by a dictionary gives the codes of its rows (vec.dict is then this dict) */
	SiftDictSrc dict;			/* the dictionary of the part, for the batch in hand when vec.type is SVEC_DICT32 */
	SiftKeyDict kdict;			/* the same for a key column: the dictionary of the batch, and the part it is the dictionary of */
} V6Col;

/* a key that is date_bin() of a column, or its date_trunc('month'): the batch of the column binned into out, as the vector of the key */
typedef struct V6Bin
{
	int			key;
	bool		month;
	SiftDateBin bin;
	const SiftVec *src;
	int64	   *out;
	SiftVec		vec;
} V6Bin;

/* a key or argument computed from columns (SiftV6Derive): its vector for the batch */
typedef struct V6Derived
{
	const SiftV6Derive *d;
	const SiftVec *src;
	SiftVec    *cond[SIFT_V6_MAX_CASE];
	const struct V6Derived *prefix;	/* SVD_NUM_EXPR: the derivation of the first factors of this one, whose vector it multiplies */
	SiftV6PgExpr *pg;			/* SVD_PG_EXPR: the evaluator, and the vectors of the columns it reads */
	const SiftVec *pgin[SIFT_V6_MAX_COLS];
	SiftVxOut	out;
} V6Derived;

struct SiftV6Scan
{
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6Rows *rows;			/* a row Top-N: the kept rows go here instead of the group */
	SiftV6BatchFn emit;			/* where the batches go instead of the group: the engine of a participant of a parallel run */
	void	   *emit_arg;
	SiftV6ScanStats *stats;
	SiftV6Vet	vet;
	int			ncols;
	V6Col	   *cols;
	SiftVec   **keyvec;
	SiftVec   **argvec;
	int			nbins;
	V6Bin	   *bins;
	int			nderived;
	V6Derived  *derived;
	Relation	host;			/* the index, or the sift_table, the scan reads: the columns of the quals the keys do not say are those of its input */
	SiftV6Bq   *bq;				/* those quals (SiftV6Spec.vqual), run over every batch */
	SiftBatchSel bs;
	MemoryContext batch_cx;
	bool		count_only;
	bool		minmax_only;	/* every aggregate is a MIN or MAX of an integer column and nothing else is asked: the zones of a part answer it */
	const SiftPartDesc *visible_part;	/* the part whose rows part_offer found all-visible: its batches are not vetted */
	V6VisMap	vis;
	V6ColStat  *stat;			/* an ungrouped aggregate of integer columns: the totals of the parts it has read whole */
	SiftV6Pipe *pipe;			/* the dimensions of a star join (v6_pipe.h) */
	SiftVec   **fact;			/* the vectors of the columns, for the pipe */
	const SiftClause *gate;		/* an arm of a pass over the union of the keys of its arms: the rows it takes */
	const SiftScopeRouter *router;	/* a scan for the facts of one scope key: the granules the summary of the scope does not name for it are not read (f2_scope_route.h) */
	bool		all_visible;	/* the run of granules being read is of a frozen part with no deletion: its rows need no look at the heap */
	IndexScanDesc scan;			/* a scan that is begun apart from its run (sift_v6_scan_begin) */
	int		   *leaves;
	SiftV6Recheck recheck;		/* a fold: whether the heap keeps a row the stored values could not decide */
	void	   *recheck_arg;
	int			nstore;			/* a fold: the columns that are a column of the index; the others (the score) are the caller's */
	SiftV6FoldCol *pub;
};

typedef struct SiftV6Scan V6Scan;

#endif							/* SIFT_COL_F2_V6_SCAN_INT_H */
