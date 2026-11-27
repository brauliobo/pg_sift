/*
 * agg/hybrid/hybrid.h - SiftHybridScan: a structured scan of the columnar store (a sift_col index or a sift_table) and a ranked text scan hand each other the TIDs they keep
 * as a core/tidbridge.h bridge.
 *
 * Direction A (SIFT_HYB_COL_TO_BM25): the structured quals are more selective than the query. The store scan fills the bridge, the ranked search reads only the superblocks
 * and pages of the bridge, and a hit on a lossy page of the bridge is re-checked against the heap before it counts.
 * Direction B (SIFT_HYB_BM25_TO_COL): the query is more selective. The text scan fills the bridge in chunks that fit the byte budget, and the store scan skips the granules
 * whose blocks the bridge misses and keeps the rows of the bridge only; a chunk never goes lossy, so no row is counted twice and none of a hit's score is lost. A query that
 * reads a column the store does not hold (or has a qual it does not decide) still runs B: only the rows the store keeps are fetched from the heap.
 *
 * This file holds the part that is no PostgreSQL: the cost choice, the two drivers and the rules that decide a TID. Portable (core/host.h).
 */
#ifndef SIFT_HYBRID_H
#define SIFT_HYBRID_H

#include "core/host.h"
#include "core/tidbridge.h"

#define SIFT_HYB_WIN	0.9			/* a direction must cost this fraction of the standard plan */

typedef enum SiftHybridDir
{
	SIFT_HYB_NONE = 0,					/* the standard plan wins */
	SIFT_HYB_COL_TO_BM25,
	SIFT_HYB_BM25_TO_COL,
	SIFT_HYB_ADAPTIVE,					/* a parameter leaves the selectivities unknown: the executor takes the cheaper direction (sift_hybrid_pick) once the query is bound; never a direction of a run */
	SIFT_HYB_OR							/* the query is ORed with structured quals: their rows, then the matches they do not take; not a direction of a run */
} SiftHybridDir;

typedef enum SiftHybridHost
{
	SIFT_HYB_HOST_COL = 0,				/* a sift_col index on a heap: the heap decides visibility and may hold HOT chains */
	SIFT_HYB_HOST_TABLE					/* a sift_table: the store decides visibility and hands a row over by the TID of its HOT chain's root, as its indexes do */
} SiftHybridHost;

/* What the planner knows. sel_* are fractions of rows; sel_col is 1 without a structured qual */
typedef struct SiftHybridEst
{
	SiftHybridHost host;
	double		rows;					/* live rows */
	double		granules;				/* granules of the store */
	double		col_granules;			/* granules the skip index leaves for the structured quals (granules when unknown) */
	double		heap_fetch;				/* cost of the random heap fetch of one hit (a cold fraction of random_page_cost): what the visibility of a row costs (of a sift_table SIFT_TABLE_VET_OPS: its store decides it) */
	double		row_fetch;				/* cost of giving one hit as a row: the heap fetch, or the fetch of the row of a sift_table by its TID */
	double		sel_col;				/* rows the structured quals keep, from the skip index and the statistics */
	double		sel_bm25;				/* rows the query matches, from the dictionaries' document frequencies */
	double		limit;					/* LIMIT of a ranked scan, 0 without */
	double		hit_budget;				/* bytes of work_mem the ranked search may hold for the hits no LIMIT bounds, 0 for any */
	bool		ranked;					/* ORDER BY score */
	bool		has_col_quals;
	bool		covered;				/* the store holds every column the query reads */
	double		sel_acl;				/* rows the semi join of a tenant's key set lets through (1 without one); part of sel_col */
	bool		has_residual;			/* a qual the store does not decide stays for the heap */
	bool		agg_mode;				/* the rows are folded into groups, not returned in score order */
	bool		reads_score;			/* agg_mode: sift.score is read; without it the text scan is the unordered bitmap evaluation, which scores nothing */
	bool		probeable;				/* a plan of the standard kind can reach the rows through an index on the key set of the tenant's EXISTS (a custom plan: the set is known) */
	double		probe_rows;				/* ... and the keys of the set it probes */
	double		bm25_dict_cost;			/* the text index's dictionary lookups, which every direction and the standard plan pay */
	double		allvisfrac;				/* fraction of the heap's pages that are all-visible: the rows of those pages need no heap check */
} SiftHybridEst;

typedef struct SiftHybridCosts
{
	double		col_to_bm25;			/* DBL_MAX when the direction does not apply */
	double		bm25_to_col;
	double		standard;				/* the ranked scan with a residual filter and a heap fetch per hit */
} SiftHybridCosts;

/* The fraction of granules of rows_per_granule rows that a selectivity sel leaves when the rows are placed at random */
extern double sift_hybrid_granule_hit(double sel, double rows_per_granule);

/* A direction of cost beats the standard plan by SIFT_HYB_WIN: a hybrid scan costs at least the setup it pays before its first row */
extern bool sift_hybrid_beats(double cost, const SiftHybridCosts *costs);

/* The direction that beats the standard plan by the margin of SIFT_HYB_WIN, SIFT_HYB_NONE when none does; costs (when not NULL) tells why */
extern SiftHybridDir sift_hybrid_choose(const SiftHybridEst *est, SiftHybridCosts *costs);

/* SIFT_HYB_OR: the store scan of the structured quals, a heap fetch of each row it keeps, and the unordered match with a fetch of each of its rows */
extern double sift_hybrid_or_cost(const SiftHybridEst *est);

/* The types whose values of a heap row a folded scan reads one by one (a few hits, the matches of an OR) */
extern bool sift_hybrid_point_type(Oid typid);

/* SIFT_HYB_OR folded into groups: the store scan of the structured quals folds its rows, the matches the quals do not take are read from the heap */
extern double sift_hybrid_or_agg_cost(const SiftHybridEst *est);

/* Both directions apply: the plan cannot tell which is cheaper before the query is bound */
extern bool sift_hybrid_both_apply(const SiftHybridEst *est);

/* SIFT_HYB_ADAPTIVE resolved with the selectivities of the bound query (the document frequencies of its dictionaries, the size of the key set): the cheaper of the two directions that apply */
extern SiftHybridDir sift_hybrid_pick(const SiftHybridEst *est);

/* What EXPLAIN (ANALYZE) shows */
typedef struct SiftHybridStats
{
	SiftHybridDir dir;
	SiftHybridHost host;
	uint64		chunks;					/* bridges built */
	uint64		bridge_tids;			/* TIDs of exact pages, over the chunks */
	uint64		pages_exact;			/* heap pages the bridges hold exactly and whole */
	uint64		pages_lossy;
	uint64		granules_seen;
	uint64		granules_skipped;		/* by the bridge, before any column was read */
	uint64		rows_probed;			/* rows of the surviving granules looked up in the bridge */
	uint64		rows_emitted;
	uint64		bridge_rejects;			/* A: hits of the ranked search that are in no page of the bridge */
	uint64		lossy_rechecks;			/* A: hits on a lossy page, decided by the heap */
	uint64		zero_heap_rows;			/* A: hits accepted without a heap fetch */
	uint64		heap_verifies;
	Size		peak_bytes;				/* the largest bridge */
	bool		point;					/* A: at most SIFT_TIDB_POINT_MAX exact TIDs, read one by one */
	bool		empty;					/* A: no TID, the text scan never ran */
} SiftHybridStats;

/* The rows of a granule (or the slab of a partition) that the structured quals accept: tid[r] is the SiftKey of row r, live a bit per row (NULL: all) */
typedef struct SiftHybridBatch
{
	const uint64 *tid;
	const uint64 *live;
	const float4 *score;				/* B: the score of the row's hit */
	const uint64 *maybe;				/* rows of live the stored values could not decide: the heap's row decides them (NULL: none) */
	int			n;
	uint16		part_slot;				/* where the row is in the store: the row locator of a TID */
	uint32		local_g;
	bool		exact_vis;				/* the store decided visibility of every row of the batch */
	const void *cols;					/* the store's own decoded columns of the batch, handed to the sink as they are */
} SiftHybridBatch;

typedef void (*SiftHybridSink) (void *arg, const SiftHybridBatch *batch);

/* The columnar side. A host fills it in: sift_col (hybrid_exec.c) or a sift_table (the hybrid ops of its store) */
typedef struct SiftHybridStore
{
	SiftHybridHost host;
	bool		hot_chains;				/* the heap may hold HOT chains */
	bool		exact_quals;			/* the store decides every structured qual itself */
	void	   *arg;
	/* A: every row the quals accept */
	void		(*scan_quals) (void *arg, SiftHybridSink sink, void *sink_arg);
	/* B: the rows of the bridge that the quals accept; granules the bridge misses are skipped (sift_tidbridge_granule_may_match) and counted in stats */
	void		(*scan_bridge) (void *arg, const SiftTidBridge *br, SiftHybridSink sink, void *sink_arg, SiftHybridStats *stats);
} SiftHybridStore;

/* The tuple of a TID the heap decides: visible, and with quals the structured quals too */
typedef bool (*SiftHybridHeapCheck) (void *arg, SiftKey key, bool quals);
/* B: clears the bits of the live rows of tid[0..n) the heap does not show, a page of all-visible rows at a time */
typedef void (*SiftHybridBatchCheck) (void *arg, const uint64 *tid, uint64 *live, int n);
/* B: the next hit of the text scan */
typedef bool (*SiftHybridHitFn) (void *arg, SiftKey *key, float4 *score);

typedef struct SiftHybridRun
{
	SiftHybridDir dir;
	const SiftHybridStore *store;
	MemoryContext mcxt;
	Size		budget;
	SiftTidBridge *br;
	SiftHybridStats stats;
	bool		exact_vis;				/* A: every batch was */
	bool		exact_quals;			/* A: the store decided every qual of every row it handed over */
	bool		zero_heap;				/* A: a hit on an exact page needs no heap fetch */
	bool		scores;					/* B: the bridge keeps the scores */
	uint64		chunk_hits;				/* B: hits a chunk takes */
	int64		limit;					/* B: rows the consumer wants, 0 without: it bounds the first chunk */
	SiftHybridHeapCheck heap_check;
	SiftHybridBatchCheck batch_check;
	void	   *check_arg;
} SiftHybridRun;

/* budget_bytes: what the bridge may hold (work_mem / 2); heap_check is needed by a store that does not decide visibility */
extern SiftHybridRun *sift_hybrid_begin(MemoryContext parent, SiftHybridDir dir, const SiftHybridStore *store, Size budget_bytes,
										SiftHybridHeapCheck heap_check, void *check_arg);
extern void sift_hybrid_end(SiftHybridRun *run);
/* B: limit rows are wanted (a chunk of the hits of 4 times as many rows, which grows while the rows fall short), batch_check judges the heap for a whole batch */
extern void sift_hybrid_set_limit(SiftHybridRun *run, int64 limit);
extern void sift_hybrid_set_batch_check(SiftHybridRun *run, SiftHybridBatchCheck check);

/*
 * A: runs the structured scan and finalizes the bridge, which the ranked search takes (sift_plan_set_tidbridge); want_rowloc keeps the locators of the rows;
 * plan_recheck: the ranked search needs the text of a hit (a phrase), so a hit is fetched in any case
 */
extern const SiftTidBridge *sift_hybrid_a_bridge(SiftHybridRun *run, bool want_rowloc, bool plan_recheck);

/* A: the ranked search's own verification of an index entry: in the bridge, then visible and (on a lossy page) passing the structured quals */
extern bool sift_hybrid_a_accept(SiftHybridRun *run, SiftKey key);

/* B: the next chunk of hits as a finalized bridge; false when the text scan has no hit left */
extern bool sift_hybrid_b_chunk(SiftHybridRun *run, SiftHybridHitFn next, void *arg, bool with_scores);

/* B: the rows of the chunk that the store keeps, to the sink (every row visible: the store's or the heap's judgment) */
extern void sift_hybrid_b_emit(SiftHybridRun *run, SiftHybridSink sink, void *sink_arg);

/* The text of EXPLAIN: "A", "B" */
extern const char *sift_hybrid_dir_name(SiftHybridDir dir);

#endif							/* SIFT_HYBRID_H */
