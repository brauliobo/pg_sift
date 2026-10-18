/*
 * sift_aggspec.h - the plan of a SiftAgg: what the planner proved about a query and the executor runs.
 *
 * The planner (sift_aggplan.c) resolves every column, jsonb path and array element the query uses to a leaf of the column
 * source (sift_colsrc.h) and builds a SiftAggSpec; the spec travels in custom_private as a flat List (sift_aggspec_pack /
 * sift_aggspec_unpack: Integer, String and Node items in a fixed order) so the plan can be copied, cached and sent to
 * parallel workers like any other plan. Nothing in a spec points to the planner's data.
 *
 * Levels: a query without a set-returning function runs at row level (one unit row = one tuple). With one (jsonb_to_recordset,
 * jsonb_array_elements, jsonb_each over an array or object leaf) it runs at element level: one tuple per element of the group
 * leaf `group`, the row level columns of its parent row being available to every tuple.
 */
#ifndef SIFT_AGGSPEC_H
#define SIFT_AGGSPEC_H

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

#include "sift_aggkern.h"
#include "sift_colsrc.h"

#define SIFT_AGG_MAX_KEYS	5		/* group columns (at most 4), the cell keys of the arms and the grouping set number */
#define SIFT_AGG_MAX_VX		16		/* operations of a float8 aggregate argument */
#define SIFT_AGG_MAX_CLEAVES	3	/* leaves one combo reads */
#define SIFT_AGG_MAX_JKEYS	3		/* equality columns of a lookup join */
#define SIFT_AGG_MAX_JCOLS	6		/* columns of the joined table that expressions read */
#define SIFT_AGG_MAX_PARAMS	(SIFT_AGG_MAX_CLEAVES + SIFT_AGG_MAX_JCOLS)
#define SIFT_AGG_MAX_COMBOS	8
#define SIFT_AGG_MAX_JOINS	4
#define SIFT_AGG_MAX_VLEAVES	4
#define SIFT_NUM_MAX_DIGITS	38		/* digits an int128 holds whatever they are */

/* a number of 38 digits at most, 8-byte aligned: palloc'd memory is not 16-byte aligned, the compiler must not assume it */
typedef __int128 sift_i128 __attribute__((aligned(8)));

/*
 * A numeric expression: the postfix program of leaves (numeric(p <= 18, s), int2, int4 or int8 columns), constants and the
 * operations whose result PostgreSQL's numeric functions give exactly, over int128 values scaled by 10^scale of the step's result,
 * where the scale is the dscale numeric_add / numeric_sub (the larger one), numeric_mul (their sum), round / trunc / floor / ceil
 * (the digits asked for) give. The planner proves that no step needs more than SIFT_NUM_MAX_DIGITS digits, so none overflows.
 */
typedef enum SiftAggNxOp
{
	SNX_LEAF,					/* push the leaf (an INT leaf: int2, int4, numeric as its integer) */
	SNX_CONST,					/* push c */
	SNX_ADD,					/* left * 10^a + right * 10^b (aligned to the larger scale) */
	SNX_SUB,
	SNX_MUL,
	SNX_NEG,
	SNX_ABS,
	SNX_ROUND,					/* x / 10^a rounded (half away from zero), times 10^b */
	SNX_TRUNC,					/* toward zero */
	SNX_FLOOR,
	SNX_CEIL
} SiftAggNxOp;

typedef struct SiftAggNx
{
	SiftAggNxOp op;
	int			leaf;
	int			a;
	int			b;
	sift_i128	c;
} SiftAggNx;

typedef struct SiftAggNum
{
	int			n;				/* steps; 0: no program */
	SiftAggNx  *step;
	int			scale;			/* digits after the point of the result: the dscale PostgreSQL gives it */
} SiftAggNum;

typedef enum SiftAggPredKind
{
	SAP_TRUE,
	SAP_FALSE,
	SAP_AND,
	SAP_OR,
	SAP_CMP,					/* leaf op operand: kernels on I32/I64/F64 leaves */
	SAP_IN,						/* leaf IN / NOT IN (array operand): I32 leaves */
	SAP_NULL,					/* leaf IS [NOT] NULL */
	SAP_LUT,					/* boolean expression of one leaf, evaluated by the executor on every distinct value */
	SAP_COMBO,					/* boolean slot (op) of a combo (leaf), see SiftAggCombo */
	SAP_ANY_ELEM,				/* row level: some element of the array leaf `leaf` passes child[0] (element level); op 1: none does */
	SAP_NCMP,					/* numeric expression op operand: the program's values against the operand scaled to the program's scale */
	SAP_GATE					/* boolean expression without columns (stable functions, parameters), evaluated once per scan: true for every row or for none */
} SiftAggPredKind;

typedef struct SiftAggPred
{
	SiftAggPredKind kind;
	int			id;				/* preorder number, the index of the node's executor state */
	int			leaf;			/* SAP_CMP, SAP_IN, SAP_NULL, SAP_LUT; SAP_ANY_ELEM: the array leaf */
	int			op;				/* SAP_CMP: SiftCmpOp (leaf op operand); SAP_IN, SAP_NULL: 1 = NOT IN / IS NOT NULL; SAP_ANY_ELEM: 1 = NOT */
	Node	   *expr;			/* SAP_CMP, SAP_NCMP: operand (no Vars); SAP_IN: array operand; SAP_LUT: bool expression of Param 0 (PARAM_EXEC); SAP_GATE: the condition */
	Oid			argtype;		/* SAP_CMP, SAP_IN, SAP_NCMP: type of the operand (element type for IN) */
	SiftAggNum	num;			/* SAP_NCMP */
	int			level;			/* 0 row, 1 element: of the leaves the node uses (AND/OR: the highest of the children) */
	bool		late;			/* SAP_COMBO whose expression may raise an error, and the AND/OR above one: evaluated only on the rows that reach it (after the snapshot check for a conjunct of the query) */
	int			nchild;
	struct SiftAggPred **child;
	char	   *desc;			/* EXPLAIN */
	double		sel;			/* planner only, not packed: the selectivity of a SAP_CMP, for the cost of the scan */
} SiftAggPred;

typedef enum SiftAggKeyKind
{
	SAK_LEAF,					/* the leaf's value */
	SAK_LUT,					/* expr(leaf value), evaluated once per distinct value */
	SAK_COMBO,					/* key slot (slot) of a combo (leaf) */
	SAK_SET,					/* the number of the grouping set of a row: the last key of a query with grouping sets */
	SAK_NUM						/* a numeric expression (a numeric or int8 column included): the key is its scaled value */
} SiftAggKeyKind;

typedef struct SiftAggKey
{
	SiftAggKeyKind kind;
	int			leaf;			/* SAK_COMBO: the combo */
	int			slot;			/* SAK_COMBO */
	Node	   *expr;			/* SAK_LUT: expression of Param 0 */
	SiftAggNum	num;			/* SAK_NUM */
	Oid			typid;			/* of the key as the query outputs it */
	int32		typmod;
	Oid			collid;
	Oid			eqop;			/* the grouping equality operator */
	char	   *desc;
} SiftAggKey;

typedef enum SiftAggKind
{
	SAA_COUNT_STAR,
	SAA_COUNT,					/* count(leaf) */
	SAA_COUNT_ROWS,				/* count(DISTINCT x) where x is unique per row: distinct parent rows at element level */
	SAA_COUNT_DISTINCT,			/* count(DISTINCT leaf) */
	SAA_SUM_INT,				/* sum of an I32 leaf (int2, int4): int8 */
	SAA_SUM_F64,				/* sum of a F64 leaf: float8 (float4 for a float4 result) */
	SAA_AVG_INT,				/* avg of an I32 leaf: numeric */
	SAA_AVG_F64,				/* avg of a F64 leaf: float8 */
	SAA_MIN,
	SAA_MAX,
	SAA_SUM_NUM,				/* sum of a numeric expression: numeric, added exactly in 128 bits */
	SAA_AVG_NUM,				/* avg of a numeric expression: the numeric division of the exact sum and count */
	SAA_PCT_CONT,				/* percentile_cont(frac) WITHIN GROUP (ORDER BY float8 argument): float8 */
	SAA_PCT_DISC,				/* percentile_disc(frac) WITHIN GROUP (ORDER BY leaf or float8 argument): the argument's type */
	/* aggregates of an expression over a column of a joined table: the ids of its distinct values come from a combo slot */
	SAA_M_COUNT,				/* count(x) */
	SAA_M_DISTINCT,				/* count(DISTINCT x) */
	SAA_M_MIN,					/* min(x), max(x): the distinct values of a group compared at the end by the type's btree order */
	SAA_M_MAX,
	SAA_M_SUM,					/* sum(x), avg(x) of numeric and float8: the count of each distinct value of a group, summed at the end */
	SAA_M_AVG,
	SAA_GROUPING,				/* GROUPING(keys) of the grouping sets */
	/* the aggregates of the outer query over the groups of per-row aggregates (SiftAggArm): they read the arm's inner aggregates */
	SAA_O_COUNT_STAR,			/* count(*): the inner groups */
	SAA_O_COUNT_ROWS,			/* count(DISTINCT id): the rows that have an inner group */
	SAA_O_AVG_F64,				/* avg of a float8 inner aggregate */
	SAA_O_SUM_F64,				/* sum of a float8 inner aggregate */
	SAA_O_SUM_I64,				/* sum of an int8 inner aggregate: numeric */
	/* PostgreSQL's own aggregate (aggfn: sum, avg of int2, int4, int8, interval, avg of float4) of a column of a joined table: its transition function runs on the distinct values, each as many times as it occurred, then its final function */
	SAA_M_PG,
	/* kinds are added at the end */
	SAA_GEN,					/* any other aggregate: PostgreSQL's own transition and final functions on the decoded values (SiftAggGen) */
	/* aggregates of an expression that PostgreSQL evaluates (a combo slot with `direct`): the value of each tuple, added one at a time with the aggregate's own arithmetic */
	SAA_X_COUNT,				/* count(x) */
	SAA_X_MIN,					/* min(x), max(x): the type's btree comparison, a tie takes the later value as PostgreSQL's larger / smaller do */
	SAA_X_MAX,
	SAA_X_SUM,					/* sum(x), avg(x) of numeric (numeric_add) and float8 (float8pl) */
	SAA_X_AVG
} SiftAggKind;

/* `inner column op constant`, a conjunct of the FILTER of an outer aggregate */
typedef struct SiftAggCond
{
	int			col;			/* column of the inner query */
	int			op;				/* SiftCmpOp */
	bool		isint;			/* the column is an int8 aggregate: compared as ci, else as cd */
	int64		ci;
	double		cd;
} SiftAggCond;

/* One step of the postfix program of a float8 aggregate argument (+ - * of leaves and constants, evaluated in double) */
typedef enum SiftAggVxOp
{
	SVX_LEAF,					/* push leaf (an I32 or F64 leaf, widened to double) */
	SVX_CONST,					/* push c */
	SVX_ADD,
	SVX_SUB,
	SVX_MUL,
	SVX_NEG
} SiftAggVxOp;

typedef struct SiftAggVx
{
	SiftAggVxOp op;
	int			leaf;
	double		c;
} SiftAggVx;

/*
 * SAA_GEN: an aggregate whose transition and final functions are not volatile, run by PostgreSQL's own functions through fmgr on the
 * values of the scan (as ExecAgg runs them), so that the result is PostgreSQL's by construction. An argument is a leaf of the
 * source (decoded to a Datum of its type), an expression of up to SIFT_AGG_MAX_GLEAVES leaves evaluated by PostgreSQL's evaluator
 * with the leaves' values as Params, or a constant. The aggregate is serial: its state is a Datum per group, with no merge.
 */
#define SIFT_AGG_MAX_GARGS		FUNC_MAX_ARGS	/* arguments of a generic aggregate: as many as PostgreSQL allows */
#define SIFT_AGG_MAX_GLEAVES	4		/* leaves one argument expression reads */

typedef struct SiftAggGenArg
{
	Oid			typid;
	int			nleaves;		/* 0: a constant (or mapped); 1 with expr NULL: the leaf itself */
	bool		mapped;			/* an expression of the columns of a joined table: the value of slot `slot` of combo `combo` for the row */
	int			combo;
	int			slot;
	int			leaf[SIFT_AGG_MAX_GLEAVES];
	Node	   *expr;			/* a constant, or an expression of Params (PARAM_EXEC) 0..nleaves-1, one per leaf */
} SiftAggGenArg;

typedef struct SiftAggGen
{
	Node	   *aggref;			/* the planner's Aggref (what AggGetAggref gives the aggregate's functions) */
	int			nargs;			/* the Aggref's args, ORDER BY-only (resjunk) ones included */
	int			ntrans;			/* ... the ones the transition function takes: the first ntrans */
	SiftAggGenArg arg[SIFT_AGG_MAX_GARGS];
	Node	   *direct;			/* ordered-set aggregates: the List of direct arguments (constant expressions) */
	bool		distinct;		/* DISTINCT: the distinct values of a group, in btree order, go through the transition function */
	int			darg;			/* DISTINCT: the argument that varies (the others are constants) */
	bool		ordered;		/* WITHIN GROUP: the values of a group go through a tuplesort, then through the transition function */
	bool		parallel;		/* workers keep partial states that the leader merges with the aggregate's combine function (through its serial and deserial functions when the state is internal) */
	bool		hyp;			/* hypothetical-set (ordered): the arguments are the sort columns and every row counts, NULLs included */
	bool		sorted;			/* ORDER BY in the call: the arguments of a group go through a tuplesort on the sort keys, then through the transition function */
	double		cost;			/* planner only, not packed: a tuple's cost in units of cpu_operator_cost */
} SiftAggGen;

typedef struct SiftAggAgg
{
	SiftAggKind kind;
	int			leaf;			/* -1: none */
	int			nvx;			/* SUM_F64 / AVG_F64: the argument is this program instead of a leaf */
	SiftAggVx  *vx;
	SiftAggNum	num;			/* a numeric argument (SUM_NUM, AVG_NUM, and MIN, MAX, COUNT, COUNT_DISTINCT of a numeric): the argument is this program, leaf is -1 */
	bool		distinct;		/* SUM_NUM, AVG_NUM: of the distinct values */
	SiftAggPred *filter;		/* NULL: none */
	Oid			restype;		/* result type of the Aggref */
	Oid			argtype;		/* type of the leaf's SQL values (output type of min/max) */
	Oid			collid;			/* input collation: count(DISTINCT) of dictionary values */
	int			col;			/* SAA_O_*: the inner column it reads, -1 none */
	int			ncond;			/* SAA_O_*: the FILTER */
	SiftAggCond *cond;
	int			combo;			/* SAA_M_*: the combo and slot that give the ids of the values */
	int			slot;
	int			nref;			/* SAA_GROUPING: the keys of its arguments */
	int		   *ref;
	char	   *desc;
	double		frac;			/* SAA_PCT_*: the fraction (a constant in [0, 1]) */
	bool		desc_order;		/* SAA_PCT_*: ORDER BY ... DESC */
	int			share;			/* SAA_PCT_*: the aggregate whose sorted values this one reads (itself, or an earlier one on the same argument) */
	SiftAggGen *gen;			/* SAA_GEN */
	Oid			aggfn;			/* SAA_M_PG: the aggregate function */
} SiftAggAgg;

/*
 * The query is an outer aggregate over arms (a UNION ALL of subqueries, or one subquery) that are aggregates of the scanned rows
 * grouped by a unique row id and by outer keys: every inner group is one row's share of an outer group, so the executor keeps the
 * inner aggregates of the current row per outer group (row by row, as the rows of a unit follow each other), and adds them to the
 * outer aggregates when the row is done. SiftAggSpec.key are the outer keys, SiftAggSpec.agg the outer aggregates.
 */
typedef struct SiftAggVLeaf
{
	int			obj;			/* the object leaf (free keys: children "key" and "value") */
	char	   *key;			/* the value of its element with this key, NULL when the row has none: leaf nleaves + index */
} SiftAggVLeaf;

typedef struct SiftAggArm
{
	uint32		keymask;		/* the outer keys this arm groups by; the others are NULL in its rows */
	uint32		hmask;			/* the hidden keys it groups by: an inner group is finer than an outer one when it has any */
	int			nfilter;		/* the arm's WHERE: boolean slots of combos, ANDed */
	SiftAggPred **filter;
	int			ninner;			/* count(*), count(x), sum(int), sum(float8), avg(float8) over the scan */
	SiftAggAgg *inner;
	int			nhaving;		/* the arm's HAVING, ANDed: `inner aggregate (cond.col) op constant`; a cell that fails is no group of the arm */
	SiftAggCond *having;
	Node	   *hexpr;			/* the rest of the arm's HAVING (what is not `inner aggregate op constant`): a boolean expression of Params ... */
	int			nhp;			/* ... Param i being the inner aggregate hsrc[i] */
	int		   *hsrc;
	int			ncol;			/* columns of the arm */
	int		   *colsrc;			/* per column: its inner aggregate, -1 for the id, a key or a constant */
} SiftAggArm;

/*
 * A table joined by equality to leaves of the scan or to expressions of them (INNER, LEFT, SEMI or ANTI): a tuple's join key is read
 * from the leaves, the table's rows with that key that pass the rest of the ON clause are its matches (any number: the join is exact
 * for duplicates, a tuple is repeated per match, and a LEFT join keeps a tuple without a match once, with NULL columns). The table is
 * read when the scan starts, with the scan's snapshot. The columns the expressions read are colatt[]; they reach the expressions as
 * Params nleaves + i of the combo that owns the join.
 */
typedef enum SiftJoinKind
{
	SJ_INNER,
	SJ_LEFT,
	SJ_SEMI,					/* EXISTS / IN: a tuple is kept once if some row matches, and its columns are the table's no more */
	SJ_ANTI						/* NOT EXISTS: a tuple is kept if no row matches */
} SiftJoinKind;

typedef struct SiftAggJoin
{
	Oid			relid;
	SiftJoinKind kind;
	int			nkey;
	int			keyatt[SIFT_AGG_MAX_JKEYS];		/* column of the joined table */
	Oid			keytype[SIFT_AGG_MAX_JKEYS];
	int			keypos[SIFT_AGG_MAX_JKEYS];		/* position of the leaf in the combo; -1: the key is keyexpr */
	Node	   *keyexpr[SIFT_AGG_MAX_JKEYS];	/* an expression of the combo's leaves (Params), evaluated per distinct tuple of their values */
	int			fslot;			/* slot of the combo that is the rest of the ON clause (leaves and columns of the row), -1 none: a row matches if it holds */
	int			ncol;
	int			colatt[SIFT_AGG_MAX_JCOLS];
	Oid			coltype[SIFT_AGG_MAX_JCOLS];
	int32		coltypmod[SIFT_AGG_MAX_JCOLS];
	Oid			colcoll[SIFT_AGG_MAX_JCOLS];
	char	   *desc;
} SiftAggJoin;

typedef struct SiftAggSlot
{
	bool		is_key;			/* the id of the value in the output table of key `key` (of aggregate `agg`, if >= 0), or a boolean */
	int			key;
	int			agg;
	Node	   *expr;			/* over Params 0..nleaves-1 (the leaves), then the join columns */
	bool		direct;			/* a combo without a join: the value itself (a number into the combo's values, 0 NULL), for the aggregates SAA_X_* */
} SiftAggSlot;

/*
 * A combo evaluates expressions of up to SIFT_AGG_MAX_CLEAVES leaves (and, with a join, of the columns of the matching row) with
 * PostgreSQL's own evaluator, once per distinct tuple of the leaves' values (dictionary entries are compared by value). A combo
 * with a join gives the slots of every output row of a tuple: its matches, or one row of NULLs.
 */
typedef struct SiftAggCombo
{
	int			nleaves;
	int			leaf[SIFT_AGG_MAX_CLEAVES];
	Oid			ptype[SIFT_AGG_MAX_CLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_CLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_CLEAVES];
	int			join;			/* index in SiftAggSpec.join, -1 none */
	int			level;			/* of the leaves: 0 row, 1 element */
	int			nslots;
	SiftAggSlot *slot;
} SiftAggCombo;

static inline bool
is_ordered_agg(const SiftAggAgg *a)
{
	return a->kind == SAA_PCT_CONT || a->kind == SAA_PCT_DISC;
}

typedef struct SiftAggSpec
{
	Oid			indexoid;		/* the column index; InvalidOid: the table itself */
	Oid			heapoid;
	int			group;			/* leaf id of the array/object leaf the query unnests, -1: row level */
	int			nleaves;		/* the leaves the spec reads, ascending */
	int		   *leaves;
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
	SiftAggPred **post;			/* SAP_COMBO nodes */
	int			nvleaf;			/* leaves after the source's: row level scalars read from a key of an object leaf */
	SiftAggVLeaf vleaf[SIFT_AGG_MAX_VLEAVES];
	int			narms;			/* > 0: the two level form (SiftAggArm) */
	SiftAggArm *arm;
	int			nsets;			/* > 0: grouping sets, as PostgreSQL expanded them; the last key is the number of the set */
	int			nhidden;		/* keys after the outer ones (and before the set's): group expressions of the arms that are no outer key. A cell is an inner group, so these are part of its identity; no group has them */
	uint32	   *setmask;		/* per set: the keys it groups by (bit j), and the bit of the set key */
	int			npreds_total;	/* number of SiftAggPred nodes (ids 0..n-1) */
	double		est_groups;
	int			nworkers;		/* parallel workers the plan asks for (0: the leader scans alone) */
	char	   *desc_quals;		/* EXPLAIN: the conjuncts, as text */
	Node	   *search;			/* BM25 match set the rows are restricted to: a List of the index Oid (Const) and the query operands (docs/COL-SEARCH.md); NULL: none */
	Node	   *topn;			/* ORDER BY .. LIMIT n from the units: an integer list (sift_topn.h), the scan emits the heap rows of the n best instead of groups; NULL: none */
	char	   *desc_topn;		/* EXPLAIN: the sort columns */
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
 * expression of one) and plain aggregates (count, sum, avg, min, max, count(DISTINCT) of an integer or text column). The others
 * (combos, joins, ordered sets, two level arms, grouping sets, numeric and float keys, numeric states, mapped aggregates) keep
 * their state in the leader: the plan asks for no workers and the scan has no lanes.
 */
extern bool sift_aggspec_parallel_ok(const SiftAggSpec *s);

static inline bool
sift_aggspec_ordered(const SiftAggSpec *s)
{
	if (!sift_aggspec_parallel_ok(s))
		return false;
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_SUM_F64 || s->agg[i].kind == SAA_AVG_F64)
			return true;
	return false;
}

/* The comparisons among the conjuncts that bound an integer leaf of the rows (= < <= > >= against an integer operand): what a zone map can prune. nbase: the leaves of the source (not the spec's virtual ones) */
extern List *sift_aggspec_zone_cmps(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase);

/* Is the (top level, row level) predicate a text test the source can run on the encoded form? The constants are palloc'd in CurrentMemoryContext */
extern bool sift_aggspec_str_test(const SiftAggPred *p, const SiftSrcLayout *lay, SiftSrcStrTest *out);

extern int64 sift_aggspec_operand_int(Oid t, Datum d);

extern List *sift_aggspec_pack(const SiftAggSpec *spec);

/* Arms that read the same rows for the same aggregates and differ in their keys: can one scan serve them (sets of one spec, keyoff[i] the first key of arm i) */
extern bool sift_aggspec_fusable(const SiftAggSpec *a, const SiftAggSpec *b);
extern SiftAggSpec *sift_aggspec_fuse(SiftAggSpec *const *m, int n, int *keyoff);
extern SiftAggSpec *sift_aggspec_unpack(const List *items);

#endif							/* SIFT_AGGSPEC_H */
