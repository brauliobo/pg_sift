/*
 * bm25/cscan_int.h - shared between cscan.c and cscan_keyed.c.
 */
#ifndef SIFT_CSCAN_INT_H
#define SIFT_CSCAN_INT_H

#include "nodes/extensible.h"
#include "utils/sortsupport.h"

#include "bm25/eval.h"
#include "bm25/planner.h"

typedef enum SiftColKind
{
	COL_VAR,					/* heap column attno (0: whole row, < 0: system column) */
	COL_SCORE,
	COL_MAX_SCORE,
	COL_COUNT
} SiftColKind;

typedef struct SiftCol
{
	SiftColKind kind;
	int			attno;
} SiftCol;

typedef struct SiftTotals
{
	int64		sb_scanned;
	int64		blocks_scored;
	int64		blocks_skipped;
	int64		candidates;
	int64		verified;
	int64		heap_checks;
	int64		heap_pages;
	int64		filter_sbs;			/* pushdown: what the bitmaps kept, summed over the scans */
	int64		filter_exact;
	int64		filter_lossy;
} SiftTotals;

/* Heap pages of an unordered scan read ahead, and the gap from the previous page that is worth a read of its own */
#define PREFETCH_PAGES	16
#define PREFETCH_GAP	8
/*
 * A match of a keyed scan that is among the rows of the best leading keys: its index entry, its keys (those
 * of the boundary row for the rows tied with it) and the score the ranker gives it
 */
typedef struct KeyRow
{
	ItemPointerData tid;
	float4		score;
	Datum	   *key;
	bool	   *isnull;
} KeyRow;

typedef struct SiftScanState
{
	CustomScanState css;
	SiftScanMode mode;
	Oid			indexoid;
	Oid			heapoid;
	int64		k;
	List	   *queries;			/* ExprState */
	bool	   *query_any;			/* per query: an array of queries, ORed (body @@> ANY (array)) */
	ExprState  *quals;				/* residual quals over the scan tuple */
	ExprState  *cuts;				/* cutoffs of the score, over the scan tuple of a ranked row */
	Relation	heap;
	bool		heap_opened;
	Relation	index;
	IndexInfo  *iinfo;
	TupleTableSlot *heapslot;
	int			ncols;
	SiftCol    *col;
	MemoryContext cxt;				/* everything of one scan */
	MemoryContext tmpcx;			/* one candidate's text */
	bool		started;
	bool		empty;				/* a query is NULL: no rows */
	bool		done;				/* count: row returned */
	bool		need_heap;
	SiftIndexState *st;
	SiftPlan   *plan;
	PlanState  *bitmap;				/* ranked pushdown: bitmap of the tuples the other quals keep */
	double		gate,				/* the bitmap is built when the query matches between gate and gate_to documents */
				gate_to;
	SiftFilter *filter;
	SiftMatchIter *it;
	SiftRanker *rank;
	SiftVis		vis;
	bool		vis_open;
	float4		max_score;
	SiftPageMatch pm;
	SiftPageMatch ahead[PREFETCH_PAGES];	/* matches read from the iterator, whose heap pages are being read */
	int			ahead_pos;
	int			ahead_n;
	BlockNumber ahead_blk;				/* last page read from the iterator */
	bool		ahead_done;
	int			wd;
	uint64		mask;
	uint32		vm_sb;
	uint64		allvis[SIFT_PAGEMASK_WORDS];
	int64		nsearches;
	SiftTotals	tot;
	SiftPar		par;				/* parallel-aware plan run with workers: par.sh is the shared state */
	/* keyed scan: by the leading sort keys, then by score */
	int			nkeys;				/* 0: by score alone */
	ExprState **keyexpr;
	SortSupportData *keysort;
	int16	   *keylen;
	bool	   *keybyval;
	MemoryContext kcx;				/* the rows of the current round */
	KeyRow	  **krows;				/* in order of keys, score and entry */
	int64		nkrows,
				kpos,				/* rows handed up */
				kheld,				/* the best keys' rows the current round keeps, besides the ties of the last */
				kpasses;			/* rounds so far */
	bool		kcomplete;			/* the round holds every match */
} SiftScanState;

extern TupleTableSlot *fill_scan_slot(SiftScanState *ss, float4 score);
extern bool scan_verify(void *arg, ItemPointer tid, bool want_doc, text **doc);
extern void reset_iter(SiftScanState *ss);
extern TupleTableSlot *next_row(SiftScanState *ss, ItemPointer entry);
extern void keyed_search(SiftScanState *ss);
extern TupleTableSlot *keyed_next(SiftScanState *ss);

#endif
