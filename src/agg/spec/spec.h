/*
 * agg/spec/spec.h - the plan of a SiftAgg: the spec and its calls
 * The order of the groups, SiftAggSpec itself, the functions that pack, unpack and ask it,
 * and the shapes of the pre-aggregates; the last of the three parts of agg/spec.h.
 */
#ifndef SIFT_AGG_SPEC_SPEC_H
#define SIFT_AGG_SPEC_SPEC_H

#include "agg/spec/join.h"

/* a key of the order the groups are emitted in: the sort operator, collation and nulls order of the parent's pathkey */
typedef struct SiftAggOrd
{
	int			key;
	Oid			sortop;
	Oid			collation;
	bool		nulls_first;
	Const	   *subst;			/* COALESCE(key, subst) is the sort value: a NULL key sorts as this constant; NULL: the key itself */
} SiftAggOrd;

typedef struct SiftAggSpec
{
	Oid			indexoid;		/* the column index; InvalidOid: the table itself. A partitioned table: that of its first partition */
	Oid			heapoid;
	int			nparts;			/* > 0: heapoid is a partitioned table and the scan reads these partitions, whose indexes have one layout (agg/part.h) */
	Oid		   *partheap;
	Oid		   *partindex;
	List	   *partprune;		/* the conditions on the partition key that a parameter or a stable function decides: the scan drops the partitions they refute when it starts */
	int			group;			/* leaf id of the array/object leaf the query unnests, -1: row level */
	bool		part_final;		/* the group key holds the partition key of the table: a serial scan finishes the groups of a partition when its units end (they get no row from another) and starts the table again */
	bool		left_lateral;	/* LEFT JOIN LATERAL ON true: a row none of whose elements passes the lateral's own conditions is one tuple with NULL element columns */
	int			nleaves;		/* the leaves the spec reads, ascending */
	int		   *leaves;
	uint8	   *recheck;		/* per entry of leaves: how the query reads the leaf when the source holds a value it cannot store (SiftSrcRecheck, SRK_NONE: such a value is an error) */
	uint64	   *gate;			/* per entry of leaves: the aggregates (bit j: agg[j]) that read the leaf, all of them with a FILTER; a row of a value the leaf cannot store raises only if the FILTER of one holds (0: it raises for every row) */
	int			npred;			/* conjuncts, ANDed */
	SiftAggPred **pred;
	int			nkeys;
	SiftAggKey *key;
	int			naggs;
	SiftAggAgg *agg;
	int			njoin;
	SiftAggJoin *join;
	int			ncombo;
	SiftAggCombo *combo;
	int			npost;			/* conjuncts over the joined rows (boolean slots of join combos), ANDed after the joins */
	int			nprepost;		/* prefix applied inside the nullable input, before its preserved join */
	SiftAggPred **post;			/* SAP_COMBO nodes */
	uint32	   *postsets;		/* per post conjunct: the sets of a fused pass that apply it (bit i: set i); 0: all of them */
	uint32	   *setaggs;		/* per aggregate of a fused pass: the sets that update it (bit i: set i); NULL: all of them update all */
	SiftAggPred **setpred;		/* per set of a fused pass: the conjuncts its arm has beyond those of pred, as one node (NULL: none); NULL: no set has any */
	int			nvleaf;			/* leaves after the source's: row level scalars read from a key of an object leaf */
	SiftAggVLeaf vleaf[SIFT_AGG_MAX_VLEAVES];
	int			narms;			/* > 0: the two level form (SiftAggArm) */
	SiftAggArm *arm;
	SiftAggSpine *spine;		/* the arms are joined to a spine; NULL: they are not */
	int			nsets;			/* > 0: grouping sets, as PostgreSQL expanded them; the last key is the number of the set */
	int			nhidden;		/* keys after the outer ones (and before the set's): group expressions of the arms that are no outer key. A cell is an inner group, so these are part of its identity; no group has them */
	uint32	   *setmask;		/* per set: the keys it groups by (bit j), and the bit of the set key */
	int			npreds_total;	/* number of SiftAggPred nodes (ids 0..n-1) */
	double		est_groups;
	double		est_rows;		/* the rows the planner expects to aggregate: with est_groups, what a group of a count(DISTINCT) can hold */
	int			nworkers;		/* parallel workers the plan asks for (0: the leader scans alone) */
	double		wide_unit_rows;	/* the workers are priced for the rows the scan reads, not for the tuples PostgreSQL expects of the joins (workers_ratio): the rows of a unit, > 0 where they are, and then par_launch keeps one process for a scan of few rows; 0 where they are not */
	const char *serial;			/* why the plan asks for none whatever the cost, for EXPLAIN VERBOSE (the planner's reasons; sift_aggspec_serial_reason has those of the spec); else NULL */
	char	   *desc_quals;		/* EXPLAIN: the conjuncts, as text */
	Node	   *search;			/* BM25 match set the rows are restricted to: a List of the index Oid (Const) and the query operands (docs/COL-SEARCH.md); NULL: none */
	Node	   *topn;			/* ORDER BY .. LIMIT n from the units: an integer list (agg/topn.h), the scan emits the heap rows of the n best instead of groups; NULL: none */
	char	   *desc_topn;		/* EXPLAIN: the sort columns */
	int			gtopn_k;		/* ORDER BY an aggregate LIMIT n (+ OFFSET): the groups that rank after the n-th of aggregate gtopn_agg are not emitted (agg/gtopn.c); 0: none */
	int			gtopn_agg;
	bool		gtopn_desc;
	bool		gtopn_nulls_first;
	int			nord;			/* > 0: the groups are emitted in the order of these keys (agg/gorder.c), which the path's pathkeys promise to the parent */
	SiftAggOrd *ord;
} SiftAggSpec;

/* "-123.450" as -123450 with scale 3; false unless it is a plain decimal of at most SIFT_NUM_MAX_DIGITS digits (not NaN, not infinite) */
extern bool sift_parse_decimal(const char *s, sift_i128 *v, int *scale);
extern sift_i128 sift_pow10(int e);

/*
 * Float sums depend on the order of the additions. A scan with one is split in SIFT_AGG_LANES lanes, lane l being the
 * superblocks l, l + SIFT_AGG_LANES, ...; each lane is summed in superblock order and the lanes are added in lane order,
 * so that the result does not depend on how many processes scan (docs/COL-PLAN.md, "Parallel scan").
 */
#define SIFT_AGG_LANES		16

/*
 * The specs whose group tables can leave a process (dumped and merged by value) and be reset: plain keys (a column, or an
 * expression of one) and plain aggregates (count, sum, avg, min, max, count(DISTINCT) of an integer or text column, the outer aggregates of
 * two level arms). The others (ordered sets, numeric and float keys, numeric states, mapped aggregates) keep their state in the leader: the
 * plan asks for no workers and the scan has no lanes. A join to a child plan (a sub-select, VALUES list or function) is read by the leader,
 * who shares it with the workers; a child plan run per combination of leaves is the leader's alone: no workers either.
 */
extern bool sift_aggspec_parallel_ok(const SiftAggSpec *s);
extern const char *sift_aggspec_serial_reason(const SiftAggSpec *s, int *agg);

/* the specs whose group table can also go to partitions on disk past hash memory (agg/spill.c): count, sum, avg, min and max over one pass */
extern bool sift_aggspec_spillable(const SiftAggSpec *s);

/* the count(DISTINCT x) a scan may stream in the order of the zone minimum of x (agg/dist/), -1: none */
extern int sift_aggspec_dstream(const SiftAggSpec *s);

/* a float sum: the order of its additions is part of its result */
static inline bool
sift_aggkind_float_sum(const SiftAggAgg *a)
{
	return a->kind == SAA_SUM_F64 || a->kind == SAA_AVG_F64 || a->kind == SAA_O_SUM_F64 || a->kind == SAA_O_AVG_F64 ||
		((a->kind == SAA_X_SUM || a->kind == SAA_X_AVG) && a->restype == FLOAT8OID);
}

/* a scan with a spine or a correlated child plan: workers were added for them after the release that scanned the others in lanes */
extern bool sift_aggspec_workers_only(const SiftAggSpec *s);

/* a scan with a float sum, in the specs whose scan can be split in lanes */
static inline bool
sift_aggspec_float_sums(const SiftAggSpec *s)
{
	if (!sift_aggspec_parallel_ok(s))
		return false;
	for (int i = 0; i < s->naggs; i++)
		if (sift_aggkind_float_sum(&s->agg[i]))
			return true;
	return false;
}

/*
 * The float sums are added in lane order, whoever scans: in a plan with workers (launched or not), and in the scans that were always added so, with
 * or without workers. The scans that gained workers later (sift_aggspec_workers_only) add in the order of the rows when the plan has none, as they did.
 */
static inline bool
sift_aggspec_ordered(const SiftAggSpec *s)
{
	return sift_aggspec_float_sums(s) && (s->nworkers > 0 || !sift_aggspec_workers_only(s));
}

/* the types sift_aggspec_operand_int reads */
extern bool sift_aggspec_integer_type(Oid t);

/* The comparisons among the conjuncts that bound an integer leaf of the rows (= < <= > >= against an integer operand): what a zone map can prune. nbase: the leaves of the source (not the spec's virtual ones) */
extern List *sift_aggspec_zone_cmps(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, bool nulls);

/* ... and whether one predicate is such a comparison; the zone tests of an OR are those of its disjuncts */
extern bool sift_aggspec_zone_cmp(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase);
/* a test built of AND, OR and zone comparisons: the span of a calendar cell decides it (every row passes, none does, or the cell is cut) */
extern bool sift_aggspec_span_pred(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase);
extern bool sift_aggspec_null_zone(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase);

/*
 * The TIDs the SAP_TID conjuncts keep together, as the keys (block << 16 | offset) [*lo, *hi) (empty when *lo >= *hi); false when the scan
 * has none. A key is ordered as PostgreSQL orders a TID.
 */
#define SIFT_TID_KEY_END	(UINT64CONST(1) << 48)
extern bool sift_aggspec_tid_range(const SiftAggSpec *s, uint64 *lo, uint64 *hi);
extern bool sift_aggspec_plain(const SiftAggSpec *s);

/* agg/cost/leaf.c: does a predicate test the leaf; is every reader of a numeric leaf a sum or average that adds its dictionary entries (SiftAggAgg.wsum) */
extern bool pred_uses_leaf(const SiftAggSpec *s, const SiftAggPred *p, int leaf, bool values);
extern bool combo_wsum_only(const SiftAggSpec *s, int ci);
extern bool leaf_wsum_only(const SiftAggSpec *s, int leaf);

/* How the pre-aggregates answer a scan: the histogram of dim of the cube (-1: the sums of the leaves), or the rollup of the list leaf list */
typedef struct SiftAggPaShape
{
	int			dim;
	bool		cal;				/* dim >= 0: the cube's calendar dim of that date or timestamp leaf (the cells of a unit), not the leaf's own dim */
	uint8		calmask;			/* cal: the grains (1 << SIFT_CAL_*) of the cells that hold one value of the key and the tests; a unit with other cells scans */
	bool		hcal;				/* hdim is the calendar dim of the leaf */
	bool		hcalalt;			/* hdim is the leaf's own dim, and a unit with no histogram of it (too many days) answers from the calendar dim of the leaf, with calmask */
	bool		calalt;				/* dim is the leaf's own dim, tested by comparisons the span of a cell decides: a unit with no histogram of it answers from the calendar dim of the leaf, with calmask */
	bool		ktab;				/* the dim (or hdim) is only a key table of whole segments: a unit that is not one is scanned */
	int			hdim;				/* dim < 0 only: the dim a zone comparison tests, whose histogram answers a unit whose zone is not inside it; -1: none */
	int			list;				/* >= 0: the rollup of this SCL_ARRAY / SCL_OBJECT leaf answers, the elements are the rows */
	bool		rows;				/* the rollup of the rows of the unit answers (list is -1): the dims are columns of the index (column format minor 8) */
	bool		nullz;				/* the IS [NOT] NULL conjuncts of columns that are no dim are decided by the zone of a unit, like the comparisons: a unit whose zone does not hold them for all its rows is scanned */
	int			ndims;				/* rollup: the leaves a unit's rollup must hold as dims: the keys, then the others the conjuncts read */
	int			rdim[SIFT_SRC_MAX_RDIMS];
	int			nmeas;				/* ... and as measures */
	int			rmeas[SIFT_SRC_MAX_MEAS];
} SiftAggPaShape;

/* the shape is answered from a rollup (of a list's elements or of the rows), not from histograms or sums */
#define PA_ROLLUP(pa) ((pa).list >= 0 || (pa).rows)

/*
 * Can a unit be answered from its pre-aggregates (docs/COL-PREAGG.md, agg/colsrc.h SiftSrcUnitAgg)? A scan of one source without joins, combos
 * or sets. Over the rows: at most one group key, that is a column of a histogram, and aggregates of the stored measures: count(*), count(m), sum
 * and avg of an integer or numeric m, and min and max when there is no key. Conjuncts on other leaves must be zone comparisons (a unit answers
 * when its zone lies inside them), the rest must test the key's leaf (or, without a key, one leaf: dim). Without a key, a unit whose zone is not
 * inside a zone comparison on a dim of a histogram (hdim) answers from the groups of that histogram that pass it.
 * Over the elements of a list (an array of objects or scalars, a map): any number of group keys, conjuncts and count(leaf) over the dims of its
 * rollup (low-cardinality scalars of the elements), and count, sum, avg (integer, numeric and float8), min and max of its measures; the conjuncts
 * of the rows must be zone comparisons. The unit answers when its rollup holds these dims and measures, else it is scanned.
 * Over the rows, when that fails (two or more keys, a key and a conjunct on another column) and the index has the rollup of its rows (reloption
 * rollup_dims): the same, with the columns of the rollup as dims, the conjuncts all on dims and the aggregates of integer and numeric measures.
 */
/*
 * Calendar expressions (agg/expr/cal.c): sift_aggexpr_cal classifies a key or a test of a date, timestamp or timestamptz leaf (Param 0) and gives in mask the
 * grains of calendar cells (bit 1 << SIFT_CAL_*) that hold one value of it; SAC_NOT: it is no calendar expression, SAC_ZONE: it depends on the session time
 * zone, SAC_FINE: it changes within a day (hour, minute, the timestamp itself). sift_aggkey_cal does so for a SAK_LUT key that is evaluated for each day
 * of its rows.
 */
enum {SAC_NOT, SAC_OK, SAC_ZONE, SAC_FINE};
extern int	sift_aggexpr_cal(Node *e, uint8 *mask);
extern bool sift_aggexpr_utc_shadowed(Node *e);
extern int	sift_aggkey_cal(const SiftAggKey *k, uint8 *mask);
extern bool sift_aggspec_preagg(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, SiftAggPaShape *out);

/* Why a spec is not answered from the pre-aggregates, in the order the checks run */
typedef enum SiftPaWhy
{
	SPW_OK,
	SPW_OFF,					/* sift.agg_preagg is off */
	SPW_NONE,					/* the index stores no pre-aggregates */
	SPW_ARMS,					/* the scan feeds union arms */
	SPW_SETS,					/* grouping sets */
	SPW_JOIN,					/* a joined table */
	SPW_COMBO,					/* a combination of keys or a lookup */
	SPW_POST,					/* a post-aggregation step */
	SPW_VLEAF,					/* a virtual leaf */
	SPW_NO_AGG,					/* no aggregate (a DISTINCT) */
	SPW_SEARCH,					/* a text search narrows the rows */
	SPW_TOPN,					/* a top-N */
	SPW_NOT_LIST,				/* the unnested leaf is not an array or a map */
	SPW_NO_ROLLUP,				/* the index writes no rollup of the list */
	SPW_KEY_EXPR,				/* a group key is an expression, not a leaf */
	SPW_KEY_LEAF,				/* a group key is a leaf that no rollup holds as a dim (not an integer or text scalar of the list) */
	SPW_PRED,					/* a conjunct tests more than the dims, or a leaf of the rows besides the zones */
	SPW_AGG_FILTER,				/* an aggregate with FILTER */
	SPW_AGG_DISTINCT,			/* an aggregate with DISTINCT */
	SPW_AGG_KIND,				/* an aggregate the tuples cannot add up */
	SPW_AGG_ARG,				/* its argument is not a plain leaf of the list */
	SPW_AGG_TYPE,				/* ... or has a type the rollup does not sum */
	SPW_AGG_APPROX,				/* sift.approx_count_distinct of a leaf without stored sketches (hll_columns), or of the groups of a histogram */
	SPW_DIM_NOT_STORED,			/* a dim or count(leaf) the rollup of the list does not hold */
	SPW_MEAS_NOT_STORED,		/* a measure it does not hold */
	SPW_ROW_KEYS,				/* rows: more than one group key */
	SPW_ROW_KEY,				/* rows: the key is not a plain column */
	SPW_ROW_PRED,				/* rows: a conjunct the histogram cannot test */
	SPW_ROW_NO_HIST,			/* rows: the index writes no histogram of the key */
	SPW_ROW_NO_SUMS,			/* rows: no sums at all */
	SPW_ZONE,					/* a comparison of the rows with NULL, or an IN list with gaps, decides no unit */
	SPW_ROW_CAL_ZONE,			/* rows: the calendar key uses the session time zone (a timestamptz or a timestamp cast to one) */
	SPW_ROW_CAL_FINE			/* rows: the calendar key changes within a day (hour, minute, the timestamp itself) */
} SiftPaWhy;

extern SiftPaWhy sift_aggspec_preagg_why(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, SiftAggPaShape *out);
extern const char *sift_pawhy_name(SiftPaWhy why);
extern const char *sift_pawhy_slug(SiftPaWhy why);
extern bool sift_pawhy_fixable(SiftPaWhy why);

/*
 * What the two ways to answer a unit cost, in user instructions measured on the rollup benchmark (docs/COL-PREAGG.md section 6): *elem for an element of the
 * list scanned, *tuple for a tuple of its rollup (varint decode of the columns the query reads, the fold into the groups). The rollup answers a unit when its
 * tuples cost less than its elements; sift.agg_rollup_cost scales the tuple (0: always the rollup).
 */
extern void sift_aggspec_unit_costs(const SiftAggSpec *s, const SiftSrcLayout *lay, double *elem, double *tuple);
extern double sift_agg_rollup_cost;
extern PGDLLIMPORT bool sift_agg_preagg_distinct;	/* sift.agg_preagg_distinct */
extern Datum sift_approx_trans(struct FunctionCallInfoBaseData *fcinfo);	/* agg/approx.c: the transition function of sift.approx_count_distinct */
#define SIFT_AGG_INSTR_PER_OP	4.4		/* user instructions of one operator of the planner's cost model (agg/cost/model.h) */

/* The one leaf a predicate of the rows (group -1) or of the elements of the array leaf group reads (-1: none), -2 if it is not a test of a plain leaf of that level (a combo, a late expression) */
/* The leaf of a numeric program that is `leaf * mul + add` (a lone leaf, or int8 arithmetic with a constant that ends in the range-checked cast), -1 if it is more */
extern int sift_aggnum_linear(const SiftAggNum *n, sift_i128 *mul, sift_i128 *add);
extern int sift_aggspec_pred_leaf(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, int group);

/* Is the (top level, row level) predicate a text test the source can run on the encoded form? The constants are palloc'd in CurrentMemoryContext */
extern bool sift_aggspec_elem_test(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, SiftSrcStrTest *out);
extern bool sift_aggspec_str_test(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, SiftSrcStrTest *out);
extern bool sift_aggspec_text_bound(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, SiftSrcStrTest *out);

/* Whether the collation lowers A-Z to a-z in a UTF-8 database: ILIKE and ~* of an ASCII part compare the ASCII letters in lower case then */
extern bool sift_aggspec_ascii_fold(Oid collid);

/*
 * The literals (spec_lits.c) every string that matches a pattern test of the dictionary holds: up to SIFT_PAT_LITS of at least 3 bytes, longest first, each of
 * them in lower case and to compare with the ASCII letters of the strings folded when fold[i] (ILIKE, ~*); neg: the test is the NOT of it, a string without one
 * of them passes.
 */
#define SIFT_PAT_LITS	3
typedef struct SiftPatLits
{
	int			n;
	bool		neg;
	bool		fold[SIFT_PAT_LITS];
	const char *lit[SIFT_PAT_LITS];
	uint32		len[SIFT_PAT_LITS];
} SiftPatLits;

/* The literals of the boolean expression of a lookup table over a dictionary (one pattern test of Param 0 and a constant, its NOT, or an AND of tests); false when there are none */
extern bool sift_pattern_lits(Node *e, SiftPatLits *out);
extern bool sift_pattern_lits_of(Oid funcid, Oid collid, const char *pat, uint32 len, SiftPatLits *out);

extern int64 sift_aggspec_operand_int(Oid t, Datum d);
extern Datum sift_cal_bound(Node *cal, int mode, Datum operand);
extern bool sift_cal_nan(Oid rtype, Datum d);
/* the window a periodic calendar step is monotone in (SAP_LUT, SAP_COMBO with cal set: calmode is a SCW_*) */
typedef enum SiftCalWindow
{
	SCW_NONE,
	SCW_YEAR,
	SCW_MONTH,
	SCW_DAY,
	SCW_HOUR,
	SCW_MINUTE
} SiftCalWindow;
#define SCW_MAXWIN 256		/* the most windows a prefilter enumerates */
#define SCW_YEAR4 32			/* to_char starts with YYYY: only years 1000 to 9999 are in order as text */
extern int64 sift_cal_windows(int win, int64 min, int64 max);
extern int	sift_cal_periodic(Node *cal, Oid ttype, int win, Datum operand, int64 min, int64 max, int maxwin, int64 **arms);
/* the bytes the scan allocates and clears to test an IN list of nv values over a range of span values (agg/pred_in.c; 0: a hash table), for the cost */
extern double sift_agg_in_table_bytes(uint64 span, int nv);
extern double sift_agg_in_test_ns(uint64 span, int nv);

extern List *sift_aggspec_pack(const SiftAggSpec *spec);

/* Arms that read the same rows for the same aggregates and differ in their keys, the leaves and slots those read: can one scan serve them (sets of one spec; keymap[i][j] the key of the fused spec that is key j of arm i) */
extern bool sift_aggspec_fusable(SiftAggSpec *const *m, int n, bool preagg);
extern SiftAggSpec *sift_aggspec_fuse(SiftAggSpec *const *m, int n, int **keymap, int ***aggmap);
extern SiftAggSpec *sift_aggspec_unpack(const List *items);

#endif							/* SIFT_AGG_SPEC_SPEC_H */
