/*
 * agg/scan_int.h - the state of the scan of an index of format major 2 or of a sift_table (scan.c): the columns, bins and derived vectors of one batch, and the context the scans of the parallel
 * participants and the folds share with it.
 */
#ifndef SIFT_COL_F2_V6_SCAN_INT_H
#define SIFT_COL_F2_V6_SCAN_INT_H

#include "postgres.h"

#include "agg/engine.h"
#include "agg/group/int.h"
#include "col/f2/agg/scan.h"
#include "col/f2/agg/unnest/run.h"
#include "agg/qual/bqual.h"
#include "col/f2/agg/colstat.h"
#include "agg/win/datebin_pg.h"
#include "agg/expr/str.h"
#include "agg/expr/pg.h"
#include "agg/pipe.h"
#include "col/f2/agg/preagg.h"
#include "agg/topn/rows.h"
#include "agg/vet.h"
#include "col/f2/scope/route.h"
#include "col/f2/store/scan.h"

typedef struct V6Col
{
	int			leaf;
	int			attno;
	int			scale;
	bool		json;
	bool		jtype;			/* json: the column is the JSON type of the key (SiftV6Col.jtype), not its value */
	bool		jsonb_cast;		/* json: SiftV6Col.jsonb_cast, a value of the exception that is no number raises */
	uint8		jbase;			/* jtype: the SIFT_COLTAG_* of a row that holds a value of the leaf (a number, a string, a boolean) */
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
	const SiftColDef *def;		/* a DATUM column: its head leaf (the leaves after it that its value is made of are lane columns) */
	int16		typlen;			/* a DATUM column: the length and the passing of its type */
	bool		typbyval;
	bool		bare;			/* is_table, and the cell holds the bare bytes of the value (sift_f2_type_bare_bytes), not its datum image */
	bool		is_table;		/* the leaf is a cell of a sift_table: the datum of the attribute, not the bytes a sift_col index or the path of a document holds */
	bool		lane;			/* a leaf the datum of the column before it is made of: it is decoded with that one, and has no vector of its own */
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
	SiftV6Preagg *preagg;		/* the cubes of the granules a part keeps (v6_preagg.h) */
	int			ncols;
	V6Col	   *cols;
	SiftVec   **keyvec;
	SiftVec   **argvec;
	int			nbins;
	V6Bin	   *bins;
	int			nderived;
	V6Derived  *derived;
	bool		is_table;		/* the host is a sift_table: the cells of its leaves are the datum of the attribute */
	Relation	host;			/* the index, or the sift_table, the scan reads: the columns of the quals the keys do not say are those of its input */
	SiftV6Bq   *bq;				/* those quals (SiftV6Spec.vqual), run over every batch */
	SiftBatchSel bs;
	MemoryContext batch_cx;
	bool		count_only;
	bool		minmax_only;	/* every aggregate is a MIN or MAX of an integer column and nothing else is asked: the zones of a part answer it */
	const SiftPartDesc *visible_part;	/* the part whose rows part_offer found all-visible: its batches are not vetted */
	V6VisMap	vis;
	V6ColStat  *stat;			/* an ungrouped aggregate of integer columns: the totals of the parts it has read whole */
	const SiftColDef *defs;		/* the leaves of the index or table, by id */
	SiftV6Pipe *pipe;			/* the dimensions of a star join (v6_pipe.h) */
	SiftV6UnnestRows *unnest;	/* a table whose array column is unnested: the pairs of each row (f2_v6_unnest_run.h) */
	const int  *first;			/* the first leaf of each attribute of the host */
	SiftVec   **fact;			/* the vectors of the columns, for the pipe, and for a row Top-N the heap addresses of the rows (tidvec) after them */
	SiftVec		tidvec;
	const SiftClause *gate;		/* an arm of a pass over the union of the keys of its arms: the rows it takes */
	const SiftScopeRouter *router;	/* a scan for the facts of one scope key: the granules the summary of the scope does not name for it are not read (f2_scope_route.h) */
	bool		all_visible;	/* the run of granules being read is of a frozen part with no deletion: its rows need no look at the heap */
	IndexScanDesc scan;			/* a scan that is begun apart from its run (sift_v6_scan_begin) */
	SiftStoreScan *store_scan;	/* the same for a sift_table, which has no index to scan */
	bool		unsatisfiable;	/* a sift_table whose quals no row satisfies */
	int		   *leaves;
	SiftV6Recheck recheck;		/* a fold: whether the heap keeps a row the stored values could not decide */
	void	   *recheck_arg;
	int			nstore;			/* a fold: the columns that are a column of the index; the others (the score) are the caller's */
	SiftV6FoldCol *pub;
	struct SiftV6ParScan *par;	/* the granules of the scan that the compute threads of the backend fold (agg/par/run.h) */
	struct SiftV6PinRun *pin;	/* the same for the statements of par/pinrun.h */
	SiftF2GranCounts grans;		/* the granules of the index scan, added to stats after each pass and morsel */
};

typedef struct SiftV6Scan V6Scan;

/* f2_v6_scan.c: what the scan of a sift_table (f2_v6_scan_table.c) shares with the scan of an index */
extern void sift_f2_v6_scan_setup(V6Scan *s, const SiftColDef *defs, const int *first, struct SiftV6StarRun *star);
extern int *sift_f2_v6_scan_leaves(const V6Scan *s);
extern bool *sift_f2_v6_scan_nan_leaves(const V6Scan *s, int nleaves);
extern void sift_f2_v6_sink(void *arg, const SiftF2Batch *b);
/* The granules the scan counted since the last call, added to its stats */
extern void sift_f2_v6_flush_grans(V6Scan *s);
extern int	sift_f2_v6_slot_of(V6Scan *s, const int *first, const SiftV6Col *c, bool takes_nan);
extern void sift_f2_v6_derive_columns(V6Scan *s);
extern void sift_f2_v6_bin_keys(V6Scan *s);
/* The rows of the slice of n rows from off that are live and visible, in s->bs */
extern uint16 sift_f2_v6_vet_slice(V6Scan *s, const uint64 *live, const uint64 *tid, bool visible, int off, int n);
extern bool sift_f2_v6_batch_visible(const V6Scan *s, const SiftF2Batch *b);
/* f2_v6_scan_table.c */
extern void sift_f2_v6_scan_table_morsels(V6Scan *s, Relation rel, const SiftStoreQuals *quals, Snapshot snap);
extern void sift_f2_v6_scan_table_run(V6Scan *s, Relation rel, const SiftStoreQuals *quals, Snapshot snap, const uint64 *skip, int nskip, struct SiftV6StarRun *star,
								  SiftV6ChunkFn next_chunk, void *chunk_arg);

/* f2_v6_scan_fill.c: the vectors of the columns of a batch from the decoded views of its leaves */
/* The view of the text of row of the column: the bytes stay where the view of the store holds them */
extern SiftStrView sift_f2_v6_text_view(const ColView *cv, int row, bool table);
extern void sift_f2_v6_fill_col(V6Col *c, const ColView *cv, int off, int n, uint64 part_gen, const uint64 *mask);
extern uint64 **sift_f2_v6_fill_cols(V6Scan *s, const ColView *views, const float4 *score, uint64 part_gen, int off, int n);
extern void sift_f2_v6_raise_kept(V6Scan *s, const ColView *views, uint64 *const *raises, int off, int n);
extern void sift_f2_v6_refuse_nans(const V6Scan *s);
/* f2_v6_datum.c: the datums of a column of a type no native vector holds: the rows of mask (all n when it is NULL) made in cx */
extern void sift_f2_v6_fill_datum(V6Col *c, const ColView *views, int off, int n, MemoryContext cx, const uint64 *mask);
/* the values of the exceptions of the rows the batch keeps; the rows whose cast raises, NULL: none */
extern uint64 *sift_f2_v6_fill_exceptions(V6Scan *s, V6Col *c, const ColView *cv, int off, int n);
/* jsonb_typeof of the key of a leaf: the name of the type each row's chunk keeps (a value of the leaf's own type, an exception's tag, a JSON null), NULL for a row with no value for the key */
extern void sift_f2_v6_fill_type(V6Col *c, const ColView *cv, int off, int n);
extern void sift_f2_v6_fill_score(V6Col *c, const float4 *score, int off, int n);

#endif							/* SIFT_COL_F2_V6_SCAN_INT_H */
