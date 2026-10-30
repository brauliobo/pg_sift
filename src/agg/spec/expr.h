/*
 * agg/spec/expr.h - the plan of a SiftAgg: what it computes
 * The limits, the numeric and float expressions, the predicates, the keys, the aggregates,
 * the arms and the spine; the first of the three parts of agg/spec.h.
 */
#ifndef SIFT_AGG_SPEC_EXPR_H
#define SIFT_AGG_SPEC_EXPR_H

#include "catalog/pg_type_d.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

#include "agg/kern.h"
#include "agg/colsrc.h"

/* Each physical key has one bit in the grouping/arm masks, including the set number. */
#define SIFT_AGG_MAX_KEYS	(sizeof(uint32) * CHAR_BIT)
#define SIFT_AGG_MAX_VX		16		/* operations of a float8 aggregate argument */
#define SIFT_AGG_MAX_CLEAVES	8	/* leaves one combo reads */
#define SIFT_AGG_MAX_JKEYS	3		/* equality columns of a lookup join */
#define SIFT_AGG_MAX_JCOLS	16		/* columns of the joined table that expressions read */
#define SIFT_AGG_MAX_JIDX	3		/* equalities of a column of the joined table with a value, for an index scan that loads it */
#define SIFT_AGG_NULL_INPUT	(SIFT_AGG_MAX_CLEAVES + SIFT_AGG_MAX_JCOLS)
#define SIFT_AGG_NULL_INPUT_LOC (-0x51F7)
#define SIFT_AGG_LEVEL_LOC (-0x51F8)	/* location of a Param an enclosing query level sets for each row it passes down (a LATERAL or correlated reference): the scan reads the executor's value */
#define SIFT_AGG_MAX_PARAMS	(SIFT_AGG_NULL_INPUT + 1)
#define SIFT_AGG_INIT_PARAM	(1 << 24)	/* added to the id of an ARRAY sub-select's output Param in a lookup table expression (Param 0 is the leaf's value) */
#define SIFT_AGG_MAX_COMBOS	128
#define SIFT_AGG_MAX_JOINS	32
#define SIFT_AGG_MAX_VLEAVES	16
#define SIFT_NUM_PRODUCT_DIGITS	48	/* digits of a product of two decimal expressions that a sum adds in 256 bits (agg_sum_add_product) */
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
	SNX_CEIL,
	SNX_CMP3,					/* left <=> right as -1, 0 or 1 (a, b as SNX_ADD align the scales); NaN equals NaN and is greater than every number */
	SNX_INTCAST					/* numeric to an integer of a bytes (2, 4, 8): a NaN raises PostgreSQL's error; the value is the one rounded before; b = 1: a value outside int8 raises "bigint out of range" (the result of int8 arithmetic, a = 8) */
} SiftAggNxOp;

/* the value of a NaN in a program (numeric(p <= 18, s) columns hold it as a flag): greater than every number a program has */
#define SIFT_NX_NAN	((sift_i128) (~(unsigned __int128) 0 >> 1))

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
	int			dig;			/* digits the proof gave the result: a sum of it adds up exactly (agg_sum_add), the pre-aggregates take only 26 */
} SiftAggNum;

#define SIFT_AGG_MAX_WIDE	8		/* conjuncts on wide leaves (SAP_WIDE) of one scan: what a column source takes in set_wide */

/* SAP_NULL ops besides 0 (IS NULL) and 1 (IS NOT NULL): a slot that is a JSON null, a NULL slot that is not (the key is missing) */
#define SNULL_JSON		2
#define SNULL_MISSING	3

#define SIFT_AGG_SEQ_ELEMS	8	/* the elements of a list a test in order names (SAP_ANY_ELEM, op 2) */

typedef enum SiftAggPredKind
{
	SAP_TRUE,
	SAP_FALSE,
	SAP_AND,
	SAP_OR,
	SAP_CMP,					/* leaf op operand: kernels on I32/I64/F64 leaves */
	SAP_IN,						/* leaf IN / NOT IN (array operand): I32 leaves */
	SAP_NULL,					/* leaf IS [NOT] NULL; the ops SNULL_JSON and SNULL_MISSING tell a JSON null from a missing key among the NULL slots (element leaves) */
	SAP_LUT,					/* boolean expression of one leaf, evaluated by the executor on every distinct value */
	SAP_COMBO,					/* boolean slot (op) of a combo (leaf), see SiftAggCombo */
	SAP_ANY_ELEM,				/* row level: some element of the array leaf `leaf` passes child[0] (element level); op 1: none does; op 2: the list is as long as the children and the element at position k passes child[k] */
	SAP_NCMP,					/* numeric expression op operand: the program's values against the operand scaled to the program's scale */
	SAP_GATE,					/* boolean expression without columns (stable functions, parameters), evaluated once per scan: true for every row or for none */
	SAP_TYPE,					/* jsonb_typeof(leaf's path) in the types op (SIFT_SRC_JT_* bits); a conjunct of the query only: the source keeps the rows (sift_colsrc_filter_types), the executor does not evaluate it */
	SAP_WIDE,					/* leaf op operand (SWT_* op, SiftCmpOp or IN / NOT IN / IS [NOT] NULL) on a wide leaf: numeric of more than 18 digits, money, interval; a conjunct of the query only, tested by the source (sift_colsrc_filter_wide), the executor does not evaluate it */
	SAP_LEAFCMP,				/* leaf op leaf2: two scalar leaves of one row (int2/int4, int8, date, timestamp or float columns of one type class and width); NULL on either side is not true */
	SAP_ELEM_ROW,				/* element level, inside SAP_ANY_ELEM: the element leaf `leaf` op the row leaf `leaf2` of the element's row (SiftCmpOp); NULL on either side is not true */
	SAP_TID,					/* ctid op constant tid (op: SiftCmpOp but SCMP_NE) at the top level of the WHERE: the scan reads the superblocks of the range, the executor tests the rows of the edge ones (sift_aggspec_tid_range) */
	SAP_ELEM_EXISTS,			/* after the joins: some element of the array leaf `leaf2` of the tuple's row passes child[0] (element level conjuncts of the leaves, if any) and boolean slot `op` of the scoped combo `leaf`, which reads the element's leaves, the row's and the tuple's lookup outputs */
	SAP_STEP,					/* boolean expression of one timestamp leaf that changes only on a boundary of a second, minute, hour or day (`step` seconds, on the session zone's clock with `zoned`): the executor evaluates it once for each such step of the data, not for each distinct timestamp */
	SAP_MATCHED					/* the tuple has a row of the join of combo `leaf` (a conjunct over the joined rows, of the arms of a fused pass that have an inner join where the pass has a left one) */
} SiftAggPredKind;

typedef struct SiftAggPred
{
	SiftAggPredKind kind;
	int			id;				/* preorder number, the index of the node's executor state */
	int			leaf;			/* SAP_CMP, SAP_IN, SAP_NULL, SAP_LUT, SAP_TYPE, SAP_WIDE; SAP_ANY_ELEM: the array leaf; SAP_ELEM_ROW: the element leaf */
	int			leaf2;			/* SAP_ELEM_ROW: the leaf of the row, -1 otherwise */
	int			op;				/* SAP_CMP, SAP_ELEM_ROW: SiftCmpOp (leaf op operand); SAP_IN, SAP_NULL: 1 = NOT IN / IS NOT NULL; SAP_ANY_ELEM: 1 = NOT, 2 = in order; SAP_TYPE: the types; SAP_WIDE: SWT_* (agg/colsrc.h) */
	Node	   *expr;			/* SAP_CMP, SAP_NCMP, SAP_WIDE: operand (no Vars); SAP_IN, SAP_WIDE IN / NOT IN: array operand; SAP_LUT: bool expression of Param 0 (PARAM_EXEC); SAP_GATE: the condition */
	Oid			argtype;		/* SAP_CMP, SAP_IN, SAP_NCMP, SAP_WIDE: type of the operand (element type for IN) */
	Node	   *cal;			/* SAP_LUT of an equality of a periodic calendar step (extract(hour FROM ts) = 14, to_char(ts, 'MM') = '03'): List (f(placeholder), operand), whose ranges the source tests first (init_filter.c); SAP_CMP of a calendar step (agg/pred_cal.c): f(placeholder) of the calendar step `f(x) op C` (agg/pred_cal.c); the operand C of `expr` is turned into the least x with f(x) >= C (calmode 1) or the greatest x with f(x) <= C (2) when the scan starts */
	int			calmode;
	int			step;			/* SAP_STEP: the seconds one evaluation covers (1, 60, 3600, 86400) */
	int			zoned;			/* SAP_STEP: the steps are those of the session zone's clock (SIFT_ZONE_STEP), not of UTC */
	SiftAggNum	num;			/* SAP_NCMP */
	int			level;			/* 0 row, 1 element: of the leaves the node uses (AND/OR: the highest of the children) */
	bool		late;			/* SAP_COMBO whose expression may raise an error, and the AND/OR above one: evaluated only on the rows that reach it (after the snapshot check for a conjunct of the query) */
	bool		pass_poison;	/* over a list unnested by jsonb_to_recordset: SAP_ANY_ELEM passes a row whose list raises when it is read again from the heap, and the error is raised where the rows reach the aggregates; SAP_ELEM_EXISTS raises it itself, for the tuples that reach it */
	int			nchild;
	struct SiftAggPred **child;
	char	   *desc;			/* EXPLAIN */
	double		sel;			/* planner only, not packed: the selectivity of a SAP_CMP, for the cost of the scan */
} SiftAggPred;

/* bit and varbit have no hash operator class, but equal values have the same image (the padding bits are zero): a key of one is grouped by image */
#define SIFT_EQ_BY_IMAGE(opcode) ((opcode) == F_BITEQ || (opcode) == F_VARBITEQ)

typedef enum SiftAggKeyKind
{
	SAK_LEAF,					/* the leaf's value */
	SAK_LUT,					/* expr(leaf value), evaluated once per distinct value */
	SAK_COMBO,					/* key slot (slot) of a combo (leaf) */
	SAK_SET,					/* the number of the grouping set of a row: the last key of a query with grouping sets */
	SAK_NUM,					/* a numeric expression (a numeric or int8 column included): the key is its scaled value */
	SAK_SPINE					/* a column of the spine of a two level aggregate: no arm computes it, the spine rows give its ids */
} SiftAggKeyKind;

#define SIFT_ZONE_STEP	1
#define SIFT_ZONE_CUT	2

typedef struct SiftAggKey
{
	SiftAggKeyKind kind;
	int			leaf;			/* SAK_COMBO: the combo */
	int			slot;			/* SAK_COMBO */
	Node	   *expr;			/* SAK_LUT: expression of Param 0 */
	int			bucket;			/* SAK_LUT of a timestamp leaf: the expression is the same within this many seconds, so it is evaluated once per bucket of rows */
	int			zoned;			/* bucket of a timestamptz leaf on the clock of the session's time zone when the scan starts, not of UTC: SIFT_ZONE_STEP the bucket is a step of that clock, SIFT_ZONE_CUT
								 * date_trunc to a second, minute or hour, which keeps the offset of the row: the bucket is the instant it cuts to; 0: UTC */
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
	SAA_X_AVG,
	SAA_O_COUNT_KEY				/* count(DISTINCT x) of a column the arms group by: the distinct ids of key `slot` in the cells of a group */
} SiftAggKind;

/* the number of kinds: spec.c keeps a row of facts for each (kind_info); not an enumerator, so that a switch over the kinds does not ask for it */
#define SAA_NKINDS	(SAA_O_COUNT_KEY + 1)

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
	const char *serial;			/* why no workers keep partial states for the leader to merge with the aggregate's combine function (through its serial and deserial functions when the state is internal); NULL: they can */
	bool		hyp;			/* hypothetical-set (ordered): the arguments are the sort columns and every row counts, NULLs included */
	bool		sorted;			/* ORDER BY in the call: the arguments of a group go through a tuplesort on the sort keys, then through the transition function */
	bool		first;			/* DISTINCT ON: only the first row of a group by the ORDER BY of the call goes through the transition function, kept as the rows pass (no sort) */
	int			hll;			/* sift.approx_count_distinct of a plain leaf: the leaf whose units' stored sketches the aggregate merges (agg/approx.c), else -1 */
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
	Bitmapset  *reads;			/* planner only, not packed: the leaves its arguments read */
	double		est_distinct;	/* planner only, not packed, SAA_M_DISTINCT and the ordered-set aggregates: the distinct values of the argument, as PostgreSQL estimates them */
	Oid			restype;		/* result type of the Aggref */
	Oid			argtype;		/* type of the leaf's SQL values (output type of min/max) */
	Oid			collid;			/* input collation: count(DISTINCT) of dictionary values */
	int			col;			/* SAA_O_*: the inner column it reads, -1 none */
	int			ncond;			/* SAA_O_*: the FILTER */
	SiftAggCond *cond;
	int			combo;			/* SAA_M_*: the combo and slot that give the ids of the values */
	int			slot;			/* ... SAA_O_COUNT_KEY: the key whose ids it counts */
	int			nref;			/* SAA_GROUPING: the keys of its arguments */
	int		   *ref;
	char	   *desc;
	double		frac;			/* SAA_PCT_*: the fraction (a constant in [0, 1]) */
	bool		desc_order;		/* SAA_PCT_*: ORDER BY ... DESC */
	int			share;			/* SAA_PCT_*: the aggregate whose sorted values this one reads (itself, or an earlier one on the same argument) */
	SiftAggGen *gen;			/* SAA_GEN */
	Oid			aggfn;			/* SAA_M_PG: the aggregate function */
	int			wsum;			/* SAA_X_SUM, SAA_X_AVG of a numeric leaf read as a dictionary (wide): the leaf + 1, whose entries the sum adds without the combo; 0 none */
} SiftAggAgg;

/*
 * The query is an outer aggregate over arms (a UNION ALL of subqueries, or one subquery) that are aggregates of the scanned rows
 * grouped by a unique row id and by outer keys: every inner group is one row's share of an outer group, so the executor keeps the
 * inner aggregates of the current row per outer group (row by row, as the rows of a unit follow each other), and adds them to the
 * outer aggregates when the row is done. SiftAggSpec.key are the outer keys, SiftAggSpec.agg the outer aggregates.
 */
typedef struct SiftAggVLeaf
{
	int			obj;			/* the object leaf (free keys: children "key" and "value"), the array leaf of a length, or the scalar leaf of a type */
	char	   *key;			/* the value of its element with this key, NULL when the row has none: leaf nleaves + index */
	bool		count;			/* the number of elements of the array leaf obj (jsonb_array_length), NULL where it has none; key is unused */
	bool		types;			/* the JSON type of the value of the scalar leaf obj (jsonb_typeof), as the name text, NULL where the path has no value; key is unused */
	uint8		recheck;		/* a member read only through the cast of its jsonb value (SiftSrcRecheck, SRK_NONE: not): a row whose member of this key holds a value the leaf cannot store is read again from the heap, and raises the error of the cast */
	Oid			cast;			/* ... to this type (float8, int4 or int8): the name of the type is in the message */
} SiftAggVLeaf;

/* a leaf of a member of an object by key: its element columns are its object's children "key" and "value" */
static inline bool
sift_aggvleaf_member(const SiftAggVLeaf *v)
{
	return !v->count && !v->types;
}

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

#define SIFT_AGG_MAX_ARMS	16

/*
 * The spine of a two level aggregate: the rows of a small plan (a VALUES list, a series, a table of buckets) that the arms are joined to
 * by equality of their keys. A cell is repeated for each spine row its join keys equal; the spine columns the outer query groups by are
 * keys of the scan (SAK_SPINE) that the spine rows give. A row of a preserved spine that no cell joins is a row of NULL arm columns.
 */
typedef struct SiftAggSpine
{
	bool		left;			/* the spine is the preserved side of a LEFT JOIN: a row no cell joined is a group of its own */
	int			child;			/* the plan of the rows: custom_plans[child] */
	int			ncol;			/* its output columns */
	int			npair;			/* the equalities of the join */
	struct SiftAggSpinePair
	{
		int			col;		/* the spine column; -1: a constant */
		Node	   *konst;
		bool		nullsafe;	/* NULL equals NULL */
		Oid			typid;
		Oid			collid;
		Oid			eqop;
		int			key[SIFT_AGG_MAX_ARMS];	/* per arm: the key of the scan its side is; -1: NULL in the arm */
	}			pair[SIFT_AGG_MAX_JKEYS];
	int			nout;			/* the spine columns that are keys of the scan */
	int			outcol[SIFT_AGG_MAX_KEYS];
	int			outkey[SIFT_AGG_MAX_KEYS];
} SiftAggSpine;

#endif							/* SIFT_AGG_SPEC_EXPR_H */
