/*
 * agg/v6/v6_pg_expr.c - PostgreSQL expressions over the vectors of a batch (agg/v6/v6_pg_expr.h).
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6_datum.h"
#include "agg/v6/v6_pg_expr.h"

typedef struct PlanOkCtx
{
	bool		ok;
} PlanOkCtx;

static bool
plan_ok_walker(Node *n, PlanOkCtx *c)
{
	if (n == NULL)
		return false;
	switch (nodeTag(n))
	{
		case T_Param:
		case T_SubPlan:
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

bool
sift_v6_pgexpr_plan_ok(const Expr *e)
{
	PlanOkCtx	c = {.ok = true};

	if (contain_volatile_functions((Node *) e) || expression_returns_set((Node *) e))
		return false;
	plan_ok_walker((Node *) e, &c);
	return c.ok;
}

typedef struct VarsCtx
{
	Index		relid;
	List	   *vars;
	bool		overflow;
} VarsCtx;

static bool
vars_walker(Node *n, VarsCtx *c)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if (v->varno != c->relid)
			return false;
		foreach_ptr(Var, seen, c->vars)
			if (seen->varattno == v->varattno)
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
sift_v6_pgexpr_vars(const Expr *e, Index relid, List **vars)
{
	VarsCtx		c = {.relid = relid};

	vars_walker((Node *) e, &c);
	*vars = c.vars;
	return !c.overflow;
}

typedef struct RenumberCtx
{
	Index		relid;
	const List *vars;
} RenumberCtx;

static Node *
renumber_mutator(Node *n, RenumberCtx *c)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((const Var *) n)->varno == c->relid)
	{
		Var		   *v = copyObject((Var *) n);

		for (int k = 0; k < list_length(c->vars); k++)
			if (((const Var *) list_nth(c->vars, k))->varattno == v->varattno)
			{
				v->varno = SIFT_V6_PGX_RELID;
				v->varnosyn = SIFT_V6_PGX_RELID;
				v->varattno = (AttrNumber) (k + 1);
				v->varattnosyn = v->varattno;
				return (Node *) v;
			}
		SIFT_INVARIANT(false, "a column of an expression of the aggregate scan is not among its inputs");
	}
	return expression_tree_mutator(n, renumber_mutator, c);
}

Expr *
sift_v6_pgexpr_renumber(const Expr *e, Index relid, const List *vars)
{
	RenumberCtx c = {relid, vars};

	return (Expr *) renumber_mutator((Node *) e, &c);
}

bool
sift_v6_pgexpr_derive_of(const SiftV6Resolver *rs, Index relid, Expr *e, SiftV6Derive *d, Oid *restype, List **items)
{
	List	   *vars;
	List	   *ints;
	List	   *item;
	SiftV6Col	col;
	int			at = 0;

	if (IsA(e, Var) || IsA(e, Const) || !sift_v6_pgexpr_plan_ok(e) || !sift_v6_pgexpr_vars(e, relid, &vars) || vars == NIL)
		return false;
	*restype = getBaseType(exprType((Node *) e));
	ints = list_make1_int((int) *restype);
	foreach_ptr(Var, v, vars)
	{
		if (!rs->column(rs->ctx, (Expr *) v, true, &col))
			return false;
		if (v == linitial(vars))
			d->src = col;
		ints = list_concat(ints, list_make3_int(sift_v6_col_input_attno(&col), (int) col.typid, col.scale));
	}
	item = list_make2(sift_v6_pgexpr_renumber(e, relid, vars), ints);
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
sift_v6_pgexpr_item(const SiftV6Spec *s, const SiftV6Derive *d, SiftV6PgItem *out)
{
	const List *item = list_nth(s->pgexpr, (int) d->aux);
	const List *ints = lsecond(item);

	out->expr = linitial(item);
	out->restype = (Oid) linitial_int(ints);
	out->nin = (list_length(ints) - 1) / 3;
	for (int i = 0; i < out->nin; i++)
		out->col[i] = (SiftV6Col) {.attno = list_nth_int(ints, 1 + 3 * i),.typid = (Oid) list_nth_int(ints, 2 + 3 * i),.scale = list_nth_int(ints, 3 + 3 * i)};
}

bool
sift_v6_spec_parallel_ok(PlannerInfo *root, const SiftV6Spec *s)
{
	if (s->constant_groups)
		return false;
	for (int i = 0; i < s->nkeys; i++)
		if (sift_v6_key_generic(s->key[i].typid))
			return false;
	foreach_ptr(List, item, s->pgexpr)
		if (!is_parallel_safe(root, linitial(item)))
			return false;
	foreach_ptr(Node, clause, s->vqual)
		if (!is_parallel_safe(root, clause))
			return false;
	return true;
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
sift_v6_pgexpr_open(MemoryContext cx, Expr *e, int nin, const SiftV6PgIn *in)
{
	SiftV6PgExpr *x = MemoryContextAllocZero(cx, sizeof(SiftV6PgExpr));
	MemoryContext old = MemoryContextSwitchTo(cx);
	TupleDesc	desc = CreateTemplateTupleDesc(Max(nin, 1));

	Assert(nin <= SIFT_V6_MAX_COLS);
	x->cx = cx;
	x->nin = nin;
	memcpy(x->in, in, nin * sizeof(SiftV6PgIn));
	for (int i = 0; i < nin; i++)
		TupleDescInitEntry(desc, (AttrNumber) (i + 1), NULL, in[i].typid, -1, 0);
	if (nin == 0)
		TupleDescInitEntry(desc, 1, NULL, INT4OID, -1, 0);
	sift_tupdesc_finalize(desc);
	x->slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	x->econtext = CreateStandaloneExprContext();
	x->econtext->ecxt_scantuple = x->slot;
	x->state = ExecInitExpr(e, NULL);
	MemoryContextSwitchTo(old);
	return x;
}

void
sift_v6_pgexpr_eval(SiftV6PgExpr *x, const SiftVec *const *in, const SiftBatchSel *bs)
{
	ExprContext *ec = x->econtext;
	TupleTableSlot *slot = x->slot;

	CHECK_FOR_INTERRUPTS();
	ResetExprContext(ec);
	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));
			MemoryContext old = MemoryContextSwitchTo(ec->ecxt_per_tuple_memory);

			ExecClearTuple(slot);
			for (int i = 0; i < x->nin; i++)
			{
				const SiftVec *v = in[i];
				const bool	valid = v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1);

				slot->tts_isnull[i] = !valid;
				slot->tts_values[i] = valid ? sift_v6_native_datum(v, r, x->in[i].typid, x->in[i].scale) : (Datum) 0;
			}
			ExecStoreVirtualTuple(slot);
			MemoryContextSwitchTo(old);
			x->vals[r] = ExecEvalExprSwitchContext(x->state, ec, &x->isnull[r]);
		}
}

void
sift_v6_pgexpr_derive(SiftV6PgExpr *x, const SiftVec *const *in, const SiftBatchSel *bs, SiftVxOut *out)
{
	const Oid	restype = x->restype;
	const bool	native = sift_v6_type_native(restype);
	bool		all_valid = true;
	MemoryContext old;

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
	.type = native ? sift_v6_result_vec_type(restype) : SVEC_DATUM,.typid = native ? InvalidOid : restype};
	if (!native)
		get_typlenbyval(restype, &out->vec.typlen, &out->vec.typbyval);
}

SiftV6PgExpr *
sift_v6_pgexpr_bind(MemoryContext cx, const SiftV6Spec *s, const SiftV6Derive *d, SiftV6BqCol col, void *arg, const SiftVec **in)
{
	SiftV6PgItem it;
	SiftV6PgIn	pin[SIFT_V6_MAX_COLS];
	SiftV6PgExpr *x;

	sift_v6_pgexpr_item(s, d, &it);
	for (int i = 0; i < it.nin; i++)
	{
		const SiftV6InCol c = {.attno = it.col[i].attno,.typid = it.col[i].typid,.scale = it.col[i].scale};

		in[i] = col(arg, &c);
		pin[i] = (SiftV6PgIn) {.typid = it.col[i].typid,.scale = it.col[i].scale};
	}
	x = sift_v6_pgexpr_open(cx, it.expr, it.nin, pin);
	x->restype = it.restype;
	return x;
}
