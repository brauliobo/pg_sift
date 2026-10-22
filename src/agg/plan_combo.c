/*
 * agg/plan_combo.c - lookup joins and combos: expressions PostgreSQL evaluates over value combinations
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

JoinCtx *
join_of_var(AggCtx *c, Node *n)
{
	Var		   *v;

	if (!IsA(n, Var))
		return NULL;
	v = (Var *) n;
	if (v->varlevelsup == 0 && v->varattno > 0)
		foreach_ptr(JoinCtx, jc, c->joins)
			if (jc->rti == v->varno)
			return jc;
	return NULL;
}

/* a column of a joined plan the spec reads (keys and columns): the path outputs it */
static void
join_var(JoinCtx *jc, Var *v)
{
	foreach_ptr(Var, x, jc->cvars)
		if (x->varattno == v->varattno)
			return;
	jc->cvars = lappend(jc->cvars, copyObject(v));
}

/* the index of column v of the joined table among the columns the expressions read; -1 when there are too many */
static int
join_col(JoinCtx *jc, Var *v)
{
	SiftAggJoin *j = &jc->j;

	join_var(jc, v);
	for (int i = 0; i < j->ncol; i++)
		if (j->colatt[i] == v->varattno)
			return i;
	if (j->ncol == SIFT_AGG_MAX_JCOLS)
		return -1;
	j->colatt[j->ncol] = v->varattno;
	j->coltype[j->ncol] = v->vartype;
	j->coltypmod[j->ncol] = v->vartypmod;
	j->colcoll[j->ncol] = v->varcollid;
	return j->ncol++;
}

/* the position of leaf `id` in the combo (added if new), its type fixed by the first reference that has one; -1 when it does not fit */
int
combo_leaf(ComboCtx *cb, int id, bool typed, Oid ptype, int32 ptypmod, Oid pcoll)
{
	int			p = 0;

	while (p < cb->nl && cb->leaf[p] != id)
		p++;
	if (p == cb->nl)
	{
		if (p == SIFT_AGG_MAX_CLEAVES)
			return -1;
		cb->leaf[cb->nl++] = id;
		cb->ptype[p] = ptype;
		cb->ptypmod[p] = ptypmod;
		cb->pcoll[p] = pcoll;
	}
	else if (typed && cb->typed[p] && (cb->ptype[p] != ptype || cb->ptypmod[p] != ptypmod || cb->pcoll[p] != pcoll))
		return -1;
	if (typed && !cb->typed[p])
	{
		cb->typed[p] = true;
		cb->ptype[p] = ptype;
		cb->ptypmod[p] = ptypmod;
		cb->pcoll[p] = pcoll;
	}
	return p;
}

ComboCtx *
new_combo(AggCtx *c, int join)
{
	ComboCtx   *cb = palloc0(sizeof(ComboCtx));

	cb->join = join;
	c->combos = lappend(c->combos, cb);
	return cb;
}

int
combo_index(AggCtx *c, ComboCtx *cb)
{
	foreach_ptr(ComboCtx, x, c->combos)
		if (x == cb)
			return foreach_current_index(x);
	return -1;
}

static int
combo_level(AggCtx *c, ComboCtx *cb)
{
	int			lvl = 0;

	for (int p = 0; p < cb->nl; p++)
		lvl = Max(lvl, cb->leaf[p] >= 0 ? leaf_level(c, cb->leaf[p]) : combo_level(c, list_nth(c->combos, cb->refcombo[p])));
	return lvl;
}

/* An input from an earlier lookup is identified by that output row, including its NULL extension. */
int
combo_reference(AggCtx *c, ComboCtx *cb, JoinCtx *jc, Var *v)
{
	int			col = join_col(jc, v),
				p = 0;

	if (col < 0 || jc->combo >= combo_index(c, cb))
		return -1;
	while (p < cb->nl && !(cb->leaf[p] == -1 && cb->refcombo[p] == jc->combo && cb->refcol[p] == col))
		p++;
	if (p == cb->nl)
	{
		if (p == SIFT_AGG_MAX_CLEAVES)
			return -1;
		cb->leaf[p] = -1;
		cb->refcombo[p] = jc->combo;
		cb->refcol[p] = col;
		cb->ptype[p] = v->vartype;
		cb->ptypmod[p] = v->vartypmod;
		cb->pcoll[p] = v->varcollid;
		cb->typed[p] = true;
		cb->nl++;
	}
	return p;
}

/* The table column m and the other side e of the equality q when m belongs to the join jc and a key is free; lt and rt are the operator's input types */
static OpExpr *
join_equality(AggCtx *c, JoinCtx *jc, Node *q, Node **m, Node **e, Oid *lt, Oid *rt)
{
	OpExpr	   *op = (OpExpr *) q;
	Node	   *l,
			   *r;

	if (!IsA(q, OpExpr) || jc->j.nkey == SIFT_AGG_MAX_JKEYS)
		return NULL;
	if (list_length(op->args) != 2 || builtin_cmp(op->opno, lt, rt) != SCMP_EQ)
		return NULL;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (join_of_var(c, l) == jc)
	{
		*m = l;
		*e = r;
	}
	else if (join_of_var(c, r) == jc)
	{
		*m = r;
		*e = l;
	}
	else
		return NULL;
	return op;
}

/* the column m of the joined table is the next key of the join */
static void
join_key(JoinCtx *jc, Var *m)
{
	SiftAggJoin *j = &jc->j;

	join_var(jc, m);
	j->keyatt[j->nkey] = m->varattno;
	j->keytype[j->nkey] = m->vartype;
}

/* `column = leaf` of the joined table: the equality of a dictionary leaf with a text column, of an integer leaf with an integer column */
bool
join_clause(AggCtx *c, JoinCtx *jc, Node *q)
{
	OpExpr	   *op;
	Oid			lt,
				rt;
	Node	   *m,
			   *e;
	SiftAggJoin *j = &jc->j;
	ComboCtx   *cb = list_nth(c->combos, jc->combo);
	SiftSrcLeaf *lf;
	int			leaf;
	Oid			mt;
	TypeClass	tc;

	if ((op = join_equality(c, jc, q, &m, &e, &lt, &rt)) == NULL)
		return false;
	if (bms_is_member(jc->rti, pull_varnos(c->root, e)) || (leaf = match_leaf(c, e)) < 0)
		return false;
	lf = leaf_of(c, leaf);
	mt = ((Var *) m)->vartype;
	tc = type_class(lt);
	if (lf->kind != SCL_SCALAR || lf->lossy)
		return false;
	if (lf->vtype == SCV_CODE)
	{
		if (lt != TEXTOID || rt != TEXTOID || (mt != TEXTOID && mt != VARCHAROID) || (lf->typid != TEXTOID && lf->typid != VARCHAROID) ||
			!OidIsValid(op->inputcollid) || !get_collation_isdeterministic(op->inputcollid))
			return false;
	}
	else if ((lf->vtype != SCV_I32 && lf->typid != INT8OID) || (tc != TC_INT && tc != TC_DATE) || tc != type_class(rt) || tc != type_class(mt) ||
			 tc != type_class(lf->typid))
		return false;
	join_key(jc, (Var *) m);
	if ((j->keypos[j->nkey] = combo_leaf(cb, leaf, false, lf->typid, -1, lf->vtype == SCV_CODE ? DEFAULT_COLLATION_OID : InvalidOid)) < 0)
		return false;
	use_leaf(c, leaf);
	j->nkey++;
	return true;
}

bool
combo_walker(Node *n, ComboWalk *w)
{
	int			id;
	JoinCtx    *jc;

	if (n == NULL || w->bad)
		return false;
	if (null_input_param(n))
	{
		w->null_input = true;
		return false;
	}
	/* a relabel keeps the representation: the leaf is read as what it is, and the relabel stays around the parameter */
	if (IsA(n, RelabelType))
		return combo_walker((Node *) ((RelabelType *) n)->arg, w);
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr))
	{
		if ((jc = join_of_var(w->c, n)) != NULL)
		{
			if (w->jc == NULL || jc->combo > w->jc->combo)
				w->jc = jc;
			w->vars = lappend(w->vars, n);
			return false;
		}
		if ((id = match_leaf(w->c, n)) >= 0)
		{
			int			p = 0;

			while (p < w->nl && w->leaf[p] != id)
				p++;
			if (p == w->nl && p == SIFT_AGG_MAX_CLEAVES)
				w->bad = true;
			else if (p < w->nl && (w->ptype[p] != exprType(n) || w->ptypmod[p] != exprTypmod(n) || w->pcoll[p] != exprCollation(n)))
				w->bad = true;
			else if (p == w->nl)
			{
				w->leaf[p] = id;
				w->ptype[p] = exprType(n);
				w->ptypmod[p] = exprTypmod(n);
				w->pcoll[p] = exprCollation(n);
				w->nl++;
			}
			w->bad |= !combo_leaf_ok(w->c, id);
			return false;
		}
		w->bad |= IsA(n, Var);
	}
	if (IsA(n, SubPlan) && lut_subplan_ok(w->c, (SubPlan *) n))
	{
		w->subparams = list_concat_copy(w->subparams, ((SubPlan *) n)->paramIds);
		w->c->nsubplan++;
		w->c->subplan_cost += ((SubPlan *) n)->startup_cost;
	}
	else if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && list_member_int(w->subparams, ((Param *) n)->paramid))
		return false;
	else if ((IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, Aggref) ||
			 IsA(n, WindowFunc) || IsA(n, GroupingFunc) || IsA(n, PlaceHolderVar) || IsA(n, CurrentOfExpr) || IsA(n, NextValueExpr) ||
			 IsA(n, SetToDefault))
		w->bad = true;
	return !w->bad && expression_tree_walker(n, combo_walker, w);
}

/* a divisor that is a constant other than zero */
static bool
nonzero_const(Node *n)
{
	Const	   *k = (Const *) strip_relabel(n);

	if (!IsA(k, Const) || k->constisnull)
		return false;
	if (k->consttype == NUMERICOID)
		return !DatumGetBool(DirectFunctionCall2(numeric_eq, k->constvalue, NumericGetDatum(int64_to_numeric(0))));
	return false;
}

/* can the function raise an error on some argument? Only what is known not to: leakproof functions (PostgreSQL's own promise), numeric arithmetic, and the widening of a leaf */
static bool
func_may_raise(Oid fid, List *args)
{
	switch (fid)
	{
		case F_NUMERIC_ADD:
		case F_NUMERIC_SUB:
		case F_NUMERIC_MUL:
		case F_NUMERIC_UMINUS:
		case F_NUMERIC_ABS:
		case F_ABS_NUMERIC:
		case F_SIGN_NUMERIC:
		case F_CEIL_NUMERIC:
		case F_CEILING_NUMERIC:
		case F_FLOOR_NUMERIC:
		case F_NUMERIC_INT2:
		case F_NUMERIC_INT4:
		case F_NUMERIC_INT8:
			return false;
		case F_ROUND_NUMERIC_INT4:
		case F_TRUNC_NUMERIC_INT4:
			return !IsA(strip_relabel(lsecond(args)), Const);
		case F_NUMERIC_DIV:
		case F_NUMERIC_MOD:
			return !nonzero_const(lsecond(args));
		case F_FLOAT8_NUMERIC:
			return !IsA(strip_relabel(linitial(args)), Var);
		default:
			return !get_func_leakproof(fid);
	}
}

/* may the expression raise an error (a division by zero, a cast, an overflow)? The leaves it reads are the Vars of the table */
static bool
may_raise_walker(Node *n, AggCtx *c)
{
	if (n == NULL)
		return false;
	if ((IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType)) && match_leaf(c, n) >= 0)
		return false;
	switch (nodeTag(n))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_RelabelType:
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CaseExpr:
		case T_CaseWhen:
		case T_CoalesceExpr:
		case T_ArrayExpr:
			break;
		case T_FuncExpr:
			if (func_may_raise(((FuncExpr *) n)->funcid, ((FuncExpr *) n)->args))
				return true;
			break;
		case T_OpExpr:
		case T_DistinctExpr:
			{
				OpExpr	   *o = (OpExpr *) n;

				Oid			lt,
							rt;

				set_opfuncid(o);
				if (builtin_cmp(o->opno, &lt, &rt) < 0 && func_may_raise(o->opfuncid, o->args))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) n;
				Oid			lt,
							rt;

				set_sa_opfuncid(sa);
				if (builtin_cmp(sa->opno, &lt, &rt) < 0 && func_may_raise(sa->opfuncid, sa->args))
					return true;
			}
			break;
		default:
			return true;
	}
	return expression_tree_walker(n, may_raise_walker, c);
}

/*
 * `column = expression of leaves` of the joined table: PostgreSQL evaluates the expression once per distinct tuple of the leaves'
 * values and the table is probed with the result (the key is not a leaf, so there is no id to read it from).
 */
bool
join_expr_clause(AggCtx *c, JoinCtx *jc, Node *q)
{
	OpExpr	   *op;
	Oid			lt,
				rt;
	Node	   *m,
			   *e;
	SiftAggJoin *j = &jc->j;
	ComboCtx   *cb = list_nth(c->combos, jc->combo);
	ComboWalk	w = {c};
	TypeClass	tc;
	Oid			mt,
				et;

	if ((op = join_equality(c, jc, q, &m, &e, &lt, &rt)) == NULL)
		return false;
	mt = ((Var *) m)->vartype;
	et = exprType(e);
	tc = type_class(lt);
	if (bms_is_member(jc->rti, pull_varnos(c->root, e)) || contain_volatile_functions(e))
		return false;
	if (lt == TEXTOID)
	{
		if (rt != TEXTOID || (mt != TEXTOID && mt != VARCHAROID) || (et != TEXTOID && et != VARCHAROID) ||
			!OidIsValid(op->inputcollid) || !get_collation_isdeterministic(op->inputcollid))
			return false;
	}
	else if ((tc != TC_INT && tc != TC_DATE) || tc != type_class(rt) || tc != type_class(mt) || tc != type_class(et))
		return false;
	combo_walker(e, &w);
	if (w.bad || (w.jc != NULL && w.jc->combo >= jc->combo) || (w.nl == 0 && w.vars == NIL))
		return false;
	for (int p = 0; p < w.nl; p++)
		if (combo_leaf(cb, w.leaf[p], true, w.ptype[p], w.ptypmod[p], w.pcoll[p]) < 0)
			return false;
	for (int p = 0; p < w.nl; p++)
		use_matched_leaf(c, w.leaf[p]);
	foreach_ptr(Var, v, w.vars)
		if (combo_reference(c, cb, join_of_var(c, (Node *) v), v) < 0)
			return false;
	join_key(jc, (Var *) m);
	j->keypos[j->nkey] = -1;
	jc->kexpr[j->nkey] = e;
	j->nkey++;
	return true;
}

/*
 * a slot for expr in the combo of its leaves (and of the table it reads columns of): its combo and slot numbers; false if it does not
 * fit. `force`: the join whose combo holds the slot, though the expression may read no column of its table (the rest of an ON clause).
 * `after_joins`: a deferred combo reads lookup outputs as inputs, so the expression is evaluated only for surviving joined tuples.
 */
bool
combo_slot_in(AggCtx *c, JoinCtx *force, Node *expr, bool is_key, bool after_joins, int *ci, int *si)
{
	ComboWalk	w = {c};
	ComboCtx   *cb = NULL;
	SlotReq    *rq;

	if (contain_volatile_functions(expr))
		return false;
	combo_walker(expr, &w);
	if (force && w.jc != NULL && w.jc->combo > force->combo)
		return false;
	if (after_joins)
		w.jc = NULL;
	else if (force)
		w.jc = force;
	if (w.bad || (w.nl == 0 && w.jc == NULL && !w.null_input && (!after_joins || w.vars == NIL)))
		return false;
	if (w.jc)
		cb = list_nth(c->combos, w.jc->combo);
	else if (!after_joins)
		foreach_ptr(ComboCtx, x, c->combos)
		{
			bool		same = x->join < 0 && x->nl == w.nl;

			for (int p = 0; same && p < w.nl; p++)
			{
				int			q = 0;

				while (q < x->nl && x->leaf[q] != w.leaf[p])
					q++;
				same = q < x->nl;
			}
			if (same)
				cb = x;
		}
	if (cb == NULL)
		cb = new_combo(c, -1);
	for (int p = 0; p < w.nl; p++)
		if (combo_leaf(cb, w.leaf[p], true, w.ptype[p], w.ptypmod[p], w.pcoll[p]) < 0)
			return false;
	foreach_ptr(Var, v, w.vars)
	{
		JoinCtx    *jc = join_of_var(c, (Node *) v);

		if ((jc == w.jc ? join_col(jc, v) : combo_reference(c, cb, jc, v)) < 0)
			return false;
	}
	for (int p = 0; p < w.nl; p++)
		use_matched_leaf(c, w.leaf[p]);
	rq = palloc0(sizeof(SlotReq));
	rq->expr = expr;
	rq->is_key = is_key;
	rq->agg = -1;
	rq->late = !is_key && w.jc == NULL && may_raise_walker(expr, c);
	cb->slots = lappend(cb->slots, rq);
	*ci = combo_index(c, cb);
	*si = list_length(cb->slots) - 1;
	return true;
}

bool
combo_slot(AggCtx *c, Node *expr, bool is_key, int *ci, int *si)
{
	return combo_slot_in(c, NULL, expr, is_key, false, ci, si);
}

typedef struct ComboSub
{
	AggCtx	   *c;
	ComboCtx   *cb;
	JoinCtx    *jc;
} ComboSub;

static Node *
combo_sub_mutator(Node *n, ComboSub *s)
{
	int			id;
	Param	   *p;

	if (n == NULL)
		return NULL;
	if (IsA(n, ScalarArrayOpExpr))
	{
		Node	   *r = expression_tree_mutator(n, combo_sub_mutator, s);

		hash_param_saop((ScalarArrayOpExpr *) r);
		return r;
	}
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr))
	{
		p = makeNode(Param);
		p->paramkind = PARAM_EXEC;
		p->location = -1;
		if (s->jc != NULL && join_of_var(s->c, n) == s->jc)
		{
			Var		   *v = (Var *) n;

			p->paramid = s->cb->nl + join_col(s->jc, v);
			p->paramtype = v->vartype;
			p->paramtypmod = v->vartypmod;
			p->paramcollid = v->varcollid;
			return (Node *) p;
		}
		if (join_of_var(s->c, n) != NULL)
		{
			Var		   *v = (Var *) n;

			p->paramid = combo_reference(s->c, s->cb, join_of_var(s->c, n), v);
			Assert(p->paramid >= 0);
			p->paramtype = v->vartype;
			p->paramtypmod = v->vartypmod;
			p->paramcollid = v->varcollid;
			return (Node *) p;
		}
		if ((id = match_leaf(s->c, n)) >= 0)
		{
			int			pos = combo_leaf(s->cb, id, false, 0, 0, 0);

			p->paramid = pos;
			p->paramtype = s->cb->ptype[pos];
			p->paramtypmod = s->cb->ptypmod[pos];
			p->paramcollid = s->cb->pcoll[pos];
			return (Node *) p;
		}
		pfree(p);
	}
	return expression_tree_mutator(n, combo_sub_mutator, s);
}

/* the expression of an argument or slot of combo ci over Params, now: the scope that resolves its columns is the one the caller is in */
Node *
lower_scoped(AggCtx *c, int ci, Node *expr)
{
	ComboCtx   *cb = list_nth(c->combos, ci);
	ComboSub	sub = {c, cb, cb->join >= 0 ? list_nth(c->joins, cb->join) : NULL};

	return combo_sub_mutator(expr, &sub);
}

/* the joins and combos of the query as the spec holds them: the slots' expressions over Params */
void
finish_combos(AggCtx *c, SiftAggSpec *s)
{
	s->njoin = list_length(c->joins);
	s->join = palloc0(Max(s->njoin, 1) * sizeof(SiftAggJoin));
	foreach_ptr(JoinCtx, jc, c->joins)
		s->join[foreach_current_index(jc)] = jc->j;
	s->ncombo = list_length(c->combos);
	s->combo = palloc0(Max(s->ncombo, 1) * sizeof(SiftAggCombo));
	foreach_ptr(ComboCtx, cb, c->combos)
	{
		SiftAggCombo *o = &s->combo[foreach_current_index(cb)];
		ComboSub	sub = {c, cb, cb->join >= 0 ? list_nth(c->joins, cb->join) : NULL};

		o->nleaves = cb->nl;
		o->join = cb->join;
		for (int p = 0; p < cb->nl; p++)
		{
			o->leaf[p] = cb->leaf[p];
			o->refcombo[p] = cb->refcombo[p];
			o->refcol[p] = cb->refcol[p];
			o->ptype[p] = cb->ptype[p];
			o->ptypmod[p] = cb->ptypmod[p];
			o->pcoll[p] = cb->pcoll[p];
		}
		o->level = combo_level(c, cb);
		if (sub.jc)
		{
			s->join[cb->join].args = NIL;
			foreach_ptr(Node, arg, sub.jc->j.args)
				s->join[cb->join].args = lappend(s->join[cb->join].args, sub.jc->j.scoped ? arg : combo_sub_mutator(arg, &sub));
		}
		for (int k = 0; sub.jc && k < sub.jc->j.nkey; k++)
			if (sub.jc->kexpr[k])
				s->join[cb->join].keyexpr[k] = combo_sub_mutator(sub.jc->kexpr[k], &sub);
		o->nslots = list_length(cb->slots);
		o->slot = palloc0(Max(o->nslots, 1) * sizeof(SiftAggSlot));
		foreach_ptr(SlotReq, rq, cb->slots)
		{
			SiftAggSlot *sl = &o->slot[foreach_current_index(rq)];

			sl->is_key = rq->is_key;
			sl->key = rq->key;
			sl->agg = rq->agg;
			sl->direct = rq->direct;
			sl->expr = rq->lowered ? rq->expr : combo_sub_mutator(rq->expr, &sub);
		}
	}
}

/* EXPLAIN: the leaves a combo reads */
char *
combo_text(AggCtx *c, int ci, Node *expr)
{
	ComboCtx   *cb = list_nth(c->combos, ci);
	StringInfoData s;

	initStringInfo(&s);
	for (int p = 0; p < cb->nl; p++)
		appendStringInfo(&s, "%s%s", p ? ", " : "", cb->leaf[p] >= 0 ? leaf_name(c, cb->leaf[p]) : "lookup output");
	return psprintf("%s: %s", s.data, expr_text(c, expr));
}

/* a boolean expression of several leaves, or of leaves and the columns of the matching row of a joined table */
SiftAggPred *
compile_combo_pred_at(AggCtx *c, Node *n, bool after_joins)
{
	int			ci,
				si;
	SiftAggPred *p;
	ComboCtx   *cb;

	if (exprType(n) != BOOLOID || !combo_slot_in(c, NULL, n, false, after_joins, &ci, &si))
		return NULL;
	cb = list_nth(c->combos, ci);
	p = new_pred(c, SAP_COMBO);
	p->leaf = ci;
	p->op = si;
	p->level = after_joins || cb->join >= 0 ? 2 : combo_level(c, cb);
	p->late = slot_req(c, ci, si)->late;
	p->desc = combo_text(c, ci, n);
	return p;
}

SiftAggPred *
compile_combo_pred(AggCtx *c, Node *n)
{
	return compile_combo_pred_at(c, n, false);
}
