/*
 * agg/expr/pg.h - SVD_PG_EXPR: a key or an argument of the scan that is a PostgreSQL expression over columns the scan decodes (lower(x), CASE, COALESCE, x % 7, date_trunc('month', d), substr, a SQL function,
 * any operator), evaluated by the executor's own machinery (ExecInitExpr, ExecEvalExpr) for the active rows of a batch, after the native predicates have narrowed it.
 *
 * One reusable virtual slot (TTSOpsVirtual) takes the columns of the row, one dedicated ExprContext takes everything the evaluation allocates: its per-tuple memory is reset once for each batch of
 * SIFT_VEC_LEN rows, not for each row, so that the Datums of the batch (a text result a string view points into) stay valid until the batch is folded and nothing leaks past it. The expression
 * is over Vars of varno SIFT_V6_PGX_RELID whose varattno k is the k-th input (1-based).
 */
#ifndef SIFT_AGG_V6_PG_EXPR_H
#define SIFT_AGG_V6_PG_EXPR_H

#include "postgres.h"

#include "executor/executor.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

#include "agg/qual/bqual.h"
#include "agg/expr/str.h"
#include "agg/plan/rewrite.h"

#define SIFT_V6_PGX_RELID		1

/* An input of the expression: the PostgreSQL type of its column and, for a numeric the scan holds as scaled integers, the digits after the point */
typedef struct SiftV6PgIn
{
	Oid			typid;
	int			scale;
} SiftV6PgIn;

typedef struct SiftV6PgExpr
{
	MemoryContext cx;
	Oid			restype;		/* the base type of the result of a derivation (sift_v6_pgexpr_bind) */
	Oid			res_of;			/* the restype the next three were resolved for, the first time a derivation ran (the length of a type no native vector holds) */
	bool		res_native;
	int16		res_typlen;
	bool		res_typbyval;
	ExprState  *state;
	ExprContext *econtext;
	TupleTableSlot *slot;
	int			nin;
	SiftV6PgIn	in[SIFT_V6_MAX_COLS];
	Datum		vals[SIFT_VEC_LEN];
	bool		isnull[SIFT_VEC_LEN];
} SiftV6PgExpr;

/*
 * The expression can run over the native vectors: no volatile function, no set-returning function, no aggregate or window, and only the Vars of the inputs. A sub-plan or an executor parameter it holds
 * is one the scan may start when a row first needs it (lut_subplan_ok, initplan_ok of agg/safety.h), and only when root is given: the scan of a node that prepares them (sift_v6_exec_begin)
 */
extern bool sift_v6_pgexpr_plan_ok(PlannerInfo *root, const Expr *e);
/* the same for a batch qual, whose $n the node binds as constants before it opens it (sift_v6_bind_params) */
extern bool sift_v6_pgexpr_plan_ok_bound(PlannerInfo *root, const Expr *e);

/* The expression starts a sub-plan or reads an executor parameter: only the node that runs the scan can evaluate it */
extern bool sift_v6_pgexpr_exec(const Node *e);

/* The expression over the inputs in[0 .. nin - 1], in cx (the state lives as long as cx). With a node (ps), it is that node's own: its sub-plans are the node's, and cx has to live as long as the node */
extern SiftV6PgExpr *sift_v6_pgexpr_open(MemoryContext cx, PlanState *ps, Expr *e, int nin, const SiftV6PgIn *in);

/* vals[r] and isnull[r] of every active row of bs; in[i] is the vector of input i. The Datums are valid until the next call */
extern void sift_v6_pgexpr_eval(SiftV6PgExpr *x, const SiftVec *const *in, const SiftBatchSel *bs);

/* The same, as the vector of the expression's result: native when its type has a native vector, else a SVEC_DATUM */
extern void sift_v6_pgexpr_derive(SiftV6PgExpr *x, const SiftVec *const *in, const SiftBatchSel *bs, SiftVxOut *out);

/* The evaluator of the derivation d of a spec: col gives the vector of each column it reads (the scan decodes them), put in in[0 ..] */
extern SiftV6PgExpr *sift_v6_pgexpr_bind(MemoryContext cx, const SiftV6Spec *s, const SiftV6Derive *d, SiftV6BqCol col, void *arg, const SiftVec **in);

/*
 * The derivation of a key or an argument the scan evaluates with these: e is an expression (not a column) of at most SIFT_V6_MAX_COLS columns of the range table entry relid, each one a
 * column rs resolves, and PostgreSQL can run it as it is (sift_v6_pgexpr_plan_ok of root). d is SVD_PG_EXPR with the first column as its source and the place of its item in *items as aux, *restype the base type of
 * the result. An item is the renumbered expression and the ints (result type, then attno, type and scale of each column).
 */
extern bool sift_v6_pgexpr_derive_of(PlannerInfo *root, const SiftV6Resolver *rs, Index relid, Expr *e, bool needs_column, SiftV6Derive *d, Oid *restype, List **items);

typedef struct SiftV6PgItem
{
	Expr	   *expr;
	Oid			restype;
	int			nin;
	SiftV6Col	col[SIFT_V6_MAX_COLS];
} SiftV6PgItem;

/* The derivation d reads no column of the scan: nothing to decode for it */
extern bool sift_v6_pgexpr_sourceless(const SiftV6Spec *s, const SiftV6Derive *d);
/* The item of the derivation d (SVD_PG_EXPR) of a spec, or the at-th of its items, and the type of the vector its result is in */
extern void sift_v6_pgexpr_item(const SiftV6Spec *s, const SiftV6Derive *d, SiftV6PgItem *out);
extern SiftVecType sift_v6_pgexpr_vec_type(const SiftV6Spec *s, const SiftV6Derive *d);
/* The type of the vector a derivation d of the spec gives, for those whose vector has one type for the whole scan (SVD_NUM_EXPR's is a NUM64 or a NUM128 by the batch, SVD_ARITH's an unnest's) */
extern SiftVecType sift_v6_derive_vec_type(const SiftV6Spec *s, const SiftV6Derive *d);
extern bool sift_v6_derive_vec_typed(const SiftV6Derive *d);
extern void sift_v6_pgexpr_item_at(const SiftV6Spec *s, int at, SiftV6PgItem *out);
/* The workers of a parallel scan can run the statement: its PostgreSQL expressions are parallel safe, and its groups are bytes the leader merges (a generic key's codes are the state of one backend, a constant group is a row of the one that read some, a percentile_cont's values are offsets into the arena of one backend) */
extern bool sift_v6_spec_parallel_ok(PlannerInfo *root, const SiftV6Spec *s);
/* The spec has derivations or batch quals PostgreSQL evaluates: the text columns they read are given as strings, never as the codes of a dictionary */
extern bool sift_v6_spec_pg_reads(const SiftV6Spec *s);
/* The columns the derivations of the spec read, the room the scan's column array needs for them */
extern int	sift_v6_spec_pg_inputs(const SiftV6Spec *s);

/* The Vars of e of the range table entries relid and elem_relid (0: none), each once in the order met: their attnos and types; false when there are more than SIFT_V6_MAX_COLS or e names a column of another range table entry */
extern bool sift_v6_pgexpr_vars(const Expr *e, Index relid, Index elem_relid, List **vars);
/* A copy of e whose Vars of relid and elem_relid are the Vars of SIFT_V6_PGX_RELID numbered by their place in vars (as sift_v6_pgexpr_vars found them) */
extern Expr *sift_v6_pgexpr_renumber(const Expr *e, Index relid, Index elem_relid, const List *vars);

#endif							/* SIFT_AGG_V6_PG_EXPR_H */
