/*
 * agg/agg_int.h - the state of the executor of agg/agg.c and what its parts share
 */
#ifndef SIFT_AGG_AGG_INT_H
#define SIFT_AGG_AGG_INT_H

#include "executor/nodeAgg.h"
#include "agg/back.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/spec.h"
#include "agg/topn.h"
#include "core/visibility.h"

#define VEC			SIFT_AGG_VEC
#define VECW		SIFT_AGG_VECW
#define PADW		8
#define MAX_RANGE	(1 << 22)		/* values a range table covers */
#define DIRECT_MAX	(1 << 20)		/* groups of the direct arrays */
#define PRESIZE_MIN		4096		/* groups the planner expects above which a single key starts with room for them */
#define LANE_DIRECT_MAX	(1 << 15)	/* a lane is built and folded or dumped sixteen times: sparse group spaces go to the hash table */
#define NDICT_SLOTS	8

typedef struct OutTable
{
	Oid			collid;
	bool		image;			/* values are the same when their bytes are (generic DISTINCT), not when the type's equality says so */
	bool		byval;
	int16		len;
	FmgrInfo	hashfn;
	FmgrInfo	eqfn;
	uint32		n;				/* ids 1..n */
	uint32		cap;
	Datum	   *val;
	uint32	   *hv;
	uint32		nslot;
	uint32	   *slot;			/* id or 0 */
	uint64		bytes;			/* the by-reference values held */
} OutTable;

typedef struct RangeTab
{
	int32		lo;
	int32		hi;				/* covered values lo..hi; lo > hi: none */
	void	   *tab;			/* esize bytes per value, the entry of lo first */
	int			esize;
	char	   *mem;			/* the allocation: it holds the values plo..phi, so that growing a range is not a copy */
	int32		plo;
	int32		phi;
} RangeTab;

/* fill(arg, v, entry) is called for each value newly covered, ascending */
typedef void (*FillFn) (void *arg, int32 v, void *entry);

typedef struct DictSlot
{
	const void *id;
	uint64	  **lut;			/* per predicate id: bit set of n + 1 entries (bit n: the NULL input) */
	uint32	  **kmap;			/* per key: n + 1 key ids */
} DictSlot;

typedef struct PredState
{
	bool		ready;
	bool		cnull;
	int64		ci;
	double		cd;
	bool		in_hasnull;
	bool		in_dense;		/* the elements are every value of [in_lo, in_lo + in_nlut): the interval of the zone holds nothing else */
	bool		in_empty;		/* the array has no element: x NOT IN () holds for every row, a NULL x too */
	int64		in_lo;
	uint32		in_nlut;
	uint8	   *in_lut;			/* a table over [in_lo, in_lo + in_nlut) ... */
	int32	   *in_sorted;		/* ... or, when the values are too far apart for one, the sorted values (an integer leaf) ... */
	uint32		in_nsorted;
	int64	   *in_htab;		/* ... or a hash table of the values (a 64-bit leaf), in_hmask + 1 slots, in_hused marks the full ones */
	uint8	   *in_hused;
	uint64		in_hmask;
	ExprState  *expr;			/* SAP_LUT */
	Param	   *param;
	bool		null_result;	/* SAP_LUT: the expression for a NULL input */
	SiftSrcStrTest *str;		/* a row level text test of the query, which the source runs on the encoded form (str_init) */
	RangeTab	rt;				/* SAP_LUT on an integer leaf: result bytes */
	uint64	   *rowmask;		/* element queries, a row predicate: the unit's rows */
	bool		rowmask_ok;
	int			nop;			/* numeric operand: the comparison of the scaled integers equivalent to the query's (SiftCmpOp) ... */
	sift_i128	cn;				/* ... with this operand, or ... */
	int			konst;			/* ... -1, or 1 / 0: it holds for every / no value */
	bool		nan_res;		/* numeric operand: the comparison holds for a NaN (numeric orders it above every number and equal to itself) */
} PredState;

/* distinct numbers to ids 1.. in order of appearance (open addressing) */
typedef struct IdMap
{
	MemoryContext cxt;
	uint32		n;
	uint32		cap;
	sift_i128  *val;
	uint32		nslot;
	uint32	   *slot;			/* id or 0 */
} IdMap;

typedef struct KeyState
{
	OutTable	ot;				/* distinct output values (ids 1..) */
	ExprState  *expr;
	Param	   *param;
	RangeTab	rt;				/* integer leaves: value -> id */
	bool		ident;			/* integer or float leaf as the key (and the grouping set number): ids are given to the values in order of appearance */
	int64	   *idval;			/* ident: the value of id i + 1 (a float: float_key) */
	uint32		negzero;		/* a float: the id of the zero group when the first zero seen was -0 (the one PG shows), else 0 */
	uint32		nident;
	uint32		capident;
	uint32		wcap;			/* ident, values too far apart for a table: open addressing value -> id (0 = free), power of 2 */
	int64	   *wval;
	uint32	   *wid;
	uint32		nwide;			/* the entries of the open addressing table */
	int64		bucket;			/* LUT keys on a timestamp leaf: the microseconds one evaluation covers (the table maps bucket numbers to ids); 0: not one */
	uint32		nullid;			/* LUT keys on integer leaves: the id of expr(NULL) */
	uint32		cap;			/* direct mode: ids 0..cap-1 */
	uint32		mult;
	bool		numk;			/* SAK_NUM: ids of the program's values */
	IdMap		im;
} KeyState;

/* open addressing set of (group, value id) pairs; a slot holds the pair with bit 63 set, 0 is empty */
typedef struct Dist Dist;

typedef struct DSet
{
	uint64	   *slot;
	int64	   *cnt;			/* counted sets: the times each pair was added, per slot */
	uint64		n;
	uint64		cap;
	bool		counted;
} DSet;

/*
 * The values of an ordered-set aggregate, kept exactly (the multiset PostgreSQL sorts): a histogram of the distinct (group,
 * value) pairs while it fits work_mem, and every value in a tuplesort (work_mem, spilling to disk as the normal plan's sort)
 * once it does not. A value is its order key (float8 in the order of float8_cmp: -0 = 0, NaN last), a group the id of its
 * sorted values (in order of appearance; acc->i of the aggregate that owns the values, which the group renumbering carries).
 */
#define OSA_BLOCK	64

typedef struct OsaSlot
{
	uint64		key;
	uint32		sid1;			/* sorted-values id + 1, 0: free */
	uint32		cnt;
} OsaSlot;

typedef struct Osa
{
	OsaSlot    *slot;			/* histogram; NULL once spilled */
	uint64		cap;
	uint64		n;
	uint64		maxn;
	uint32		nsid;
	uint64		next;			/* read position in the compacted histogram */
	Tuplesortstate *sort;
	TupleTableSlot *put;
	TupleTableSlot *get;
} Osa;

/* PostgreSQL's own aggregate of a joined column (SAA_M_PG): its functions, and the state the transition function starts from */
typedef struct PgAgg
{
	FmgrInfo	trans;
	FmgrInfo	final;
	bool		strict;			/* the transition function is strict: the first value is the state when there is no initial one */
	bool		has_final;
	bool		final_extra;
	bool		init_null;
	Datum		init;
	int16		tlen;			/* the state's type */
	bool		tbyval;
	int16		rlen;			/* the result's type */
	bool		rbyval;
} PgAgg;

typedef struct AggAcc
{
	int64	   *n;				/* tuples / non-null values */
	int64	   *i;
	sift_i128  *w;				/* numeric sums: the exact sum of the scaled values; numeric min and max: the value */
	double	   *d;
	uint64	   *last;			/* count distinct of the rows: the last row counted */
	uint8	   *nf;				/* float sums: a non-finite input was added */
	Datum	   *v;				/* aggregates over distinct values (SAA_M_*): the result, made by mapped_finish; SAA_GEN: the state */
	Datum	   *r;				/* SAA_GEN: the result */
	double	   *dt;				/* float sums of a scan that goes through the lanes in one table: the sum of the lanes done */
} AggAcc;

/* the distinct values of a dictionary leaf over all the dictionaries of the scan: ids 1.. */
typedef struct Intern
{
	uint32		n;
	uint32		cap;
	uint32		nslot;
	const char **ptr;
	uint32	   *len;
	uint32	   *hv;
	uint32	   *slot;
} Intern;

/* the rows of a joined table, hashed by their key columns; the columns the expressions read are kept as Datums */
typedef struct MapTab
{
	int			nkey;
	int			ncol;
	bool		ktext[SIFT_AGG_MAX_JKEYS];
	uint32		nrow;
	uint32		cap;
	const char **kptr;			/* [row * nkey + k] text keys */
	uint32	   *klen;
	int64	   *kint;			/* integer keys */
	Datum	   *col;			/* [row * ncol + c] */
	bool	   *cnull;
	uint32	   *hv;
	uint32	   *next;			/* row + 1 of the next row of the bucket */
	uint32		nslot;
	uint32	   *head;
	MemoryContext cxt;			/* where the rows live: the scan's, or the statement's for a table its arms share (map_load_shared) */
	bool		filtered;		/* only rows that hold the ON clause's own conditions were loaded (map_load) */
	bool		keep;			/* the side is preserved: the rows no tuple matched are output rows of NULL leaves */
	bool	   *nokey;			/* keep: the rows with a NULL key, which no tuple matches and are not in the chains */
	bool	   *hit;			/* keep: the rows some tuple matched */
} MapTab;

typedef struct ComboEnt
{
	uint32		nout;			/* output rows of the tuple: its matches, one row of NULLs (LEFT JOIN) or none (INNER) */
	uint32		base;			/* first output row */
} ComboEnt;

/* the ids (1..) of the distinct values of a 64-bit leaf of a combo: one table of (value, id), so a probe is one cache line */
typedef struct Id64Ent
{
	int64		key;
	uint32		id;				/* 0: free */
} Id64Ent;

typedef struct Id64
{
	MemoryContext cxt;
	Id64Ent    *tab;
	uint32		nslot;
	uint32		n;
	uint32		cap;
	int64	   *val;			/* the value of an id */
	uint32		nan_id;			/* the id of the NaN of a numeric leaf (not in tab), 0 until one is seen */
} Id64;

/* a numeric value as an integer times 10^-scale (ok: it is a finite number of at most 38 digits) */
typedef struct DNum
{
	sift_i128	m;
	int16		scale;
	bool		ok;
} DNum;

typedef struct ComboSt
{
	Intern		in[SIFT_AGG_MAX_CLEAVES];
	Id64		id64[SIFT_AGG_MAX_CLEAVES];	/* 64-bit leaves (numeric, int8, timestamps, floats): the ids of their distinct values */
	Datum		unit[SIFT_AGG_MAX_CLEAVES];	/* numeric leaves: 10^-scale with that many digits, a stored value is the integer times it */
	MemoryContext cxt;			/* the cache: entries, outputs, values and ids of the leaves; reset when it passes sift.agg_combo_cache */
	bool		resettable;		/* no join, no dictionary leaf: nothing outside the cache holds its ids */
	bool		by_id;			/* one 64-bit leaf, no join: the entry of a tuple is the id of its value, there is no table of entries */
	MapTab	   *map;
	uint8	   *slut;			/* a semi or anti join that is a plain filter on one integer leaf: a table over [slo, slo + snlut) of its keys */
	int32		slo;
	uint32		snlut;
	ExprState **slot;
	List       *args;			/* correlated child parameter expressions */
	ExprState  *kx[SIFT_AGG_MAX_JKEYS];	/* the join keys that are expressions */
	int16	   *tlen;			/* per slot: the length and pass-by-value of its result, for the direct slots */
	bool	   *tbyval;
	Datum	   *dat;			/* the values of the direct slots, a slot's output is the number + 1 of one */
	DNum	   *dnum;			/* per value of dat, for the slots of numeric sums: the exact integer and scale it stands for */
	uint32		ndat;
	uint32		datcap;
	char	   *arena;			/* the by-reference values of dat are copied here, in blocks of the cache */
	Size		arena_left;
	bool	   *decode;			/* per slot: the argument of a numeric sum or average, whose values dnum holds */
	uint32		nevals;
	uint32		nent;
	uint32		entcap;
	int			ninputs;		/* stored cache tuple width, at least one */
	uint64	   *vid;			/* [ent * ninputs]: 0 NULL, else the value's id in `in` / the integer + 1 */
	bool	   *null_input;		/* the nullable input was absent for this entry */
	ComboEnt   *ent;
	uint32		nht;
	uint32	   *ht;				/* ent + 1 */
	uint32		dircap;			/* one input: entry + 1 of the id (below COMBO_DIR_MAX) by index, 0 not seen (combo_ent) */
	uint32	   *dir;
	uint32		nout;
	uint32		outcap;
	uint32	   *val;			/* [out * nslots + slot]: key id (0 NULL) or boolean */
	uint32	   *maprow;		/* output row -> lookup row, UINT32_MAX for a NULL extension */
} ComboSt;

/* the tuples of a chunk that reach the aggregates: positions in the chunk, element and row ordinals, and the output row of every join combo */
typedef struct Tup
{
	int			n;
	uint32	   *sel;
	uint32	   *tidx;
	uint32	   *ridx;
	uint32	   *cout[SIFT_AGG_MAX_COMBOS];
} Tup;

/* the state of one arm of a two level query (SiftAggArm): per tuple of the chunk, and per outer group the inner aggregates of the current row */
typedef struct ArmSt
{
	uint32	   *gid;
	uint8	   *ok;				/* the arm's filters hold */
	ExprState  *hstate;			/* the rest of the arm's HAVING */
	uint8	  **iok;			/* per inner aggregate: it takes the tuple */
	double	  **dx;				/* its float8 value */
	int64	  **ix;				/* its integer value */
	uint64	   *ord;			/* per outer group: the row its cell holds, UINT64_MAX none */
	int64	  **n;				/* per inner aggregate and group */
	int64	  **i;
	double	  **d;
	uint8	  **nf;
	double	   *amax;			/* per inner aggregate: the largest absolute float input */
} ArmSt;

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
	int			clbase[SIFT_AGG_MAX_COMBOS];	/* per combo: its first dictionary map slot (after the keys and aggregates) */
	int			ncl;
	int			nj;				/* join combos, in the order the stages apply them */
	int			jcombo[SIFT_AGG_MAX_JOINS];
	MemoryContext comcxt;			/* temporaries of one combo entry */
	ArmSt	   *arm;
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
	/* groups */
	bool		hash;
	uint32		direct_max;		/* groups above which the table is a hash table */
	uint32		ngroups;		/* hash mode: groups made */
	uint32		gcap;			/* allocated groups */
	int64	   *seen;
	AggAcc	   *acc;
	uint32	   *hkey;			/* hash mode: nkeys ids per group */
	uint32		(*hashg) (struct AggExec *, const uint32 *);	/* hash mode: the probe for nkeys ids */
	uint32		hkcap;
	uint32	   *hslot;
	uint32		nhslot;
	DSet	   *dset;			/* aggregates over distinct values (SAA_M_*, sum(DISTINCT numeric)): per aggregate, the (group, value) pairs seen */
	Dist	  **dist;			/* count(DISTINCT x): per aggregate, the values seen of each group */
	OutTable   *aot;			/* count distinct of a dictionary leaf: the distinct values over all dictionaries */
	double	   *amax;			/* avg(float8): the largest absolute input */
	IdMap	   *amap;			/* count(DISTINCT numeric): the ids of the values */
	Osa		  **osa;			/* ordered-set aggregates: the values of the aggregate (shared by those on one argument) */
	double	  **oval;			/* ... and their results by sorted-values id */
	int			n_osa_spilled;
	struct GenSt **gst;			/* generic aggregates (SAA_GEN), per aggregate */
	uint8	   *fwin;			/* DISTINCT ON: per tuple of the chunk, its row became the best of its group (decided by the first of the aggregates, which the others follow) */
	AggState   *gas;			/* the AggState their functions are called with */
	MemoryContext gres;			/* their results */
	int			n_dist_spilled;	/* count(DISTINCT) sets that outgrew hash_mem and went through a sort */
	int64		n_dist_chunks;	/* count(DISTINCT) bitmap chunks of the workers that the leader ORed in */
	/* output */
	uint32	   *glist;
	uint32		nglist;
	uint32		gpos;
	/* items of work, lanes and workers (run_scan) */
	struct ScanShared *shared;
	struct AggExec *gtab;		/* the table of the result: this one, or (lanes) its own view */
	bool		lanes;			/* this table is one of the lanes */
	bool		lane_serial;	/* the lanes are scanned in turn into this table: the float sums of a lane are added to dt when it is done */
	char	  **pending;		/* lanes: the partial tables that wait for the lanes before them */
	uint32		next_lane;		/* lanes: the next one to combine */
	struct AggPar *par;			/* workers launched by the leader */
	SiftSrcPrune *prune;		/* the zone tests of the conjuncts */
	int			nprune;
	bool	   *pushed;			/* per spec->pred: the source applies it (filter_init) */
	SiftSrcFilter *pflt;		/* ... and the filter it applies */
	int			nfilter;		/* how many */
	int			ntype;			/* the SAP_TYPE conjuncts: the source keeps the rows of their types (type_init) */
	bool		recheck;		/* the source reads again the rows that hold a value a leaf cannot store (spec->recheck, recheck_init) */
	int			nwide;			/* the SAP_WIDE conjuncts: the source keeps the rows that pass (wide_init) */
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
	int			nshared;		/* the lookup tables another node of the statement had loaded (map_load_shared) */
	int			nsemi;			/* the semi and anti joins answered from a byte table (semi_lut_build) */
	Size		semi_bytes;		/* and their size */
	uint64		lookup_read;	/* rows of the joined tables the scan read (map_load), those that went into a table, the loads through an index, the time */
	uint64		lookup_loaded;
	int			lookup_index;
	uint64		lookup_us;
	const uint64 *rsel;			/* the rows of the unit that still count, when the leaves are read for them only */
	List	   *pack;			/* the plan's spec, for the workers */
	ParamListInfo params;
	Snapshot	snap;
	struct shm_mq_handle *mqh;	/* in a worker: the queue to the leader */
	/* the unit */
	SiftSrcUnit unit;
	SiftSrcVec *uvec;
	bool	   *uvec_ok;
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
	Tup			stage[SIFT_AGG_MAX_JOINS];	/* the output of each join stage that repeats or drops tuples */
	Tup		   *cur;
	int			cstart;			/* the chunk: [cstart, cstart + cn) of the unit's rows / elements */
	int			cn;
	uint32	   *sel;
	uint32	   *tidx;
	uint32	   *ridx;
	uint32	   *seid[SIFT_AGG_MAX_JOINS];	/* the entries of the tuples a stage that repeats them is working on */
	/* scratch of combos */
	uint64	   *cvid[SIFT_AGG_MAX_CLEAVES];
	uint32	   *cfree[SIFT_AGG_MAX_COMBOS];	/* output row per tuple of a combo without a join */
	bool		cfree_ok[SIFT_AGG_MAX_COMBOS];
	uint32	   *peid;
	uint32	   *pti;
	uint32	   *pri;
	uint32	   *gid;
	uint32	   *kids;			/* hash mode: ids per key, spec->nkeys * VEC */
	uint32	   *cu32;
	uint64	   *c64;
	uint8	   *okb;
	uint8	   *nb;
	double	   *vstk;			/* float8 aggregate arguments: SIFT_AGG_MAX_VX vectors */
	sift_i128  *nstk;			/* numeric programs: SIFT_AGG_MAX_VX vectors */
	uint8	   *nflag;			/* ... and the NaN flag of each of their values */
	uint32	   *pix;			/* the data index of the rows a predicate evaluates: start + i */
	uint64	   *okm;
	uint64	   *tmpm;
	uint64	   *tmpm2;
	uint32	   *tmpu32;
	/* pre-aggregates (preagg_init) */
	bool		pa_on;			/* a unit that is exact may be answered from what the index stored with it */
	bool		pa_miss;		/* the last preagg_rollup failed for lack of a rollup, a dim or a measure (not for the values in the unit) */
	SiftAggPaShape pa;			/* pa.dim: the leaf of the histograms that answer it, -1: the sums of the leaves; pa.list >= 0: the rollup of that list */
	/* counters */
	int64		n_units;
	int64		n_pre_units;	/* of them, answered from pre-aggregates */
	int64		n_rollup_miss;	/* exact units scanned for lack of their rollup, and of them those a refresh was queued for */
	int64		n_rollup_queued;
	int64		n_rows;			/* of the others */
	int64		n_tuples;
	int64		n_dense;		/* chunks aggregated from their bitmap (dense_chunk) */
	int64		n_groups;
	const SiftAggBackend *be;	/* the backend that answers units of the scan besides the kernels below, and its state (agg/back.h) */
	void	   *be_state;
	/* arms that share one pass (fuse_start) */
	struct FuseSt *fuse;
	int			fuse_set;		/* this node's set in the fused spec */
	int			fuse_off;		/* the first of its keys */
} AggExec;

/* the parent of the expressions: a parallel worker has no plan state */
#define EXPR_PARENT(ex)	((ex)->css.ss.ps.state ? &(ex)->css.ss.ps : NULL)

#define COMBO_UNEVAL	UINT32_MAX
#define COMBO_NONE		UINT32_MAX

#define COMBO_DIR_MAX	((uint64) 1 << 16)	/* the ids a combo of one input finds its entry by index (256 kB at most) */

/*
 * The distinct values of count(DISTINCT x) per group, x an integer, or the id (dense, from 0) of a dictionary or numeric value. Three
 * structures, each bounded by hash_mem (work_mem times hash_mem_multiplier); the aggregate moves to the next one when its structure
 * would outgrow that, and nothing is lost on the way:
 *
 *   DD_BITS  a bitmap per group, in chunks of 4096 bits allocated when a value falls in them, under a directory of chunk pointers that
 *            covers the range of values seen so far (the same for every group). A value costs one bit when the domain is dense (customer
 *            ids 1 to 2 million by 36 months is 9 MB); a test-and-set tells whether it is new. The range is limited to 2^28 values.
 *   DD_HASH  an open addressing set of (group, value) pairs, sized to hash_mem; values are inserted a batch at a time, the slots
 *            prefetched first.
 *   DD_SORT  every (group, value) pair goes through a tuplesort (work_mem, spilling to disk as the normal plan's sort does), and the
 *            groups' counts are taken from the sorted runs when the scan ends. A group is named by its sort id (the groups are
 *            renumbered while the scan runs), the number of its first value.
 *
 * dist_add() returns true when the pair is new and the count of the group is to be incremented at once; in DD_SORT it cannot know and
 * returns false, and dist_finish() sets the counts of all the groups.
 */
#define DB_SHIFT		12
#define DB_WORDS		(1 << (DB_SHIFT - 6))
#define DB_NCD_MIN		64
#define DB_NCD_MAX		65536
#define DB_DENSE		32				/* bits from which a chunk goes to the leader as it is rather than as pairs */
#define DB_PREFETCH		(2 << 20)		/* bytes of bitmaps from which the cache no longer holds them */
#define DB_BATCH		32				/* values whose bits are prefetched together */
#define DB_ARENA		255				/* chunks per arena block; the first chunk of a block links to the previous block */

typedef enum DistMode
{
	DD_BITS,
	DD_HASH,
	DD_SORT
} DistMode;

typedef uint64 *DistChunk;

struct Dist
{
	DistMode	mode;
	bool		sgn;			/* the values are int32 */
	bool		done;			/* DD_SORT: sorted and counted */
	uint64		budget;			/* bytes the bitmaps or the set may take */
	/* DD_BITS */
	int64		cbase;			/* chunk number of directory entry 0 */
	uint32		ncd;			/* entries of a directory (0: nothing yet) */
	uint32		ngd;			/* groups with room in gd */
	DistChunk **gd;				/* per group: its directory */
	uint64		bytes;
	uint64	   *arena;
	uint32		arena_left;
	/* DD_HASH */
	DSet		ds;
	uint64		maxn;
	/* DD_SORT */
	Tuplesortstate *sort;
	TupleTableSlot *put;
	TupleTableSlot *get;
	uint32		nsid;
	uint32		nsidcap;
	uint32	   *sid;			/* per group: its sort id + 1 */
	uint32		sidgroups;
	uint64	   *cnt;			/* per sort id: distinct values, once sorted */
};

typedef struct KeyFill
{
	AggExec    *ex;
	int			ki;
} KeyFill;

typedef struct OsaRun
{
	uint32		sid;
	uint64		key;
	uint64		cnt;
} OsaRun;

/*
 * The aggregate runs as ExecAgg runs it (advance_transition_function, finalize_aggregate): the state of each group is a Datum made
 * by the aggregate's transition function, called through fmgr with an AggState as its context (AggCheckCallContext, AggGetAggref,
 * AggRegisterCallback), in memory that lives as long as the scan; the final function runs once per group when the scan has ended.
 * DISTINCT: the (group, value) pairs go to a set, and the distinct values of a group go through the transition function in the
 * order of the type's btree, as PostgreSQL's sort gives them. WITHIN GROUP: the values of all groups go through one tuplesort
 * (memory bounded by work_mem) by group; each group's values are then given to the aggregate, which sorts them as it does in
 * ExecAgg, and the state is freed after the group's final function.
 */
#define GEN_INIT	1				/* the group's state is made */
#define GEN_NULL	2				/* transValueIsNull */
#define GEN_NOVAL	4				/* noTransValue: a strict transition function without initial value takes its first input as the state */
#define GEN_DONE	8				/* acc->r is the result */
#define GEN_RNULL	16				/* ... and it is NULL */

typedef struct GenSt
{
	const SiftAggGen *g;
	int			ai;				/* the aggregate's number: its table of joined values is aot[ai] */
	Aggref	   *aggref;
	ExprContext *cx;				/* the states live in its per-tuple memory */
	AggStatePerTransData pertrans;
	AggStatePerAggData peragg;
	FmgrInfo	transfn;
	FmgrInfo	finalfn;
	FmgrInfo   *cmp;				/* DISTINCT: the btree comparison and the equality of the argument's type */
	FmgrInfo   *eq;
	FunctionCallInfo tfc;
	FunctionCallInfo ffc;
	bool		strict;
	bool		has_final;
	int			nfinal;
	int			ndirect;
	Oid			transtype;
	int16		tlen;
	bool		tbyval;
	int16		rlen;
	bool		rbyval;
	Datum		initval;
	bool		initnull;
	ExprState  *argx[SIFT_AGG_MAX_GARGS];
	Datum		argk[SIFT_AGG_MAX_GARGS];		/* constant arguments */
	bool		argknull[SIFT_AGG_MAX_GARGS];
	Datum		dir[FUNC_MAX_ARGS];				/* direct arguments, evaluated once */
	bool		dirnull[FUNC_MAX_ARGS];
	Tuplesortstate *sort;
	TupleTableSlot *put;
	TupleTableSlot *get;
	uint32		nsid;				/* WITHIN GROUP, ORDER BY: groups numbered as their first value arrives (group indexes change as the table grows) */
	int			vcol[SIFT_AGG_MAX_GARGS];		/* ORDER BY: the column of the sort tuple that holds argument k after the group number, -1 for a constant */
	FmgrInfo	combinefn;				/* parallel: the partial states of the workers merge through these */
	FmgrInfo	serialfn;
	FmgrInfo	deserialfn;
	FunctionCallInfo cfc;
	FunctionCallInfo sfc;
	FunctionCallInfo dfc;
	bool		tmode;				/* the arguments go through the tuplesort of vcol tuples: ORDER BY, or DISTINCT once its hash set outgrew work_mem */
	bool		spilled;
	int			ndcol;				/* DISTINCT in the sort: the tuple columns that make two inputs equal, with their equality functions ... */
	int			dcol[SIFT_AGG_MAX_GARGS];
	FmgrInfo	deq[SIFT_AGG_MAX_GARGS];
	int16		dlen[SIFT_AGG_MAX_GARGS];
	bool		dbyval[SIFT_AGG_MAX_GARGS];
	Datum		prev[SIFT_AGG_MAX_GARGS];		/* ... and the previous input's values */
	bool		prevnull[SIFT_AGG_MAX_GARGS];
	MemoryContext pcx;
	SortSupportData *fsup;		/* DISTINCT ON (SiftAggGen.first): the comparison of each sort key of the call ... */
	int			nfk;
	int			fkey[SIFT_AGG_MAX_GARGS];	/* ... the argument it reads */
	bool		follower;		/* ... every DISTINCT ON aggregate of the query has the same sort keys, so the first one decides which rows win and the others take those */
	bool		iskey[SIFT_AGG_MAX_GARGS];
	int16		alen[SIFT_AGG_MAX_GARGS];	/* the arguments' types, to copy the values of the best row */
	bool		abyval[SIFT_AGG_MAX_GARGS];
} GenSt;

/* the best row of a group of a DISTINCT ON aggregate (the group's state until the scan ends): the values of the arguments, then their NULL flags */
typedef Datum GenBest;

#define GEN_BEST_SIZE(n)		((n) * (sizeof(Datum) + sizeof(bool)))
#define GEN_BEST_NULL(b, n, k)	(((bool *) ((b) + (n)))[k])

/* the leaves an argument reads, decoded for the chunk's tuples */
typedef struct GenIn
{
	const SiftSrcVec *vec[SIFT_AGG_MAX_GARGS][SIFT_AGG_MAX_GLEAVES];
	const uint32 *idx[SIFT_AGG_MAX_GARGS][SIFT_AGG_MAX_GLEAVES];
	const uint32 *cout[SIFT_AGG_MAX_GARGS];	/* an argument of a joined table: the chunk's combo rows and the combo's value ids */
	const uint32 *cval[SIFT_AGG_MAX_GARGS];
} GenIn;

/*
 * A unit whose rows are all visible and alive (sift_colsrc_unit_exact) is answered from what the index stored with it, as if its
 * rows had gone through the aggregates: the sums of its leaves (no key), or the groups of the histogram of the key's column. The
 * conjuncts on that column run on the groups, the others are zone comparisons that the unit's zone must lie inside. A unit that
 * cannot answer (a dirty page, the write buffer, no histogram, a measure the histogram lacks) is scanned, into the same table.
 */
typedef struct PaVal
{
	int64		rows;			/* of the group */
	int64		nn;				/* ... whose argument is not NULL */
	sift_i128	sum;			/* the sum of the argument */
	bool		has_mm;
	int64		min;
	int64		max;
	double		sumf;			/* the sum of a float8 argument */
	double		minf;			/* the smallest and the largest float8 argument */
	double		maxf;
} PaVal;

/*
 * A group table leaves a process as a blob: the key values its groups use, then per group the key ids (numbered from 1 in the
 * blob) and the states of the aggregates, in 8-byte aligned sections. blob_merge adds a blob to a table by value: the keys through
 * the table's own ids, the count(DISTINCT) sets by union, the sums in the order the blobs are merged (which the caller fixes).
 */
#define BLOB_MAGIC	0x53414731

typedef struct BlobHdr
{
	uint32		magic;
	int32		lane;
	uint32		ngroups;
	uint32		pad;
} BlobHdr;

typedef struct ScanShared
{
	pg_atomic_uint32 next;		/* the next item to take */
	uint32		nitems;
	uint32		nsb;
	uint32		morsel;			/* superblocks of an item (the scans that keep their state in the leader: all of them) */
	bool		ordered;
	uint32		partoff[FLEXIBLE_ARRAY_MEMBER];	/* a partitioned table: the first superblock of each partition and the total (sift_aggpart_offsets) */
} ScanShared;

typedef struct AggPar
{
	ParallelContext *pcxt;
	int			nlaunched;
	int			ndone;
	shm_mq_handle **mqh;
	bool	   *done;
	SiftSrcCounters cc;			/* of the workers, added */
	int64		matches;		/* BM25 matches the workers found, added */
} AggPar;

typedef struct FuseSt
{
	AggExec    *F;				/* the executor of the fused spec */
	int			refs;
} FuseSt;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* agg.c */
uint32 combo_eval_slot(AggExec *ex, int ci, uint32 row, int sl);
void preagg_fold(AggExec *ex, uint32 g, int64 rows, const PaVal *v);
void preagg_key_ids(AggExec *ex, int ki, const int64 *key, const uint8 *isnull, uint32 nonnull, const SiftSrcDict *dict, const uint32 *kept, int nkept, uint32 *ids);
void ids_to_gids(AggExec *ex, int n, const uint32 *ids, uint32 *gid);
void table_reset(AggExec *ex);
void scan_items(AggExec *ex);

/* back_host.c */
bool backend_begin(AggExec *ex);
void backend_finish(AggExec *ex);

#pragma GCC visibility pop

/* values the table would cover after adding [lo, hi] */
static inline int64
rt_span(const RangeTab *r, int32 lo, int32 hi)
{
	bool		had = r->lo <= r->hi;

	return (int64) (had ? Max(r->hi, hi) : hi) - (had ? Min(r->lo, lo) : lo) + 1;
}

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

static inline uint32
map_hash(const MapTab *m, const char **ptr, const uint32 *len, const int64 *iv)
{
	uint32		h = 0x9E3779B9;

	for (int k = 0; k < m->nkey; k++)
		h = hash_combine(h, m->ktext[k] ? hash_bytes((const unsigned char *) ptr[k], len[k]) :
						 murmurhash32((uint32) (iv[k] ^ (iv[k] >> 32))));
	return h;
}

static inline bool
map_key_eq(const MapTab *m, uint32 row, const char **ptr, const uint32 *len, const int64 *iv)
{
	for (int k = 0; k < m->nkey; k++)
	{
		size_t		i = (size_t) row * m->nkey + k;

		if (m->ktext[k] ? (m->klen[i] != len[k] || memcmp(m->kptr[i], ptr[k], len[k]) != 0) : m->kint[i] != iv[k])
			return false;
	}
	return true;
}

/* the rows whose key equals the one given; palloc'd in the current context */
static inline int
map_find(const MapTab *m, const char **ptr, const uint32 *len, const int64 *iv, uint32 **rows)
{
	uint32		h = map_hash(m, ptr, len, iv);
	int			n = 0;

	*rows = palloc(Max(m->nrow, 1) * sizeof(uint32));
	for (uint32 r = m->head[h & (m->nslot - 1)]; r != 0; r = m->next[r - 1])
		if (m->hv[r - 1] == h && map_key_eq(m, r - 1, ptr, len, iv))
			(*rows)[n++] = r - 1;
	return n;
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

static inline uint64
dset_hash(uint64 x)
{
	x *= UINT64CONST(0x9E3779B97F4A7C15);
	return x ^ (x >> 32);
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

static inline uint32
key_used(KeyState *k)
{
	return (k->numk ? k->im.n : k->ident ? k->nident : k->ot.n) + 1;
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

	return k->kind == SAK_COMBO || k->kind == SAK_SET || k->kind == SAK_NUM ? NULL : &ex->lay->leaf[k->leaf];
}

#endif							/* SIFT_AGG_AGG_INT_H */
