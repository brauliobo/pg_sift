/*
 * agg/hybrid_state.h - the state the executors of SiftHybridScan (hybrid_exec.c) and SiftHybridAgg (hybrid_agg_exec.c) share, and the steps they take the same way (hybrid_store.c): the
 * indexes and the quals opened, the right hand sides of the query and of the quals bound, the direction chosen once the query is known, the checks the heap makes of a TID.
 */
#ifndef SIFT_HYBRID_STATE_H
#define SIFT_HYBRID_STATE_H

#include "postgres.h"

#include "access/genam.h"
#include "access/skey.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "storage/buffile.h"

#include "agg/hybrid_int.h"
#include "agg/hybrid_quals.h"
#include "bm25/planner.h"
#include "col/am.h"
#include "core/store_ops.h"

typedef enum HybridColKind
{
	HC_VAR,
	HC_CTID,
	HC_SCORE
} HybridColKind;

typedef struct HybridCol
{
	HybridColKind kind;
	int			attno;				/* heap column of a HC_VAR */
	Oid			typid;
	int			leaf;				/* B: its leaf in the store, -1 when the store does not give the column back */
} HybridCol;

typedef struct HybridRow
{
	SiftKey		key;
	float4		score;
	Datum	   *vals;
	bool	   *nulls;
	bool		need_heap;			/* a value the leaf could not hold: this row is read from the heap */
} HybridRow;

typedef struct HybridState
{
	CustomScanState css;
	SiftHybridDir dir;
	SiftHybridHost host;
	Oid			text_oid,
				store_oid;
	int64		k;
	bool		ranked,
				ties,
				scored;				/* a column of the scan is sift.score */
	ExprState  *query;
	List	   *std_quals;			/* the index quals, over the columns of the store's index */
	List	   *ext;				/* SiftHybExt of the others */
	ExprState  *heap_quals;
	List	   *scan_tlist;			/* custom_scan_tlist: the columns of the scan tuple the clauses of the heap's below read */
	ExprState  *residual_state;		/* SHQ_RESIDUAL clauses, decided on the heap's row (loaded as the scan tuple) */
	ExprState  *exists_state;		/* SHQ_EXISTS clauses */
	int			nexists,
				hints_taken;		/* the key sets the store took of the nhint it was given */
	bool		hints_exact;		/* every EXISTS is its key set: the store decided them */
	ExprContext *qual_econtext;
	Relation	heap,
				text_index,
				store_index;		/* a sift_col: its index; NULL for a sift_table */
	const SiftStoreOps *ops;	/* the format of the store */
	bool		is_table;		/* a sift_table: its store decides visibility and takes the quals as predicates */
	SiftStoreSchema tsch;		/* of that store */
	struct SiftStoreQuals *table_quals;
	IndexInfo  *iinfo;
	ScanKey		keys;
	int			nkeys;
	ScanKey		hint_keys;		/* the key sets of the tenant EXISTS: taken when they help (hybrid_semi_plan.c) */
	int			nhint,
				nhrt;
	List	   *hint_quals;
	IndexRuntimeKeyInfo *hint_rtkeys;
	ScanKey		scan_keys;		/* the keys the store takes: those of keys but the sets wider than SIFT_HYB_ACL_PUSH_MAX */
	int			nscan;
	double		key_scale;		/* the values of the key sets the store takes (all the rows for one it does not) over the HYB_ARRAY_GUESS values the planner assumed of an array it cannot see */
	int			ncols;
	HybridCol  *col;
	int		   *leaves;
	int			nleaves;
	bool		scan_columns;	/* the scan of the quals hands the columns of the leaves to its sink too (SIFT_HYB_OR folded into groups), not only the scan of a bridge */
	MemoryContext cxt;
	TupleTableSlot *heapslot;
	bool		started;
	bool		adaptive;
	bool		exact_quals;		/* the store decided every qual for the values it got */
	bool		resolved;			/* an adaptive scan has taken its direction */
	bool		covered;			/* the store gives back every column the query reads */
	bool		heap_decide;		/* B: a granule with few rows of the bridge is not decoded: the heap decides them */
	bool		fetch_heap;			/* the rows are read from the heap: a column the store does not give back, or a qual it did not decide */
	bool		empty;				/* a right hand side is NULL: no row */
	ExprContext *rt_econtext;		/* the right hand sides are evaluated in it: kept until a rescan, which ExecScan's context is not */
	IndexRuntimeKeyInfo *rtkeys;
	int			nrt;
	SiftColPred *ext_preds;
	int			n_ext_preds;
	uint64		parts_skipped;
	Buffer		vmbuf;
	double		est[SIFT_NHYBE];
	bool		residual;
	SiftIndexState *ist;
	SiftQuery  *bound_query;
	SiftPlan   *plan;
	SiftRanker *rank;
	SiftMatchKeys matches;			/* B without a score to read: the matches in heap order */
	SiftVis		vis;
	bool		vis_open;
	SiftHybridStore store;
	SiftHybridRun *run;
	const SiftTidBridge *bridge;
	bool		fetched;
	ItemPointerData fetched_tid;
	ItemPointerData out_tid;
	HybridRow  *rows;
	int			nrows,
				rows_cap,
				pos;
	MemoryContext chunk_cxt;
	bool		chunks_done;
	uint64		granules_seen,
				granules_skipped;
	BufFile    *or_keys;		/* SIFT_HYB_OR: the keys the structured quals took, read back before the matches */
	bool		or_matches;		/* SIFT_HYB_OR: the keys are all read */
	bool		point;			/* B with at most SIFT_TIDB_POINT_MAX hits: no store scan */
	uint64		point_verifies;
	SiftHybridStats stats;
} HybridState;

/* The structured quals of the scan leave a store scan to do: an A scan with none has nothing to start from */
extern bool sift_hybrid_has_store_quals(const HybridState *st);

/* B: clears the bits of the live rows of tid[0..n) the heap does not show, a page of all-visible rows at a time (a SiftHybridBatchCheck) */
extern void sift_hybrid_visible_batch(void *arg, const uint64 *tid, uint64 *live, int n);

/* The rows the store hands over are exactly those that pass every qual of the scan: no qual is left to the heap */
extern bool sift_hybrid_rows_exact(const HybridState *st);

/* Opens the indexes and the quals of the CustomScan whose custom_private and custom_exprs are laid out as agg/hybrid_int.h says; heap is the table the scan reads */
extern void sift_hybrid_open(HybridState *st, CustomScan *cs, EState *estate, Relation heap);

/* Closes what sift_hybrid_open opened */
extern void sift_hybrid_close(HybridState *st);

/* The scan starts: the right hand sides of the quals and the query are bound, the text plan made, the key sets taken; false when a value leaves no row (st->empty) */
extern bool sift_hybrid_bind(HybridState *st);

/* The direction the bound query makes cheaper; agg: the rows are folded, no score is read */
extern SiftHybridDir sift_hybrid_pick_direction(HybridState *st, bool agg);

/* Ends the text scan, the visibility checks and the runs of a started scan */
extern void sift_hybrid_finish(HybridState *st);

/* The verifications of the ranked search, the next hit of the text scan and the heap's checks of a TID */
extern bool sift_hybrid_verify_a(void *arg, ItemPointer tid, bool want_doc, text **doc);
extern bool sift_hybrid_verify_b(void *arg, ItemPointer tid, bool want_doc, text **doc);
extern bool sift_hybrid_verify_point(void *arg, ItemPointer tid, bool want_doc, text **doc);
extern bool sift_hybrid_verify_rest(void *arg, ItemPointer tid, bool want_doc, text **doc);
/* SIFT_HYB_OR: the heap shows the version of tid (kept in the heap slot) and the structured quals do not take it: the match alone does */
extern bool sift_hybrid_rest_row(HybridState *st, ItemPointer tid);
extern bool sift_hybrid_next_hit(void *arg, SiftKey *key, float4 *score);
/* The text plan of a scan that reads no score and wants no order: the matches in heap order, unless the plan needs the text of a hit (a phrase) */
extern void sift_hybrid_unscored_plan(HybridState *st);
extern bool sift_hybrid_heap_check(void *arg, SiftKey key, bool quals);
extern bool sift_hybrid_visible_only(void *arg, SiftKey key, bool quals);
extern ItemPointerData sift_hybrid_tid_of(SiftKey key);

#endif							/* SIFT_HYBRID_STATE_H */
