/*
 * col/build/build_int.h - what the files of the index build share
 * the statics one file of the build calls from another
 */
#ifndef SIFT_COL_BUILD_DIR_INT_H
#define SIFT_COL_BUILD_DIR_INT_H

#ifdef SIFT_TESTING
#endif
#include "col/build.h"
#include "col/scope.h"

typedef struct ColSpill ColSpill;
typedef struct ColOpen ColOpen;

/* The key ranges a build cuts its rows into (col/build/ranges.c): range i holds the keys above bound[i - 1] up to bound[i], the last the rest and the NULLs; a single range holds one value of the key */
typedef struct ColRanges
{
	int			n;
	int			leaf;			/* the first leaf of the key */
	const SiftColDef *def;
	int64	   *bound;			/* [n - 1] */
	bool	   *single;			/* [n] */
} ColRanges;

#define SCOPE_ROWS_FACTOR	2		/* the rows a scope family is expected to hold for each row of the table, to fit its segments to the directory (a bound: the average is the keys of a member) */

struct ColBuild
{
	Relation	index;
	SiftColRel *r;
	MemoryContext cxt;
	MemoryContext pagecxt;		/* the rows of the current heap page */
	MemoryContext tmpcxt;		/* one tuple */
	SiftWriteTarget *wt;
	Size		budget;			/* memory a segment's writer may hold */
	Size		spool_budget;	/* key_cut: the memory the chunks of the key ranges' files may hold (the build's budget, or what the open units leave of it) */
	Size		run_cap;		/* key_cut: the most the rows of a run may take (run_budget) */
	uint64		share;			/* key_cut: the rows the participant is expected to scan */
	uint64		held_rows;		/* key_cut: the rows in held */
	Size		trimmed;		/* key_cut: the bytes the trims of the batches in held freed */
	SiftColBatch *batch;		/* the rows of the current superblock, flushed at the next one */
	MemoryContext heldcxt;		/* the batches in held */
	List	   *held;			/* an index with the option key_cut: the batches of the superblocks since the last run, written sorted by the key as a segment when they pass the budget, and at the end (col/order.c) */
	SiftColSegWriter *w;
	SiftColFill *fill;				/* an index with connections or scopes: the found flags of every segment are filled from the parent tables (col/conn.h), and the stamps of the scopes written (col/scope.h) */
	MemoryContext wcxt;			/* the writer of the current segment: freed with it */
	SiftColRow *rows;			/* the current page, deep copies */
	int			nrows;
	int			maxrows;
	BlockNumber page;
	SiftSegEntry *segs;
	uint8	   *lvl;			/* key_cut: how many times the rows of segs[i] were merged (a run is level 0) */
	uint32		aim;			/* key_cut: the aim of the units of the runs, which the merges of them keep */
	ColOpen    *open;			/* key_cut: the rows go to an open unit for each range (col/build/open.c) */
	ColSpill   *spill;			/* key_cut: the rows go to a file for each range of the key (col/build/spill.c) */
	uint64		spill_run_rows;	/* the rows of a run of the ranges: those the first run held, or what fits SPILL_RUN_BYTES of them */
	uint32		spill_aim;		/* the aim of the units of the runs of the ranges: the first run's, whose batches held a superblock's rows */
	int64	   *ksample;		/* key_cut: the sorted keys of the rows of superblocks spread over the table, taken before the scan (col/build/spill.c) */
	uint32		nksample;
	uint32		unit_base;		/* the unit the next run of the ranges starts at */
	bool		spill_final;	/* the ranges are being read back */
	bool		spill_tried;	/* the first full run decided whether the build goes by ranges */
	Oid			btree;			/* key_cut: the btree the heap is read through, in the order of the key (col/build/btree.c) */
	int			nsegs;
	int			segcap;			/* the entries segs has room for */
	bool		segs_fixed;		/* segs is a participant's array in shared memory: it does not grow */
	int			maxruns;		/* key_cut: the runs the build keeps before it merges some (the merge that ends it opens them all) */
	bool		shared;			/* a participant of a parallel build */
	uint64		total_rows;		/* the heap's estimate of the rows of the index */
	uint64		entries;		/* the rows this build writes: those of the index, or the entries of a scope (from the sample of its keys) */
	BlockNumber heap_pages;		/* the pages of the heap when the build began */
	int			nslots;			/* the participants that share the scan */
	uint64		pages_seen;		/* the heap pages this participant has read rows from */
	double		indexed;
	double		reported;
	/* a scope index (col/build/scope.c): the build of the entities feeds the rows of each batch, replicated by the scope keys of their member, to a build of its own for each scope */
	ColBuild   *scope[SIFT_SCOPE_MAX];
	int			nscope;
	struct SiftScopeFill *sfill;
	uint32	   *srow;			/* the expansion of the batch in hand: the row of the batch and the scope key of each entry */
	int64	   *skey;
	uint32		scap;
	int			scope_leaf;		/* a scope's build: the derived key leaf; -1 for the build of the entities */
	bool		metagen;		/* the generations of the segments are the metapage's, as a key-cut build's, though the units of the entities are not cut by key */
};

extern ColBuild *colb_alloc(Relation index, SiftWriteTarget *wt, Size budget, SiftSegEntry *segs, int segcap, bool shared, int nslots, int scope, int nscope);
extern void colb_segs_room(ColBuild *b, int n);
extern uint64	colb_next_gen(ColBuild *b);
extern void colb_scope_cut(SiftColRel *r, int scope);
extern void colb_scope_begin(ColBuild *b);
extern void colb_scope_feed(ColBuild *b, const SiftColBatch *src);
extern void colb_scope_gather(ColBuild *b);
extern void colb_scope_say_rollup(Relation index);
extern void colb_finish_runs(ColBuild *b);
extern Size colb_run_budget(ColBuild *b, Size held);
extern Size colb_writer_budget(int nslots);
extern void colb_flush_batch(ColBuild *b);
extern void colb_add_row(ColBuild *b, const SiftColRow *row);
extern void colb_write_run(ColBuild *b, bool last);
extern void colb_spill_sample(ColBuild *b, Relation heap, IndexInfo *info);
extern bool colb_spill_start(ColBuild *b);
extern void colb_spill_batch(ColBuild *b, const SiftColBatch *batch);
extern void colb_spill_finish(ColBuild *b);
extern int	colb_key_leaf(const SiftColRel *r);
extern int	colb_range_of_key(const ColRanges *g, int64 key);
extern int	colb_range_of_value(const ColRanges *g, const SiftColValue *v);
extern int	colb_range_of_row(const ColRanges *g, const SiftColBatch *batch, uint32 r);
extern bool colb_ranges_cut(ColBuild *b, ColRanges *g, int leaf, int nr, const int64 *sample, uint32 nsample);
extern int	colb_ranges_isolate(ColBuild *b, ColRanges *g, int leaf, int nr, uint64 heavy_rows, int max_single);
extern void colb_begin_segment(ColBuild *b);
extern void colb_end_segment(ColBuild *b);
extern void colb_make_room(ColBuild *b);
extern SiftColSegWriter *colb_writer_begin(ColBuild *b, MemoryContext *wcxt);
extern void colb_writer_end(ColBuild *b, SiftColSegWriter *w, MemoryContext wcxt);
extern bool colb_open_allowed(const ColBuild *b);
extern bool colb_open_forced_zero_temp(void);
extern bool colb_open_zero_temp(const ColOpen *o);
extern void colb_open_notice(const ColBuild *b, const ColOpen *o);
extern Size colb_open_mem(const ColOpen *o);
extern ColOpen *colb_open_plan(ColBuild *b, int leaf, uint32 unit_rows, Size mem, double raw_unit, double packed_unit, int nranges, bool zero_temp);
extern void colb_open_tail(const ColOpen *o, const ColBuild *b, const int64 **sample, uint32 *n, uint64 *entries);
extern bool colb_open_take(ColBuild *b, const SiftColRow *row);
extern void colb_open_split_held(ColBuild *b);
extern void colb_open_finish(ColBuild *b);
extern void colb_open_adopt(ColBuild *b);
extern int64 colb_key_of(const SiftColDef *def, const SiftColValue *v);
extern void colb_report_rows(ColBuild *b);
extern int	colb_spill_leaf_of(const SiftColRel *r, uint64 nblocks, uint64 total_rows, Size budget);

/* A key of the leaf level of a btree, and the entries before it: the boundaries of the ranges of a parallel build */
typedef struct BtBound
{
	Datum		key;
	uint64		before;
} BtBound;

/* The btree a build reads the heap through, what it costs and the keys to split it at (col/build/btree_pick.c) */
/* What a read of the heap costs on its disk, in microseconds (btree_probe.c), and whether it was measured */
typedef struct BtCosts
{
	double		random_us,
				seq_us;
	bool		measured;
} BtCosts;

typedef struct BtPlan
{
	Oid			btree;
	char	   *name;
	BtCosts		cost;
	double		heap_blocks,	/* the blocks of the heap */
				seq_blocks,		/* the blocks it reads in order: the stretches of the heap and the btree's own pages */
				degree,			/* the participants' share of the time of one */
				reads,
				random_reads,
				btree_us,
				spool_us,
				spool_mb,
				break_even_us,	/* the cost of a random read at which both paths take as long */
				room_mb;		/* what the temporary files may take (-1: any size) */
	bool		no_room;		/* the key ranges' files do not fit it: the build would be refused */
	uint64		entries;
	int			nbound;
	BtBound    *bound;
} BtPlan;

/* The rows of the btree to scan: [lo, hi) of the first column, or its NULLs; the units it numbers are unit_base up to unit_limit (0: no limit) */
typedef struct BtRange
{
	bool		has_lo,
				has_hi,
				nulls;
	Datum		lo,
				hi;
	uint32		unit_base,
				unit_limit;
} BtRange;

/* What the walks of the btrees were costed at, once one was walkable: the disk's read costs and the bytes a row takes in the key ranges' files */
typedef struct BtPricing
{
	bool		costed,
				files_priced;
	BtCosts		cost;
	double		row_bytes,
				old_us,			/* in: what the build from the index a REINDEX replaces costs (DBL_MAX: none), and the heap read at its least, whose files are priced only if neither wins */
				heap_floor_us;
} BtPricing;

/*
 * The CPU of a row in each path of a build, in microseconds: the instructions of a row of 13 columns and four path sets (300,000 rows, 3 alternating rounds, perf stat -e
 * instructions:u: 98.9 k, 56.7 k and 27.3 k) over the 11.7 G instructions a second of the lab host (docs/SIZING.md). The heap read with the key ranges' files, the heap read
 * through a btree, the rows of the index a REINDEX replaces merged by key.
 */
#define COLB_HEAP_ROW_US	8.4
#define COLB_BTREE_ROW_US	4.8
#define COLB_OLD_ROW_US		2.3

typedef struct ColOld ColOld;

/* Where the rows of a build come from: the heap (by key ranges when the index is key_cut), the heap in the order of a btree on the key, or the index a REINDEX replaces */
typedef enum ColBuildSource
{
	COLB_HEAP,
	COLB_BTREE,
	COLB_OLD
} ColBuildSource;

typedef struct BuildPlan
{
	ColBuildSource source;
	BtPlan	   *btree;
	ColOld	   *old;			/* the index a REINDEX replaces, when there is one (source says whether the rows come from it) */
	int			participants;	/* the processes that scan: 1 + the workers a parallel build through the btree has */
	MemoryContext cx;			/* the memory of the choice, deleted by colb_choose_end */
} BuildPlan;

extern BuildPlan colb_choose(Relation heap, Relation index, IndexInfo *info, bool heap_only);
extern void colb_choose_end(BuildPlan *plan);

extern ColOld *colb_old_open(Relation heap, Relation index, IndexInfo *info);
extern Relation colb_old_view(const ColOld *o);
extern const char *colb_old_refusal(const ColOld *o);
extern uint64 colb_old_rows(const ColOld *o);
extern BlockNumber colb_old_blocks(const ColOld *o);
extern IndexBuildResult *colb_old_build(Relation heap, Relation index, ColOld *o);
extern void colb_old_close(ColOld *o);

extern BtCosts colb_btree_costs(Relation heap);
extern double colb_seq_us(Relation rel);
extern BtPlan *colb_btree_best(Relation heap, Relation index, IndexInfo *info, List *cands, uint64 tuples, int participants, MemoryContext plan_cx, StringInfo names, BtPricing *price);
extern double colb_spill_price(Relation heap, Relation index, IndexInfo *info, uint64 total_rows);
extern bool colb_spill_fits(double need, double *allowed);
extern List *colb_btree_candidates(Relation index, Relation heap, const IndexInfo *info);
extern IndexBuildResult *colb_btree_build_par(Relation heap, Relation index, IndexInfo *info, const BtPlan *plan, int nworkers);
extern double colb_btree_scan(ColBuild *b, Relation heap, IndexInfo *info, Snapshot snap, Oid btree, const BtRange *range);

#endif