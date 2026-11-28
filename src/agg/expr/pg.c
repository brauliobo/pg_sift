/*
 * agg/expr/pg.c - PostgreSQL expressions over the vectors of a batch (agg/expr/pg.h).
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "miscadmin.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/safety.h"
#include "agg/datum.h"
#include "agg/pg_exec.h"
#include "agg/expr/pg.h"

typedef struct PlanOkCtx
{
	PlannerInfo *root;
	List	   *subparams;		/* the parameters the sub-plans met so far set for their own test */
	bool		bound;			/* $n is a constant the node binds before it opens the expression (sift_v6_bind_params) */
	bool		ok;
} PlanOkCtx;

static bool
exec_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubPlan) || IsA(n, AlternativeSubPlan) || (IsA(n, Param) && ((const Param *) n)->paramkind == PARAM_EXEC))
		return true;
	return expression_tree_walker(n, exec_walker, context);
}

bool
sift_v6_pgexpr_exec(const Node *e)
{
	return exec_walker((Node *) e, NULL);
}

bool
sift_v6_jsonb_number_cast(Oid funcid)
{
	return funcid == F_INT2_JSONB || funcid == F_INT4_JSONB || funcid == F_INT8_JSONB || funcid == F_FLOAT8_JSONB;
}

static bool
plan_ok_walker(Node *n, PlanOkCtx *c)
{
	if (n == NULL)
		return false;
	switch (nodeTag(n))
	{
		case T_Param:
			if (((const Param *) n)->paramkind == PARAM_EXTERN && (c->bound || c->root != NULL))
				return false;
			if (((const Param *) n)->paramkind != PARAM_EXEC ||
				(!list_member_int(c->subparams, ((const Param *) n)->paramid) && (c->root == NULL || !initplan_ok(c->root, (Param *) n))))
			{
				c->ok = false;
				return true;
			}
			return false;
		case T_SubPlan:
			if (c->root == NULL || !lut_subplan_ok(c->root, (SubPlan *) n))
			{
				c->ok = false;
				return true;
			}
			c->subparams = list_concat(c->subparams, ((const SubPlan *) n)->paramIds);
			return expression_tree_walker(n, plan_ok_walker, c);
		case T_AlternativeSubPlan:
		case T_SubLink:
		case T_Aggref:
		case T_GroupingFunc:
		case T_WindowFunc:
		case T_PlaceHolderVar:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
			c->ok = false;
			return true;
		case T_Var:
			if (((const Var *) n)->varlevelsup != 0 || ((const Var *) n)->varattno <= 0)
			{
				c->ok = false;
				return true;
			}
			return false;
		default:
			return expression_tree_walker(n, plan_ok_walker, c);
	}
}

static bool
plan_ok(PlannerInfo *root, const Expr *e, bool bound)
{
	PlanOkCtx	c = {.root = root,.bound = bound,.ok = true};

	if (contain_volatile_functions((Node *) e) || expression_returns_set((Node *) e))
		return false;
	plan_ok_walker((Node *) e, &c);
	return c.ok;
}

bool
sift_v6_pgexpr_plan_ok(PlannerInfo *root, const Expr *e)
{
	return plan_ok(root, e, false);
}

bool
sift_v6_pgexpr_plan_ok_bound(PlannerInfo *root, const Expr *e)
{
	return plan_ok(root, e, true);
}

typedef struct VarsCtx
{
	Index		relid;
	Index		elem_relid;
	List	   *vars;
	bool		overflow;
	bool		foreign;
} VarsCtx;

static bool
vars_walker(Node *n, VarsCtx *c)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if (v->varno != c->relid && (c->elem_relid == 0 || v->varno != c->elem_relid))
		{
			c->foreign |= v->varlevelsup == 0;
			return false;
		}
		foreach_ptr(Var, seen, c->vars)
			if (seen->varno == v->varno && seen->varattno == v->varattno)
				return false;
		if (list_length(c->vars) == SIFT_V6_MAX_COLS)
			c->overflow = true;
		else
			c->vars = lappend(c->vars, copyObject(v));
		return false;
	}
	return expression_tree_walker(n, vars_walker, c);
}

bool
sift_v6_pgexpr_vars(const Expr *e, Index relid, Index elem_relid, List **vars)
{
	VarsCtx		c = {.relid = relid,.elem_relid = elem_relid};

	vars_walker((Node *) e, &c);
	*vars = c.vars;
	return !c.overflow && !c.foreign;
}

typedef struct RenumberCtx
{
	Index		relid;
	Index		elem_relid;
	const List *vars;
} RenumberCtx;

static Node *
renumber_mutator(Node *n, RenumberCtx *c)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && (((const Var *) n)->varno == c->relid || (c->elem_relid != 0 && ((const Var *) n)->varno == c->elem_relid)))
	{
		Var		   *v = copyObject((Var *) n);

		for (int k = 0; k < list_length(c->vars); k++)
			if (((const Var *) list_nth(c->vars, k))->varno == v->varno && ((const Var *) list_nth(c->vars, k))->varattno == v->varattno)
			{
				v->varno = SIFT_V6_PGX_RELID;
				v->varnosyn = SIFT_V6_PGX_RELID;
				v->varattno = (AttrNumber) (k + 1);
				v->varattnosyn = v->varattno;
				v->vartype = getBaseType(v->vartype);	/* the scan hands a column of a domain over as its base type */
				return (Node *) v;
			}
		SIFT_INVARIANT(false, "a column of an expression of the aggregate scan is not among its inputs");
	}
	return expression_tree_mutator(n, renumber_mutator, c);
}

Expr *
sift_v6_pgexpr_renumber(const Expr *e, Index relid, Index elem_relid, const List *vars)
{
	RenumberCtx c = {relid, elem_relid, vars};

	return (Expr *) renumber_mutator((Node *) e, &c);
}

bool
sift_v6_pgexpr_derive_of(PlannerInfo *root, const SiftV6Resolver *rs, Index relid, Expr *e, bool needs_column, SiftV6Derive *d, Oid *restype, List **items)
{
	List	   *vars;
	List	   *ints;
	List	   *item;
	SiftV6Col	col;
	int			at = 0;

	if (IsA(e, Var) || !sift_v6_pgexpr_plan_ok(root, e) || !sift_v6_pgexpr_vars(e, relid, rs->elem_relid, &vars) || (vars == NIL && needs_column))
		return false;
	*restype = getBaseType(exprType((Node *) e));
	ints = list_make1_int((int) *restype);
	foreach_ptr(Var, v, vars)
	{
		if (!rs->column(rs->ctx, (Expr *) v, true, &col))
			return false;
		if (v == linitial(vars))
			d->src = col;
		ints = list_concat(ints, list_make5_int(sift_v6_col_input_attno(&col), (int) col.typid, col.scale, (int) col.src, col.slot));
	}
	item = list_make2(sift_v6_pgexpr_renumber(e, relid, rs->elem_relid, vars), ints);
	foreach_ptr(List, seen, *items)
	{
		if (equal(seen, item))
			break;
		at++;
	}
	if (at == list_length(*items))
		*items = lappend(*items, item);
	d->kind = SVD_PG_EXPR;
	d->aux = at;
	return true;
}

void
sift_v6_pgexpr_item_at(const SiftV6Spec *s, int at, SiftV6PgItem *out)
{
	const List *item = list_nth(s->pgexpr, at);
	const List *ints = lsecond(item);

	out->expr = linitial(item);
	out->restype = (Oid) linitial_int(ints);
	out->nin = (list_length(ints) - 1) / SIFT_V6_PGX_INTS;
	for (int i = 0; i < out->nin; i++)
	{
		const int	at = 1 + SIFT_V6_PGX_INTS * i;

		out->col[i] = (SiftV6Col) {.attno = list_nth_int(ints, at),.typid = (Oid) list_nth_int(ints, at + 1),.scale = list_nth_int(ints, at + 2),
		.src = (SiftV6Src) list_nth_int(ints, at + 3),.slot = list_nth_int(ints, at + 4)};
	}
}

SiftVecType
sift_v6_pgexpr_vec_type(const SiftV6Spec *s, const SiftV6Derive *d)
{
	SiftV6PgItem it;

	sift_v6_pgexpr_item(s, d, &it);
	return sift_v6_type_native(it.restype) ? sift_v6_result_vec_type(it.restype) : SVEC_DATUM;
}

void
sift_v6_pgexpr_item(const SiftV6Spec *s, const SiftV6Derive *d, SiftV6PgItem *out)
{
	sift_v6_pgexpr_item_at(s, (int) d->aux, out);
}

bool
sift_v6_pgexpr_sourceless(const SiftV6Spec *s, const SiftV6Derive *d)
{
	SiftV6PgItem it;

	if (d->kind != SVD_PG_EXPR)
		return false;
	sift_v6_pgexpr_item(s, d, &it);
	return it.nin == 0;
}

/* A parameter only the leader holds: the output of an initplan, a PARAM_EXEC that no sub-plan of the expression sets for its own test (a sub-plan itself runs in each process) */
static bool
leader_param_walker(Node *n, List **own)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubPlan))
		*own = list_concat(*own, ((const SubPlan *) n)->paramIds);
	if (IsA(n, Param))
		return ((const Param *) n)->paramkind == PARAM_EXEC && !list_member_int(*own, ((const Param *) n)->paramid);
	return expression_tree_walker(n, leader_param_walker, own);
}

static bool
leader_only(PlannerInfo *root, Node *e)
{
	List	   *own = NIL;

	return leader_param_walker(e, &own) || !is_parallel_safe(root, e);
}

const char *
sift_v6_spec_serial_why(PlannerInfo *root, const SiftV6Spec *s)
{
	if (s->constant_groups)
		return "its group keys read no column: the one group is a row of the process that read some";
	for (int j = 0; j < s->naggs; j++)
		if (s->agg[j].kind == SV6_PCT_CONT)
			return "percentile_cont keeps the values of a group in the memory of the process that read them";
	for (int i = 0; i < s->nkeys; i++)
		if (sift_v6_key_generic(s, &s->key[i]))
			return "a group key of a type the scan does not group by its bytes is held by codes of the process that read it";
	foreach_ptr(List, item, s->pgexpr)
		if (leader_only(root, linitial(item)))
			return "an expression it computes reads a value the leader sets or runs a function that is not parallel safe";
	foreach_ptr(Node, clause, s->vqual)
		if (leader_only(root, clause))
			return "a condition it tests reads a value the leader sets or runs a function that is not parallel safe";
	return NULL;
}

SiftVecType
sift_v6_derive_vec_type(const SiftV6Spec *s, const SiftV6Derive *d)
{
	switch (d->kind)
	{
		case SVD_PG_EXPR:
			return sift_v6_pgexpr_vec_type(s, d);
		case SVD_URL_HOST:
		case SVD_CASE_STR:
			return SVEC_STRVIEW;
		default:
			return SVEC_I64;
	}
}

bool
sift_v6_derive_vec_typed(const SiftV6Derive *d)
{
	return d->kind != SVD_NUM_EXPR && d->kind != SVD_ARITH;
}

bool
sift_v6_spec_pg_reads(const SiftV6Spec *s)
{
	return s->pgexpr != NIL || s->vqual != NIL;
}

int
sift_v6_spec_pg_inputs(const SiftV6Spec *s)
{
	int			n = 0;

	for (int k = 0; k < s->nderive; k++)
		if (s->derive[k].kind == SVD_PG_EXPR)
		{
			SiftV6PgItem it;

			sift_v6_pgexpr_item(s, &s->derive[k], &it);
			n += it.nin;
		}
	return n;
}

SiftV6PgExpr *
sift_v6_pgexpr_open(MemoryContext cx, PlanState *ps, Expr *e, int nin, const SiftV6PgIn *in)
{
	SiftV6PgExpr *x = MemoryContextAllocZero(cx, sizeof(SiftV6PgExpr));
	MemoryContext old = MemoryContextSwitchTo(cx);
	TupleDesc	desc = CreateTemplateTupleDesc(Max(nin, 1));

	Assert(nin <= SIFT_V6_MAX_COLS);
	x->cx = cx;
	x->nin = nin;
	memcpy(x->in, in, nin * sizeof(SiftV6PgIn));
	for (int i = 0; i < nin; i++)
	{
		x->in[i].typid = getBaseType(in[i].typid);
		TupleDescInitEntry(desc, (AttrNumber) (i + 1), NULL, x->in[i].typid, -1, 0);
	}
	if (nin == 0)
		TupleDescInitEntry(desc, 1, NULL, INT4OID, -1, 0);
	sift_tupdesc_finalize(desc);
	x->slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	x->econtext = ps != NULL ? CreateExprContext(ps->state) : CreateStandaloneExprContext();
	x->econtext->ecxt_scantuple = x->slot;
	x->state = ExecInitExpr(e, ps);
	if (nin == 1 && (x->in[0].typid == TIMESTAMPOID || x->in[0].typid == TIMESTAMPTZOID))
	{
		const Var  *col;

		sift_v6_step_open(cx, (const Node *) e, SIFT_V6_PGX_RELID, &x->step, &col);
	}
	MemoryContextSwitchTo(old);
	return x;
}

static void
eval_rows(SiftV6PgExpr *x, const SiftVec *const *in, const SiftBatchSel *bs)
{
	ExprContext *ec = x->econtext;
	TupleTableSlot *slot = x->slot;

	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));
			MemoryContext old = MemoryContextSwitchTo(ec->ecxt_per_tuple_memory);

			ExecClearTuple(slot);
			for (int i = 0; i < x->nin; i++)
			{
				slot->tts_values[i] = sift_v6_vec_datum(in[i], r, x->in[i].typid, x->in[i].scale, &slot->tts_isnull[i]);
			}
			ExecStoreVirtualTuple(slot);
			MemoryContextSwitchTo(old);
			x->vals[r] = ExecEvalExprSwitchContext(x->state, ec, &x->isnull[r]);
		}
}

void
sift_v6_pgexpr_eval(SiftV6PgExpr *x, const SiftVec *const *in, const SiftBatchSel *bs)
{
	SiftBatchSel heads;
	uint16		head_of[SIFT_VEC_LEN];

	CHECK_FOR_INTERRUPTS();
	ResetExprContext(x->econtext);
	if (x->step.us == 0)
	{
		eval_rows(x, in, bs);
		return;
	}
	sift_v6_step_runs(&x->step, in[0], bs, &heads, head_of);
	eval_rows(x, in, &heads);
	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w] & ~heads.mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));

			x->vals[r] = x->vals[head_of[r]];
			x->isnull[r] = x->isnull[head_of[r]];
		}
}

static void
resolve_result(SiftV6PgExpr *x)
{
	x->res_of = x->restype;
	x->res_native = sift_v6_type_native(x->restype);
	x->res_typlen = 0;
	x->res_typbyval = false;
	if (!x->res_native)
		get_typlenbyval(x->restype, &x->res_typlen, &x->res_typbyval);
}

void
sift_v6_pgexpr_derive(SiftV6PgExpr *x, const SiftVec *const *in, const SiftBatchSel *bs, SiftVxOut *out)
{
	const Oid	restype = x->restype;
	bool		native,
				all_valid = true;
	MemoryContext old;

	if (x->res_of != restype)
		resolve_result(x);
	native = x->res_native;
	sift_v6_pgexpr_eval(x, in, bs);
	old = MemoryContextSwitchTo(x->econtext->ecxt_per_tuple_memory);
	memset(out->valid, 0, sizeof(out->valid));
	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));

			all_valid &= !x->isnull[r];
			if (x->isnull[r])
				continue;
			out->valid[r >> 6] |= UINT64CONST(1) << (r & 63);
			if (native)
				sift_v6_result_put(restype, out->data, r, x->vals[r]);
			else
				((Datum *) out->data)[r] = x->vals[r];
		}
	MemoryContextSwitchTo(old);
	out->vec = (SiftVec) {.data = out->data,.valid = out->valid,.len = bs->n_raw,.all_valid = all_valid,.dense = true,
	.type = native ? sift_v6_result_vec_type(restype) : SVEC_DATUM,.typid = native ? InvalidOid : restype,.typlen = x->res_typlen,.typbyval = x->res_typbyval};
}

SiftV6PgExpr *
sift_v6_pgexpr_bind(MemoryContext cx, const SiftV6Spec *s, const SiftV6Derive *d, SiftV6BqCol col, void *arg, const SiftVec **in)
{
	SiftV6PgItem it;
	SiftV6PgIn	pin[SIFT_V6_MAX_COLS];

	sift_v6_pgexpr_item(s, d, &it);
	for (int i = 0; i < it.nin; i++)
	{
		const SiftV6InCol c = {.attno = it.col[i].attno,.typid = it.col[i].typid,.scale = it.col[i].scale};

		in[i] = col(arg, &c);
		pin[i] = (SiftV6PgIn) {.typid = it.col[i].typid,.scale = it.col[i].scale};
	}
	return sift_v6_pgexpr_of(cx, s, d, &it, pin);
}

SiftV6PgExpr *
sift_v6_pgexpr_of(MemoryContext cx, const SiftV6Spec *s, const SiftV6Derive *d, const SiftV6PgItem *it, const SiftV6PgIn *pin)
{
	SiftV6PgExpr *x;

	if (s->exec != NULL && s->exec->item[d->aux] != NULL)
		return s->exec->item[d->aux];
	x = sift_v6_pgexpr_open(cx, NULL, it->expr, it->nin, pin);
	x->restype = it->restype;
	return x;
}
