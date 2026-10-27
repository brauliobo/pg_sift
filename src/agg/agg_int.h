/*
 * agg/agg_int.h - the state of the executor of agg/agg.c and what its parts share
 */
#ifndef SIFT_AGG_AGG_INT_H
#define SIFT_AGG_AGG_INT_H

#include "executor/nodeAgg.h"
#include "utils/sortsupport.h"
#include "agg/back.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/spec.h"
#include "agg/topn.h"
#include "core/visibility.h"

#include "agg/leaf_int.h"
#include "agg/tup_int.h"
#include "agg/pred_int.h"
#include "agg/accum_int.h"
#include "agg/group_int.h"
#include "agg/dist_int.h"
#include "agg/osa_int.h"
#include "agg/lookup_int.h"
#include "agg/combo_int.h"
#include "agg/two_int.h"
#include "agg/gen_int.h"
#include "agg/gen_run_int.h"
#include "agg/unit_int.h"
#include "agg/init_int.h"
#include "agg/update_int.h"
#include "agg/blob_int.h"
#include "agg/spill_int.h"
#include "agg/par_int.h"
#include "agg/fuse_int.h"
#include "agg/back_host_int.h"

#define VEC			SIFT_AGG_VEC
#define VECW		SIFT_AGG_VECW
#define PADW		8
#define MAX_RANGE	(1 << 22)		/* values a range table covers */
#define ZONE_SET_CAP	(1 << 16)	/* members of an IN list that travel with its zone test, at most: the point directory of the source answers a list of any length up to this */
#define SPARSE_RANGE	(1 << 16)	/* a range table of more values than this is not worth filling for the few rows of a unit that are spread over it */
#define SPARSE_FACTOR	16			/* values added to the range table per row of the unit above which the keys are put in the hash table */
#define DIRECT_MAX	(1 << 20)		/* groups of the direct arrays */
#define DIRECT_SPARSE	16			/* the direct arrays (above PRESIZE_MIN slots, and 4 slots for each group expected) hold at most this many slots for each group in use, else the hash table: their size multiplies the capacity of the keys, not the groups */
#define PRESIZE_MIN		4096		/* groups the planner expects above which a single key starts with room for them */
#define SINGLE_DIRECT_MAX	(1 << 28)	/* a single key's ids are consecutive, so its arrays have no unused slots: only the memory limit caps them */
#define LANE_DIRECT_MAX	(1 << 15)	/* a lane is built and folded or dumped sixteen times: sparse group spaces go to the hash table */
#define NDICT_SLOTS	8

typedef struct Touch
{
	uint32		arm;
	uint32		g;
} Touch;

typedef struct AggExec
{
	CustomScanState css;
	SiftAggSpec *spec;
	Relation	heap;
	bool		heap_opened;
	Relation	index;
	SiftSrcSource *src;
	SiftSearch *search;			/* the BM25 match set the rows are restricted to (spec->search) */
	SiftTopN   *topn;			/* ORDER BY .. LIMIT n: the scan keeps the best rows instead of groups (spec->topn) */
	const SiftSrcLayout *lay;
	int			nbase;			/* leaves of the source: the ones after are SiftAggSpec.vleaf */
	struct
	{
		int			key;		/* leaves of the object: its "key" and "value" */
		int			val;
		const void *dict;		/* the dictionary of the last unit and the code of the key in it */
		uint32		code;
	}			vl[SIFT_AGG_MAX_VLEAVES];
	MemoryContext cxt;			/* the scan */
	MemoryContext tcxt;			/* the group table: keys, groups, aggregate states (reset by table_reset) */
	MemoryContext ucxt;			/* one unit */
	ExprContext *lutcx;
	PgAgg	  **pgagg;			/* per aggregate: SAA_M_PG */
	AggState   *pgstate;		/* the context the transition functions see, their states live in pgstate->curaggcontext's memory */
	ParamExecData lutparams[SIFT_AGG_MAX_PARAMS];
	bool		null_input;
	ComboSt    *cs;
	const uint64 *alive;		/* the items of the batch a predicate is evaluated on whose expressions PostgreSQL may evaluate (NULL: all) */
	uint32	   *calive;			/* output rows of the combo for the tuples an aggregate with a FILTER takes */
	FmgrInfo  **xcmp;			/* SAA_X_MIN, SAA_X_MAX: the comparison of the type, and the length and pass-by-value of the aggregate's type */
	int16	   *xlen;
	bool	   *xbyval;
	int		   *clbase;	/* per combo: its first dictionary map slot (after the keys and aggregates) */
	int			ncl;
	int			nj;				/* join combos, in the order the stages apply them */
	int		   *jcombo;
	MemoryContext comcxt;			/* temporaries of one combo entry */
	ArmSt	   *arm;
	SpineSt    *spine;			/* the rows of the arms' spine (SiftAggSpec.spine) */
	struct QueryDesc **wchild;	/* a worker: the executor state of the plan of each correlated join (the leader's is in the EState it runs in) */
	struct QueryDesc *wsub;		/* a worker: the executor state whose sub-plans the expressions of the spec start (the leader's is the node's EState) */
	struct ChildMemo **cmemo;	/* per join: the rows of its correlated child by parameter values, shared with the other processes of the scan (NULL: none) */
	TupleTableSlot **cmslot;	/* per join: a slot of the child's row type that reads the minimal tuples of the memo */
	const char *spine_buf;		/* the spine's plan rows, serialized by the leader before the scan (spine_fetch): a worker reads them from the shared memory */
	uint32		arm_mask;		/* the keys the tuples are grouped by: all, or an arm's, or a grouping set's */
	int			curset;			/* the grouping set being aggregated */
	uint32		gen;			/* changes when the group indexes are renumbered */
	uint32	   *setg;			/* per grouping set: the outer group of the cell being flushed */
	Touch	   *touched;		/* the cells of the current row */
	uint32		ntouch;
	uint32		captouch;
	uint64		cur_order;
	PredState  *ps;
	KeyState   *ks;
	DictSlot	dslot[NDICT_SLOTS];
	int			dnext;
	bool		started;
	bool		param_scan;		/* conditions read parameters an enclosing query level sets (custom_exprs): a rescan starts the scan over (agg_rescan) */
	bool		rescannable;	/* the parent may scan the node again (EXEC_FLAG_REWIND, BACKWARD or MARK): a spilled scan keeps the groups it returned */
	int			widen;			/* TOPN_TIES: the scan this engine runs is the widen-th start again (topn_widen) */
	List	   *retries;		/* ... the engines of the earlier starts' successors, closed with the node */
	/* groups */
	bool		hash;
	uint32		direct_max;		/* groups above which the table is a hash table */
	uint32		ngroups;		/* hash mode: groups made */
	uint32		gcap;			/* allocated groups */
	int64	   *seen;
	AggAcc	   *acc;
	uint32	   *hkey;			/* hash mode: nkeys ids per group */
	uint32		(*hashg) (struct AggExec *, const uint32 *);	/* hash mode: the probe for nkeys ids */
	uint32		(*hashgh) (struct AggExec *, const uint32 *, uint32);	/* ... for ids whose hash (ids_hash32) the caller has */
	uint32		hkcap;
	uint32	   *hslot;
	uint32		nhslot;
	DSet	   *dset;			/* aggregates over distinct values (SAA_M_*, sum(DISTINCT numeric)): per aggregate, the (group, value) pairs seen */
	Dist	  **dist;			/* count(DISTINCT x): per aggregate, the values seen of each group */
	OutTable   *aot;			/* count distinct of a dictionary leaf: the distinct values over all dictionaries */
	double	   *amax;			/* avg(float8): the largest absolute input */
	IdMap	   *amap;			/* count(DISTINCT numeric): the ids of the values */
	Osa		  **osa;			/* ordered-set aggregates: the values of the aggregate (shared by those on one argument) */
	OsaVal	  **oval;			/* ... and their results by sorted-values id */
	int			n_osa_spilled;
	uint32		n_gpruned;		/* groups dropped by the top-N of the aggregate (agg/gtopn.c) */
	struct GenSt **gst;			/* generic aggregates (SAA_GEN), per aggregate */
	uint8	   *fwin;			/* DISTINCT ON: per tuple of the chunk, its row became the best of its group (decided by the first of the aggregates, which the others follow) */
	AggState   *gas;			/* the AggState their functions are called with */
	MemoryContext gres;			/* their results */
	int			n_dist_spilled;	/* count(DISTINCT) sets that outgrew hash_mem and went through a sort */
	uint64		dist_hash;		/* the aggregates (below 64) whose sets had left their bitmaps when the table spilled: the next table starts them as hash sets */
	struct AggSpill *spill;	/* the group table goes to partitions on disk past hash memory (agg/spill.c) */
	int			n_spills;		/* ... and the times it did */
	int64		n_dist_chunks;	/* count(DISTINCT) bitmap chunks of the workers that the leader ORed in */
	/* output */
	uint32	   *glist;
	uint32		nglist;
	uint32		gpos;
	Datum	   *total_val;		/* a top-N with totals (TOPN_TOTALS): the aggregates of the one group, once the scan is done */
	bool	   *total_null;
	/* items of work, lanes and workers (run_scan) */
	struct ScanShared *shared;
	bool		lanes;			/* a process of a parallel scan with float sums: each lane keeps its own sums (AggAcc.lv) */
	bool		lane_serial;	/* the lanes are scanned in turn into this table: the float sums of a lane are added to dt when it is done */
	struct AggPar *par;			/* workers launched by the leader */
	uint32	   *vorder;			/* a parallel top-N: the superblocks in the order of the walk (nvorder of them), NULL when they are visited in windows */
	uint32		nvorder;
	uint32	   *dsorder;		/* a streaming count(DISTINCT) (dist_stream_begin): the superblocks in ascending order of the zone minimum of its column (ndsorder of them) */
	int64	   *dsmin;			/* ... and that minimum, PG_INT64_MIN where a superblock gives no bound */
	uint32		ndsorder;
	int			dsagg;			/* ... of the aggregate dsagg */
	bool		dsabort;		/* ... and the sweep was given up: the table of the groups would have spilled, so the scan started over in plain order */
	SiftSrcPrune *prune;		/* the zone tests of the conjuncts: the integer leaves they compare (nprune), then the ORs of such tests (up to nzone) */
	int			nprune;
	struct SkipMap *skip;		/* the superblocks the zones keep, made at the first unit range of the scan (par.c range_skipped) */
	int			nzone;
	List	   *jprune;			/* ... and of the join key filters too sparse for a test of the rows: SiftSrcPrune */
	bool	   *pushed;			/* per spec->pred: the source applies it (filter_init) */
	SiftSrcFilter *pflt;		/* ... and the filter it applies */
	int			nfilter;		/* how many */
	int			ntype;			/* the SAP_TYPE conjuncts: the source keeps the rows of their types (type_init) */
	bool		recheck;		/* the source reads again the rows that hold a value a leaf cannot store (spec->recheck, recheck_init) */
	bool		recheck_late;	/* ... and a row that holds one raises where it reaches the aggregate, after the lookups (SRK_CAST), not after its own conjuncts */
	bool		recheck_after_late;	/* ... a row whose list SQL cannot read (SRK_RECORDSET) raises after the conjuncts that may raise, which drop or fail it first (check_overflow_after_late) */
	int			nwide;			/* the SAP_WIDE conjuncts: the source keeps the rows that pass (wide_init) */
	bool		sb_heap;		/* the unit numbers are heap superblocks (sift_colsrc_sb_heap): a range of them is a range of the heap, for a TID range and for a match set */
	const SiftFilter *search_all;	/* the match set of every unit (not sb_heap), made on first use */
	int			tid_pred;		/* the first SAP_TID conjunct, which tests the range of all of them (tid_init); -1: none */
	uint64		tid_lo;			/* the TID keys [tid_lo, tid_hi) of the range (sift_aggspec_tid_range) */
	uint64		tid_hi;
	BlockNumber tid_blo;		/* the first and the last block of the range */
	BlockNumber tid_bhi;
	uint32		tid_sb_lo;		/* the superblocks [tid_sb_lo, tid_sb_hi) the scan reads */
	uint32		tid_sb_hi;
	uint32	   *tid_rows;		/* the rows of a chunk and their TIDs */
	ItemPointerData *tid_tids;
	SiftVis    *tid_vis;		/* a heap: the visible versions of its edge blocks' rows (a HOT chain's member, not the root the index keeps) */
	TupleTableSlot *tid_slot;
	int			njfilter;		/* the join key conjuncts join_filters added to the spec */
	int			njpass;			/* the joins that pass the tuples the key filter let through (semi_pass) */
	int			nshared;		/* the lookup tables another node of the statement had loaded (map_load_shared) */
	int			ncached;		/* the lookup tables an earlier query had loaded (lookcache_find) */
	int			nfirst;			/* the correlated children read as lookup tables (join_first_rows) */
	struct SiftAggJoin **fjoin;	/* per join: the lookup table a hybrid first-row lookup loads (first_table_join), NULL for the others */
	char	  **pfmaps;			/* worker: the leader's table of each hybrid first-row lookup, by combo */
	bool		first_peek;		/* map_first_peek: a table is asked for only if a cache holds it */
	uint64		first_hit;		/* the rows hybrid first-row lookups gave, and the inputs whose key has rows that differ (the child ran) */
	uint64		first_differ;
	int			nsemi;			/* the semi and anti joins answered from a byte table (semi_lut_build) */
	Size		semi_bytes;		/* and their size */
	uint64		lookup_read;	/* rows of the joined tables the scan read (map_load), those that went into a table, the loads through an index, the time */
	uint64		lookup_loaded;
	int			lookup_index;
	int			lookup_cols;	/* the loads from the leaves of an index of the table (map_load_cols) */
	uint64		lookup_us;
	uint64		lookup_shared;	/* the rows of the tables the leader put in the shared memory of its workers (par_launch) */
	const char **pmaps;			/* in a worker: per combo, the table of its join in the shared memory, NULL when it has none (map_load) */
	const uint64 *rsel;			/* the rows of the unit that still count, when the leaves are read for them only */
	List	   *pack;			/* the plan's spec, for the workers */
	ParamListInfo params;
	Snapshot	snap;
	struct shm_mq_handle *mqh;	/* in a worker: the queue to the leader */
	/* the unit */
	SiftSrcUnit unit;
	SiftSrcVec *uvec;
	bool	   *uvec_ok;
	SiftSrcVec *nvec;			/* LEFT lateral: per element leaf, the NULL column of the rows with no element (nullpass) */
	bool	   *nvec_ok;
	uint64	   *nullrows;		/* the rows of the unit that get a NULL tuple (left_lateral), NULL: none */
	bool		nullpass;		/* the tuples are rows (qlevel 0) that have no element: the element leaves read NULL */
	uint32	  **gofs;			/* per array leaf: elem_offs of the unit, NULL when not built yet */
	uint32	  **gpar;			/* per array leaf: the row that owns each of its elements in the unit (array_parent), NULL when not built yet */
	int			qlevel;			/* 1: element queries */
	uint32	   *parent;
	uint32	   *offs;
	int			nelem;
	uint64		row_base;		/* unique number of the unit's first row */
	bool		dense;			/* the aggregates and keys of the spec are the ones dense_chunk runs */
	uint32	   *zgid;			/* dense chunks without a key: group 0 for every row */
	int64	   *histbuf;		/* dense chunks, few groups: two histograms of histcap groups */
	uint32		histcap;
	int64	   *hist;			/* the rows of the chunk per group; NULL when this chunk has none */
	int64	   *hist2;			/* the scratch of a second one */
	/* the tuples being aggregated (the arrays of cur) */
	Tup			t0;				/* the chunk's selection */
	Tup		   *stage;			/* the output of each join stage that repeats or drops tuples */
	Tup		   *cur;
	int			cstart;			/* the chunk: [cstart, cstart + cn) of the unit's rows / elements */
	int			cn;
	uint32	   *sel;
	uint32	   *tidx;
	uint32	   *ridx;
	uint32	 ***alcout;			/* per stage: the outputs of the tuples it passes on as they are, plus its own */
	uint32	  **seid;			/* the entries of the tuples a stage that repeats them is working on */
	uint32	  **xcout;			/* per combo a scoped combo reads the output of: that row, repeated for the elements it resolves (elem_exists) */
	/* scratch of combos */
	uint64	   *cvid[SIFT_AGG_MAX_CLEAVES];
	uint32	  **cfree;			/* output row per tuple of a combo without a join */
	bool	   *cfree_ok;
	uint32	   *peid;
	uint32	   *pti;
	uint32	   *pri;
	uint32	   *gid;
	uint32	   *kids;			/* hash mode: ids per key, spec->nkeys * VEC */
	uint64	   *knul;			/* per key (VECW + PADW words each): the tuples of the chunk whose value is NULL, set by key_prepare */
	uint32	   *cu32;
	uint64	   *c64;
	uint8	   *okb;
	int64	  **ncnt;			/* the counters that take every tuple of the chunk (seen, the n of the aggregates that leave none out): counted in one pass */
	int			nncnt;
	uint8	   *nb;
	double	   *vstk;			/* float8 aggregate arguments: SIFT_AGG_MAX_VX vectors */
	sift_i128  *nstk;			/* numeric programs: SIFT_AGG_MAX_VX vectors */
	uint8	   *nflag;			/* ... and the NaN flag of each of their values */
	struct VnCache *vn;			/* the leaves of the chunk as int64 vectors for the numeric sums (agg/vnum.c) */
	uint32	   *pix;			/* the data index of the rows a predicate evaluates: start + i */
	uint64	   *okm;
	uint64	   *tmpm;
	uint64	   *tmpm2;
	uint32	   *tmpu32;
	uint32	   *setkid;			/* grouping sets in hash mode: the key ids of the chunk's tuples, shared by the sets that read them (compute_set_gids) */
	/* pre-aggregates (preagg_init) */
	bool		pa_on;			/* a unit that is exact may be answered from what the index stored with it */
	int			pa_dead_miss;	/* its dirty units that read their dead rows and were scanned all the same */
	bool		pa_dead;		/* ... and one with dead rows from what it stored minus theirs (sift.agg_dead_delta): the aggregates are counts and integer sums */
	SiftPaWhy	pa_why;			/* why not, when pa_on is false */
	int			pa_xleaf;		/* the conjunct IS NULL on this leaf is answered as the unit net of its exceptions (-1: none; hist_net) */
	int64		n_exc_units;	/* units answered net of their exceptions, and the units with some that were scanned (a missing dim, a float sum that cancels) */
	int64		n_exc_scan;
	int64		n_dead_units;	/* units answered net of their dead rows, and the units with dead rows that were scanned instead (col/delta.c) */
	int64		n_dead_scan;
	bool		pa_miss;		/* the last preagg_rollup failed for lack of a rollup, a dim or a measure (not for the values in the unit) */
	SiftSrcRollupNeed pa_need;	/* the columns of a unit's rollup the query reads (pa.list >= 0) */
	SiftAggPaShape pa;			/* pa.dim: the leaf of the histograms that answer it, -1: the sums of the leaves; pa.list >= 0: the rollup of that list */
	/* counters */
	int64		n_units;
	struct TupleTableSlot *done_slot;	/* its reads: a slot of minimal tuples */
	struct Tuplestorestate *done;	/* part_flush_on: the finished groups of the partitions scanned, as the tuples of the result */
	int64		n_flushed;
	const uint32 *partoff;		/* a parallel worker of a partitioned table: the leader's first superblock of each partition (sift_aggpart_set_offsets) */
	int			parts_pruned;	/* the partitions of a partitioned table that its conditions refuted when the scan started (part_prune_init) */
	int64		n_pre_units;	/* of them, answered from pre-aggregates */
	int64		n_super;		/* of them, in groups answered from the summary of the group (superunits) */
	int			n_split;		/* superunits split back into units while none answered: a query a summary cannot answer stops asking for them */
	bool		pcount;			/* the source may hand over a segment whose dictionary counts the rows of the one text test (count_range) */
	bool		super_on;		/* the source may hand over superunits: a row level scan that the pre-aggregates can answer, with no match set */
	bool		whole_on;		/* the source takes a segment whole for the key table of the leaf of the comparisons, and an item of the scan has to span one */
	int64		n_rollup_miss;	/* exact units scanned for lack of their rollup, and of them those a refresh was queued for */
	int64		n_rollup_queued;
	int64		n_rollup_cost;	/* exact units with a rollup scanned because their tuples cost more than their elements */
	double		pa_elem_cost;	/* sift_aggspec_unit_costs of the spec: an element scanned, a tuple of the rollup */
	double		pa_tuple_cost;
	int			pa_scan_run;	/* the units in a row whose rollup cost clearly more than their elements: a long run skips reading the rollups of the next ones */
	int64		n_rows;			/* of the others */
	int64		n_tuples;
	int64		n_dense;		/* chunks aggregated from their bitmap (dense_chunk) */
	int64		n_combo_trim;	/* times the combo caches started again at the entry limit (combo_trim) */
	int64		n_groups;
	const SiftAggBackend *be;	/* the backend that answers units of the scan besides the kernels below, and its state (agg/back.h) */
	void	   *be_state;
	/* arms that share one pass (fuse_start) */
	struct FuseSt *fuse;
	int			fuse_set;		/* this node's set in the fused spec */
	Tup		   *set_tup;		/* the tuples of a set of a fused pass that has conjuncts of its own over the joined rows */
} AggExec;

/* the parent of the expressions: a parallel worker has no plan state */
#define EXPR_PARENT(ex)	((ex)->css.ss.ps.state ? &(ex)->css.ss.ps : NULL)

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

extern bool		sift_agg_dense;
extern bool		sift_agg_semi_lut;
extern int		sift_agg_semi_lut_size;
extern bool		sift_agg_lookup_pushdown;
extern bool		sift_agg_lookup_columns;
extern bool		sift_agg_share_lookups;
extern bool		sift_agg_fuse;
extern int		sift_agg_lookup_cache_kb;
extern int		sift_agg_lookup_shared_kb;
extern int		sift_verdict_cache_kb;
extern bool		sift_agg_verdict_cache;
#ifdef SIFT_TESTING
extern bool		sift_test_verdict_fail;
extern bool		sift_test_verdict_gate;
#endif
extern int		sift_agg_combo_cache;

/* agg.c */
void run_scan(AggExec *ex);
bool part_flush_on(const AggExec *ex);
void groups_topn(AggExec *ex);
void part_flush(AggExec *ex);
void groups_order(AggExec *ex);
void list_groups(AggExec *ex);
Datum key_datum(AggExec *ex, int j, uint32 g, bool *isnull);
Datum key_id_datum(AggExec *ex, int j, uint32 id, bool *isnull);
Datum agg_datum(AggExec *ex, int ai, uint32 g, bool *isnull);
void engine_contexts(AggExec *ex, MemoryContext parent);
void engine_rels(AggExec *ex);
void engine_open(AggExec *ex, Snapshot snapshot, ParamListInfo params);
void engine_close(AggExec *ex);
void topn_items(AggExec *ex);

#pragma GCC visibility pop

static inline double
float_from_key(int64 bits)
{
	double		d;

	memcpy(&d, &bits, sizeof(d));
	return d;
}

/* the last set bit in [lo, hi) of a bitmap, -1 if none: the usual row owns a few elements, which lie in one word */
static inline int64
last_bit(const uint64 *b, uint32 lo, uint32 hi)
{
	uint32		w0 = lo >> 6,
				w = (hi - 1) >> 6;
	uint64		x;

	if (lo == hi)
		return -1;
	x = b[w] & ((UINT64CONST(2) << ((hi - 1) & 63)) - 1);
	for (;;)
	{
		if (w == w0)
			x &= ~UINT64CONST(0) << (lo & 63);
		if (x || w == w0)
			break;
		x = b[--w];
	}
	return x ? (int64) w * 64 + 63 - __builtin_clzll(x) : -1;
}

static inline int
leaf_lvl(AggExec *ex, int leaf)
{
	return ex->lay->leaf[leaf].parent >= 0;
}

static inline void
fill_ones(uint64 *m, int n)
{
	int			nw = (n + 63) / 64;

	for (int i = 0; i < nw; i++)
		m[i] = ~UINT64CONST(0);
	if (n & 63)
		m[nw - 1] = (UINT64CONST(1) << (n & 63)) - 1;
}

static inline void
clear_words(uint64 *m, int n)
{
	memset(m, 0, ((n + 63) / 64) * sizeof(uint64));
}

static inline void
bind_param(AggExec *ex, int i, Datum d, bool isnull)
{
	ex->lutparams[i].value = d;
	ex->lutparams[i].isnull = isnull;
}

static inline bool
leaf_wide(const SiftSrcLeaf *lf)
{
	return lf->vtype == SCV_I64 || lf->vtype == SCV_F64;
}

/*
 * The sum of a numeric aggregate (SAA_SUM_NUM, SAA_AVG_NUM) of a group is acc->w + acc->i * 2^128: w wraps, acc->i counts the wraps, so a sum of values of up to 38 digits
 * is exact whatever the number of rows (agg_sum_datum builds the numeric when a group has wrapped).
 */
static inline void
agg_sum_add(AggAcc *acc, uint32 g, sift_i128 v)
{
	if (__builtin_add_overflow(acc->w[g], v, &acc->w[g]))
		acc->i[g] += v > 0 ? 1 : -1;
}

/*
 * The product of two 38-digit integers (a sum of a decimal product of up to 76 digits) added to the sum of group g: the 256-bit product is hi * 2^128 + lo,
 * lo taken as a signed 128-bit value (so that agg_sum_add counts its wraps) and the high word, corrected by the sign of lo, added to the carries.
 */
static inline void
agg_sum_add_product(AggAcc *acc, uint32 g, sift_i128 a, sift_i128 b)
{
	typedef unsigned __int128 u128;
	u128		ua = a < 0 ? -(u128) a : (u128) a,
				ub = b < 0 ? -(u128) b : (u128) b;
	uint64		a0 = (uint64) ua,
				a1 = (uint64) (ua >> 64),
				b0 = (uint64) ub,
				b1 = (uint64) (ub >> 64);
	u128		p01 = (u128) a0 * b1,
				p10 = (u128) a1 * b0,
				p00 = (u128) a0 * b0,
				mid = (p00 >> 64) + (uint64) p01 + (uint64) p10,
				lo = (p00 & UINT64_MAX) | (mid << 64),
				hi = (u128) a1 * b1 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);

	if ((a < 0) != (b < 0))
	{
		lo = ~lo + 1;
		hi = ~hi + (lo == 0);
	}
	agg_sum_add(acc, g, (sift_i128) lo);
	if (unlikely(__builtin_add_overflow(acc->i[g], (int64) (sift_i128) hi + (int64) (lo >> 127), &acc->i[g])))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value overflows numeric format")));
}

/* the sum (w, carry) of a partial state added to group g */
static inline void
agg_sum_merge(AggAcc *acc, uint32 g, sift_i128 w, int64 carry)
{
	acc->i[g] += carry;
	agg_sum_add(acc, g, w);
}

extern Datum agg_sum_datum(const AggAcc *acc, uint32 g, int scale);

static inline const sift_i128 *
pow10_tab(void)
{
	static sift_i128 tab[39];

	if (tab[1] == 0)
	{
		tab[0] = 1;
		for (int i = 1; i < 39; i++)
			tab[i] = tab[i - 1] * 10;
	}
	return tab;
}

static inline uint32
combo_val(AggExec *ex, int ci, uint32 row, int sl)
{
	uint32		r = ex->cs[ci].val[(size_t) row * ex->spec->combo[ci].nslots + sl];

	return r != COMBO_UNEVAL ? r : combo_eval_slot(ex, ci, row, sl);
}

/* the group of an id tuple in hash mode */
static inline uint32
hash_group(AggExec *ex, const uint32 *ids)
{
	return ex->hashg(ex, ids);
}

/* the id of key j in group g (0: NULL) */
static inline uint32
key_id(AggExec *ex, int j, uint32 g)
{
	return ex->hash ? ex->hkey[(Size) g * ex->spec->nkeys + j] : (g / ex->ks[j].mult) % ex->ks[j].cap;
}

static inline void
gather_idx(AggExec *ex, int leaf, int nsel, const uint32 **idx_out)
{
	*idx_out = (leaf_lvl(ex, leaf) == ex->qlevel) ? ex->tidx : ex->ridx;
}

/* the leaf a key reads; NULL for the keys of a combo, a numeric expression and the grouping set number */
static inline const SiftSrcLeaf *
key_lf(AggExec *ex, int j)
{
	const SiftAggKey *k = &ex->spec->key[j];

	return k->kind == SAK_COMBO || k->kind == SAK_SET || k->kind == SAK_NUM || k->kind == SAK_SPINE ? NULL : &ex->lay->leaf[k->leaf];
}

#endif							/* SIFT_AGG_AGG_INT_H */
