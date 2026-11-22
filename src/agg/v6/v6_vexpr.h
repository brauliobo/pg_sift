/*
 * agg/v6/v6_vexpr.h - what the operators of SiftAggV6 (v6_topn.h, v6_window.h, v6_stream.h, v6_sip.h, v6_card.h) share: the format they run over, the columns of the index a query reads,
 * the batches a scan hands them, and the compiler of the expressions of the query (agg/vexpr.h) from the Expr trees PostgreSQL planned.
 *
 * The format (SiftV6Format) is what the index of format major 2 tells the planner and gives the executor: whether it owns an index, holds a leaf in a column, is sorted by columns, and
 * the source of batches it opens over a snapshot; it is bound once at load (sift_v6_format_bind) and the operators know nothing else of the files.
 *
 * An expression is compiled twice: when the path is made, to know that the machine runs it (parameters stand for typed NULLs), and when the node starts or is scanned again, with the
 * parameters of that execution (a generic plan keeps its Param nodes; no value is ever kept from the plan) and the zone of the session at that time (a plan outlives a SET TIME ZONE).
 */
#ifndef SIFT_AGG_V6_VEXPR_H
#define SIFT_AGG_V6_VEXPR_H

#include "postgres.h"

#include "commands/explain.h"
#include "nodes/execnodes.h"
#include "access/genam.h"
#include "nodes/params.h"
#include "utils/tuplestore.h"
#include "nodes/pathnodes.h"

#include "agg/plan/card_est.h"
#include "agg/vexpr.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_inset.h"
#include "agg/zoneoff.h"

#define SIFT_V6_MAX_COLS	SIFT_VEXPR_MAX_REGS
#define SIFT_V6_MAX_MASKS	SIFT_VEXPR_MAX_MASKS

/* A column the scan decodes for the operator: the attribute of the index and how it decodes */
typedef struct SiftV6InCol
{
	AttrNumber	attno;			/* of the index, 1-based, as SiftV6Col.attno */
	Oid			typid;
	SiftVecType type;
	int16		scale;
	uint8		prec;
} SiftV6InCol;

/* A boolean the scan decodes as a bitmap: a boolean column, or the presence of a key in a jsonb column */
typedef struct SiftV6InMask
{
	AttrNumber	attno;
	char	   *key;			/* NULL: the boolean column itself */
} SiftV6InMask;

/* The columns and booleans of one scan, in the order the batches give them */
typedef struct SiftV6Input
{
	Oid			indexoid;		/* the table when is_table: its attributes are the columns, as the attributes of an index are */
	bool		is_table;
	int			nkeys;			/* the columns of the index: the attribute of the table each holds, 0 for an expression; the attributes of a table (keys is not used) */
	AttrNumber	keys[INDEX_MAX_KEYS];
	int			ncols;
	SiftV6InCol col[SIFT_V6_MAX_COLS];
	int			nmasks;
	SiftV6InMask mask[SIFT_V6_MAX_MASKS];
	EState	   *estate;			/* the execution the program runs in (the values of PARAM_EXEC): NULL while a path is made */
} SiftV6Input;

/* One batch of a scan: rows the visibility of the heap and the deletions of the index leave, the columns of the input as vectors (col[i] is input column i), the booleans as bitmaps */
typedef struct SiftV6Batch
{
	uint16		n;
	const SiftVec *col;
	const SiftVexprMaskIn *masks;
	const uint64 *mask;			/* the rows taken, NULL: all n */
	const uint64 *tag;			/* a number per row that names it to fetch (a row id of the source), NULL when the operator reads no other column */
} SiftV6Batch;

typedef struct SiftV6Source SiftV6Source;
typedef struct SiftV6Vexpr SiftV6Vexpr;

struct SiftV6Source
{
	/* the next batch, valid until the next call; false at the end. The rows come in the order of the parts when the operator asked for a sorted scan */
	bool		(*next) (SiftV6Source *s, SiftV6Batch *out);
	/* the rows of the partition of this key that have not been given are not wanted (NULL: the source cannot pass over them) */
	void		(*skip) (SiftV6Source *s, const int64 *part_key, uint8 part_nulls);
	/* the columns attnos[0 .. n-1] of the table for the row of the tag, as the Datums of their types */
	bool		(*fetch) (SiftV6Source *s, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull);
	void		(*close) (SiftV6Source *s);
};

typedef struct SiftV6Format
{
	/* the relation is a sift_table (is_table) or an index of the format; the oid of the other calls is one it owns */
	bool		(*owns_rel) (Oid relid, bool is_table);
	/* the relation holds attribute att, of the class, in one leaf the scan decodes as it is stored (SV6C_NONE: a jsonb document read for the presence of a key) */
	bool		(*leaf_holds) (Oid relid, int att, SiftV6Class cls);
	/* the parts of the relation are sorted by the attributes atts[0 .. n-1], each ascending, NULLs last */
	bool		(*sorted_by) (Oid relid, const int *atts, int n);
	/* the statistics the index folded when its parts were written (agg/plan/card_est.h), NULL when it has none */
	const SiftCardStatsView *(*card_view) (Oid relid);
	/* the batches of the input over the snapshot of the estate: only the rows that pass quals (NULL: all), in the order of the index columns (ascending, NULLs last) when sorted is set */
	SiftV6Source *(*open) (EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted);
} SiftV6Format;

extern void sift_v6_format_bind(const SiftV6Format *fmt);
extern const SiftV6Format *sift_v6_format(void);
/* The operators that read through the format (the joins among them) are offered only while one is bound */
extern bool sift_v6_format_bound(void);

#define SIFT_V6_MAX_BOUNDS	8

/* A conjunct of the quals that bounds an input column of the integer class (an int, a date, a timestamp) to a closed range: a source may leave unread what lies wholly outside it; a row it keeps is still tested by the program */
typedef struct SiftV6Bound
{
	int			slot;
	bool		has_lo;
	bool		has_hi;
	int64		lo;
	int64		hi;
} SiftV6Bound;

#define SIFT_V6_MAX_INSETS	4

/* A compiled expression with the columns and booleans it reads (slots of the input it was compiled against) */
struct SiftV6Vexpr
{
	SiftVexprProg prog;
	ZoneOff    *zone;			/* the zone of a timestamptz the program reads: the scan covers the instants of a chunk before it evaluates (zone_cover) */
	int			nbounds;
	SiftV6Bound bound[SIFT_V6_MAX_BOUNDS];
	int			ninsets;
	SiftV6InSet inset[SIFT_V6_MAX_INSETS];
};

/* The decoding of a column of the type of the Var: false for a type the engine does not decode (SiftV6Class SV6C_NONE) or a numeric without a precision of at most 38 digits */
extern bool sift_v6_vec_type(Oid typid, int32 typmod, SiftV6InCol *out);

/* Starts an input over the index, as the planner sees it and as the executor opened it */
extern void sift_v6_input_init_info(SiftV6Input *in, const IndexOptInfo *ix);
extern void sift_v6_input_init_rel(SiftV6Input *in, Relation index);
/* Starts an input over a sift_table, whose columns are its attributes */
extern void sift_v6_input_init_table(SiftV6Input *in, Relation tbl);
/* the program reads the PARAM_EXEC values of estate when it is compiled: a correlated scan compiles again when it is rescanned */
extern void sift_v6_input_bind(SiftV6Input *in, EState *estate);
/* Adds the column of the index that holds the table column of v (its slot, -1 when no column of the index does, the type does not decode, or the format does not hold it) */
extern int	sift_v6_input_add_var(SiftV6Input *in, const Var *v);

/* A text column of the index as bytes (SVEC_STRVIEW), for the operators that group or order by it without computing on it (the partition sort); -1 for a column of any other type */
extern int	sift_v6_input_add_text(SiftV6Input *in, const Var *v);

/*
 * The conjunct as an in-set when it is `column = ANY (array)` with a constant or an external parameter array over an int, a date or a timestamp column (the elements may be of any integer type),
 * the values of this execution (none while the path is made, which no row passes): the set goes to out, the column to the input; false for any other conjunct. keep: where the values live.
 */
extern bool sift_v6_inset_of(MemoryContext keep, const Node *clause, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6InSet *out);

/* The integers of a constant or parameter array of the integer class for a column of vartype, ascending, NULL elements left out (in keep); the empty set while the path is made (no parameters); false for any other array */
extern bool sift_v6_array_ints(MemoryContext keep, const Node *arr, Oid vartype, ParamListInfo params, const int64 **vals, int *n);

/* The SiftCmpOp of the operator name (= <> < <= > >=); false for any other */
extern bool sift_v6_cmp_op_of(const char *name, int *op);

/* The value this execution gives the parameter; false while the path is made (no parameters) or when it is not of the type the plan was made for */
extern bool sift_v6_param_value(ParamListInfo params, const Param *p, Datum *value, bool *isnull);

/* Adds to bound[0 .. *n - 1] the bounds a conjunct gives (at most SIFT_V6_MAX_BOUNDS), the columns it reads added to in */
extern void sift_v6_bounds_of(const Node *clause, Index relid, ParamListInfo params, SiftV6Input *in, int *n, SiftV6Bound *bound);

/*
 * e (an Expr, or a RestrictInfo) over the columns of relid, read through the index of in, to a program; the columns it needs are added to in. params: the values of this execution, NULL where the
 * path is being made. The program of a boolean expression is one that filters (prog.out_is_bool). false: the machine does not run it.
 */
extern bool sift_v6_vexpr_compile(MemoryContext cxt, const Node *e, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6Vexpr *out);
/* the conjunction of the clauses (an empty list: no program, true) */
extern bool sift_v6_vexpr_quals(MemoryContext cxt, const List *clauses, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6Vexpr *out);

/* The rows of the batch that pass quals (NULL: the batch's own), written to out as a bitmap of the batch rows; returns how many */
extern uint16 sift_v6_vexpr_filter(SiftVexprCtx *ctx, const SiftV6Vexpr *quals, const SiftV6Batch *b, uint64 *out);

/*
 * The tuples of an operator that computes everything when it is first asked (a window, a stream of groups): kept in a tuplestore, which spills past work_mem and is read again by a scan again
 * with the same parameters. start before the engine runs; put for each tuple it emits; the node's slot must be of the minimal tuple kind (css.slotOps = &TTSOpsMinimalTuple).
 */
typedef struct SiftV6Store
{
	MemoryContext cx;			/* everything of one run of the engine */
	MemoryContext row_cx;		/* what one tuple takes to be built, reset by put */
	Tuplestorestate *store;
	bool		ran;
} SiftV6Store;

extern void sift_v6_store_start(SiftV6Store *s, EState *estate);
extern void sift_v6_store_put(SiftV6Store *s, TupleDesc desc, const Datum *values, const bool *isnull);
extern TupleTableSlot *sift_v6_store_get(SiftV6Store *s, TupleTableSlot *slot);
/* params_changed: the tuples are another run's; else the same ones are read from their start */
extern void sift_v6_store_rescan(SiftV6Store *s, bool params_changed);
extern void sift_v6_store_end(SiftV6Store *s);

/* EXPLAIN lines of an operator: the index it reads and a count it kept */
extern void sift_v6_explain_index(ExplainState *es, Oid indexoid);
extern void sift_v6_explain_count(ExplainState *es, const char *label, int64 value);

/* The plan of an operator of the bridge over a table's index: an index owned by the format that holds every column; false when the relation is not a plain table read as a base relation */
extern bool sift_v6_base_rel_ok(PlannerInfo *root, RelOptInfo *rel);

/* The planner keeps the target of an upper stage in root->upper_targets and sets it on the stage's rel only after the hooks ran: the operators read and add their paths with it */
extern void sift_v6_stage_target(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *output_rel);

/* A path of the operator: the cost, the order it keeps, the private list the plan callback reads; added to output_rel */
extern void sift_v6_add_path(RelOptInfo *output_rel, const CustomPathMethods *methods, double startup, double total, double rows, List *pathkeys, List *custom_private);

/* The CustomScan of a path: tlist over the Vars of custom_private[1] (the scan tlist), the relation of the index recorded for the plan cache */
extern Plan *sift_v6_plan_scan(PlannerInfo *root, CustomPath *best, List *tlist, const CustomScanMethods *methods, Oid indexoid);

/* The Vars an expression list reads of relid as the scan tlist (without duplicates), and the nodes of tag extra (T_Aggref, T_WindowFunc: a node of the operator), NIL when it reads anything else */
extern List *sift_v6_scan_tlist(const List *exprs, Index relid, NodeTag extra);

/* A numeric of v * 10^-scale, as PostgreSQL makes of the digits: a sum or an average that leaves the machine as a 128-bit integer */
extern Datum sift_v6_numeric_from_i128(SiftI128 v, int scale);

#endif							/* SIFT_AGG_V6_VEXPR_H */
