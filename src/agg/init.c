/*
 * agg/init.c - scan setup from the spec: states, predicates, keys, combos, tables, buffers
 */
/* check-memory: allow-allocs every array here follows the keys, predicates, slots or constants of one plan or query, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "executor/nodeSubplan.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"

#include "agg/agg_int.h"

static bool
param_walker(Node *n, Param **found)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid < SIFT_AGG_INIT_PARAM)
	{
		*found = (Param *) n;
		return true;
	}
	return expression_tree_walker(n, param_walker, found);
}

/* the placeholder Param 0 of an expression: the type its input has */
static Param *
find_param(Node *expr)
{
	Param	   *p = NULL;

	param_walker(expr, &p);
	SIFT_INVARIANT(p != NULL, "sift: aggregate plan expression without its input");
	return p;
}

/*
 * The hash of an IN list with a parameter (hash_param_saop) is built from the array of its first evaluation and keeps pointing into it: a packed or expanded
 * array (a PL/pgSQL variable) is flattened into the memory the evaluation resets, so the parameter is the constant of the statement, copied where the expression lives.
 */
static Node *
hashed_array_const(ScalarArrayOpExpr *sa, AggExec *ex)
{
	Param	   *p = lsecond(sa->args);
	bool		isnull;
	Datum		d;

	if (!OidIsValid(sa->hashfuncid) || !IsA(p, Param) || p->paramkind != PARAM_EXTERN)
		return (Node *) sa;
	d = ExecEvalExprSwitchContext(ExecInitExpr((Expr *) p, EXPR_PARENT(ex)), ex->lutcx, &isnull);
	if (isnull)
		sa->hashfuncid = sa->negfuncid = InvalidOid;
	else
		d = PointerGetDatum(PG_DETOAST_DATUM_COPY(d));
	ResetExprContext(ex->lutcx);
	sa->args = list_make2(linitial(sa->args), makeConst(p->paramtype, p->paramtypmod, p->paramcollid, -1, isnull ? (Datum) 0 : d, isnull, false));
	return (Node *) sa;
}

/*
 * The output of an ARRAY sub-select (an InitPlan of the query, its Param offset by SIFT_AGG_INIT_PARAM) is the constant of the expression: the sub-select
 * runs now, the first time the lookup table needs it as in the normal plan, and the lookup table's own parameters are not the executor's. A
 * parameter an enclosing query level sets (level_param) is the constant it has now.
 */
Node *
init_param_mutator(Node *n, AggExec *ex)
{
	Param	   *p = (Param *) n;
	EState	   *estate = ex->css.ss.ps.state;
	ParamExecData *prm;
	int16		len;
	bool		byval;

	if (n == NULL)
		return NULL;
	if (IsA(n, ScalarArrayOpExpr))
		return hashed_array_const((ScalarArrayOpExpr *) expression_tree_mutator(n, init_param_mutator, ex), ex);
	if (!IsA(n, Param) || p->paramkind != PARAM_EXEC || (p->paramid < SIFT_AGG_INIT_PARAM && !level_param(n)))
		return expression_tree_mutator(n, init_param_mutator, ex);
	foreach_ptr(List, v, ex->initvals)
		if (intVal(linitial(v)) == p->paramid - SIFT_AGG_INIT_PARAM)
			return (Node *) copyObject(lsecond(v));
	prm = &estate->es_param_exec_vals[p->paramid - (level_param(n) ? 0 : SIFT_AGG_INIT_PARAM)];
	if (prm->execPlan != NULL)
		ExecSetParamPlan(prm->execPlan, GetPerTupleExprContext(estate));
	get_typlenbyval(p->paramtype, &len, &byval);
	return (Node *) makeConst(p->paramtype, p->paramtypmod, p->paramcollid, len, prm->value, prm->isnull, byval);
}

static bool
init_param_walker(Node *n, List **found)
{
	if (n == NULL || IsA(n, Integer) || IsA(n, String) || IsA(n, IntList) || IsA(n, OidList))
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid >= SIFT_AGG_INIT_PARAM)
	{
		foreach_ptr(Param, f, *found)
			if (f->paramid == ((Param *) n)->paramid)
				return false;
		*found = lappend(*found, n);
	}
	return expression_tree_walker(n, init_param_walker, found);
}

/* the outputs of the InitPlans the packed spec reads, run now, for the workers: for each the id of its Param and its value as a Const (the workers have no executor state that holds them) */
List *
init_values(AggExec *ex)
{
	List	   *params = NIL,
			   *vals = NIL;

	init_param_walker((Node *) ex->pack, &params);
	foreach_ptr(Param, p, params)
		vals = lappend(vals, list_make2(makeInteger(p->paramid - SIFT_AGG_INIT_PARAM), init_param_mutator((Node *) p, ex)));
	return vals;
}

/* the operand of a condition, evaluated when the scan starts: the state of an expression is made once for the scan (its parameters stand as constants for the scan, and a top-N asks for each operand three times) */
ExprState *
operand_state(AggExec *ex, Node *expr)
{
	MemoryContext old;
	OperandState *o;

	foreach_ptr(OperandState, c, ex->operands)
		if (c->expr == expr)
			return c->state;
	old = MemoryContextSwitchTo(ex->cxt);
	o = palloc(sizeof(OperandState));
	o->expr = expr;
	o->state = ExecInitExpr((Expr *) init_param_mutator(expr, ex), EXPR_PARENT(ex));
	ex->operands = lappend(ex->operands, o);
	MemoryContextSwitchTo(old);
	return o->state;
}

/*
 * The state of an expression of the scan that is kept for it (a lookup table, a combination). A sub-plan in it (its state, hash table, the scans
 * it starts and the slots it puts in the executor's tuple table) lives in the query's memory: PostgreSQL ends the sub-plans after this node,
 * which frees its own memory.
 */
static ExprState *
init_expr(AggExec *ex, Node *expr)
{
	MemoryContext old = MemoryContextSwitchTo(contain_subplans(expr) ? ex->css.ss.ps.state->es_query_cxt : ex->cxt);
	ExprState  *e = ExecInitExpr((Expr *) init_param_mutator(expr, ex), EXPR_PARENT(ex));

	MemoryContextSwitchTo(old);
	return e;
}

void
extend_layout(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcLayout *lay = MemoryContextAlloc(ex->cxt, sizeof(SiftSrcLayout));

	ex->nbase = ex->lay->nleaves;
	if (s->nvleaf == 0)
		return;
	*lay = *ex->lay;
	lay->leaf = MemoryContextAllocZero(ex->cxt, (ex->nbase + s->nvleaf) * sizeof(SiftSrcLeaf));
	memcpy(lay->leaf, ex->lay->leaf, ex->nbase * sizeof(SiftSrcLeaf));
	for (int j = 0; j < s->nvleaf; j++)
	{
		SiftSrcLeaf *v = &lay->leaf[ex->nbase + j];

		for (int i = 0; i < ex->nbase && sift_aggvleaf_member(&s->vleaf[j]); i++)
			if (lay->leaf[i].parent == s->vleaf[j].obj)
				*(strcmp(lay->leaf[i].name, "key") == 0 ? &ex->vl[j].key : &ex->vl[j].val) = i;
		v->id = ex->nbase + j;
		v->attno = lay->leaf[s->vleaf[j].obj].attno;
		v->parent = -1;
		v->kind = SCL_SCALAR;
		if (s->vleaf[j].bound)
		{
			Oid			sub = get_range_subtype(lay->leaf[s->vleaf[j].obj].typid);

			v->vtype = sift_aggbound_i32(sub) ? SCV_I32 : SCV_I64;
			v->typid = sub;
			continue;
		}
		v->vtype = s->vleaf[j].types ? SCV_CODE : s->vleaf[j].count ? SCV_I32 : lay->leaf[ex->vl[j].val].vtype;
		v->typid = s->vleaf[j].types ? TEXTOID : s->vleaf[j].count ? INT4OID : lay->leaf[ex->vl[j].val].typid;
	}
	lay->nleaves += s->nvleaf;
	ex->lay = lay;
}

/* does a combo take a column of the rows combo ci matched (a lookup key or expression over an earlier lookup's output)? */
static bool
combo_read(const SiftAggSpec *s, int ci)
{
	for (int o = 0; o < s->ncombo; o++)
		for (int p = 0; p < s->combo[o].nleaves; p++)
			if (o != ci && s->combo[o].leaf[p] < 0 && s->combo[o].refcombo[p] == ci)
				return true;
	return false;
}

/* a join whose keys the scan's key filter has tested passes every tuple that reaches it (an anti join does not, and neither a repeated key of an inner join) */
static void
semi_pass(AggExec *ex, int ci)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggJoin *j = &ex->spec->join[ex->spec->combo[ci].join];

	if (!cs->kfilt || j->kind == SJ_ANTI || (j->kind == SJ_INNER && !map_unique(cs->map)))
		return;
	cs->spass = true;
	ex->njpass++;
}

/* does the leaf hold the keys of a byte table: an int32 leaf (int2, int4, date, bool) or an int8 one (a numeric or timestamp leaf is an int64 of another scale) */
bool
semi_lut_leaf(const SiftSrcLayout *lay, int leaf)
{
	return lay->leaf[leaf].vtype == SCV_I32 || (lay->leaf[leaf].vtype == SCV_I64 && lay->leaf[leaf].typid == INT8OID);
}

/*
 * Is the join answered from a byte table of its keys, which semi_lut_build makes from the keys of the table alone? A semi, anti or inner join by one integer
 * key of an integer leaf of the rows (semi_lut_leaf), whose combo gives nothing but the rest of the ON clause (a condition of the table's own, which the load tests).
 */
bool
semi_lut_possible(const SiftAggSpec *s, int ci, int nbase)
{
	const SiftAggCombo *cb = &s->combo[ci];
	const SiftAggJoin *j = &s->join[cb->join];

	return (j->kind == SJ_SEMI || j->kind == SJ_ANTI || j->kind == SJ_INNER) && j->nkey == 1 && j->keypos[0] >= 0 && !j->keynull[0] && cb->nleaves == 1 &&
		sift_aggspec_integer_type(j->keytype[0]) && cb->leaf[0] >= 0 && cb->leaf[0] < nbase && sift_agg_semi_lut && !combo_read(s, ci) &&
		cb->nslots == (j->fslot >= 0 ? 1 : 0) && !(j->fslot >= 0 && slot_reads_leaf(cb, j->fslot));
}

/* the slots of the open-addressing set of n keys (at most half full) */
static uint32
semi_set_slots(uint32 n)
{
	uint32		slots = 16;

	while (slots / 2 < n)
		slots *= 2;
	return slots;
}

/* the byte table over [lo, hi] of the kept keys; false when the key of an inner join repeats (it repeats a tuple: only a key that holds once is a membership test) */
static bool
semi_tab_fill(SemiSet *ss, const MapTab *m, const bool *keep, int64 lo, int64 hi, MemoryContext cx, bool unique)
{
	ss->lo = lo <= hi ? lo : 0;
	ss->n = lo <= hi ? (uint32) (hi - lo + 1) : 0;
	ss->tab = MemoryContextAllocZero(cx, ss->n + SIFT_AGG_PAD);
	for (uint32 r = 0; r < m->nrow; r++)
		if (keep[r])
		{
			if (ss->tab[map_kint(m, r, 0) - ss->lo] && unique)
			{
				pfree(ss->tab);
				memset(ss, 0, sizeof(SemiSet));
				return false;
			}
			ss->tab[map_kint(m, r, 0) - ss->lo] = 1;
		}
	return true;
}

/* the open-addressing set of the kept keys, for keys that span more than a byte table holds; false as semi_tab_fill */
static bool
semi_hash_fill(SemiSet *ss, const MapTab *m, const bool *keep, uint32 nkeys, MemoryContext cx, bool unique)
{
	uint32		slots = semi_set_slots(nkeys);

	ss->mask = slots - 1;
	ss->key = MemoryContextAlloc(cx, (Size) slots * sizeof(int64));
	for (uint32 h = 0; h < slots; h++)
		ss->key[h] = PG_INT64_MIN;
	for (uint32 r = 0; r < m->nrow; r++)
		if (keep[r])
		{
			int64		k = map_kint(m, r, 0);
			uint32		h = (uint32) (((uint64) k * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & ss->mask;

			if (k == PG_INT64_MIN)
			{
				if (ss->has_min && unique)
					goto repeated;
				ss->has_min = true;
				continue;
			}
			while (ss->key[h] != PG_INT64_MIN && ss->key[h] != k)
				h = (h + 1) & ss->mask;
			if (ss->key[h] == k && unique)
				goto repeated;
			ss->key[h] = k;
		}
	return true;
repeated:
	pfree(ss->key);
	memset(ss, 0, sizeof(SemiSet));
	return false;
}

static void
semi_lut_build(AggExec *ex, int ci)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];
	MapTab	   *m = cs->map;
	const uint64 limit = (uint64) sift_agg_semi_lut_size * 1024;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	uint32		nkeys = 0;
	bool	   *keep;
	bool		ok;
	/* the table depends on the rows alone when the load tested the table's own conditions: the same rows give the same table */
	bool		kept = m->filtered || j->fslot < 0;
	MemoryContext cx = kept ? m->cxt : ex->cxt;
	bool		wide;

	/* a combo with no leaf (leaf[0] < 0) has none to read: semi_lut_possible says so first */
	if (!semi_lut_possible(ex->spec, ci, ex->nbase))
		return;
	wide = ex->lay->leaf[cb->leaf[0]].vtype == SCV_I64;
	if (!semi_lut_leaf(ex->lay, cb->leaf[0]))
	{
		semi_pass(ex, ci);
		return;
	}
	/* an int8 leaf the scan's key filter has tested needs no test of its own (the pass an int8 leaf had before it had a table) */
	if (wide)
		semi_pass(ex, ci);
	if (cs->spass)
		return;
	if (kept && semi_set_ready(&m->sset) && m->slut_kind == j->kind && m->slut_wide == wide && semi_set_bytes(&m->sset) <= limit)
	{
		cs->sset = m->sset;
		ex->nsemi++;
		ex->semi_bytes += semi_set_bytes(&cs->sset);
		return;
	}
	/* the rest of the ON clause (the table's own conditions) holds or not per row of the table: once, not per probe; a key outside the range of an int32 leaf matches none of its values */
	keep = palloc(Max(m->nrow, 1) * sizeof(bool));
	for (uint32 r = 0; r < m->nrow; r++)
	{
		keep[r] = wide || (map_kint(m, r, 0) >= PG_INT32_MIN && map_kint(m, r, 0) <= PG_INT32_MAX);
		if (keep[r] && j->fslot >= 0 && !m->filtered)
		{
			bool		isnull;
			Datum		d;

			for (int c = 0; c < j->ncol; c++)
				map_bind(ex, cb->nleaves + c, m, r, c);
			d = ExecEvalExprSwitchContext(cs->slot[j->fslot], ex->lutcx, &isnull);
			keep[r] = !isnull && DatumGetBool(d);
			if ((r & 1023) == 1023)
				ResetExprContext(ex->lutcx);
		}
		if (keep[r])
		{
			lo = Min(lo, map_kint(m, r, 0));
			hi = Max(hi, map_kint(m, r, 0));
			nkeys++;
		}
	}
	ResetExprContext(ex->lutcx);
	if (lo <= hi && (uint64) hi - (uint64) lo >= limit)
	{
		/* more values than the byte table holds: nothing to test where the scan's key filter has (semi_pass), else the keys as a set if that fits the same memory */
		semi_pass(ex, ci);
		if (cs->spass || (Size) semi_set_slots(nkeys) * sizeof(int64) > limit)
		{
			pfree(keep);
			return;
		}
		ok = semi_hash_fill(&cs->sset, m, keep, nkeys, cx, j->kind == SJ_INNER);
	}
	else
		ok = semi_tab_fill(&cs->sset, m, keep, lo, hi, cx, j->kind == SJ_INNER);
	pfree(keep);
	if (!ok)
		return;
	if (kept)
	{
		m->sset = cs->sset;
		m->slut_kind = j->kind;
		m->slut_wide = wide;
	}
	ex->nsemi++;
	ex->semi_bytes += semi_set_bytes(&cs->sset);
}

/* the tuples of `in` that pass a semi (the key is in the table) or anti (it is not, or NULL) join of the table, to the next stage */

void
init_states(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	ex->ps = MemoryContextAllocZero(ex->cxt, Max(s->npreds_total, 1) * sizeof(PredState));
	ex->ks = MemoryContextAllocZero(ex->cxt, Max(s->nkeys, 1) * sizeof(KeyState));
}

/* `$0 = 'literal'` of a text, either side: for a deterministic collation the strings that pass are those of the same bytes */
static bool
text_eq_literal(Node *e, const char **s, int *len)
{
	OpExpr	   *o = (OpExpr *) e;
	Node	   *a,
			   *b;
	text	   *t;

	if (!IsA(e, OpExpr) || o->opfuncid != F_TEXTEQ || list_length(o->args) != 2 || !OidIsValid(o->inputcollid) || !get_collation_isdeterministic(o->inputcollid))
		return false;
	a = linitial(o->args);
	b = lsecond(o->args);
	if (IsA(b, Param))
	{
		Node	   *x = a;

		a = b;
		b = x;
	}
	if (!IsA(a, Param) || !IsA(b, Const) || ((Const *) b)->constisnull)
		return false;
	t = DatumGetTextPP(((Const *) b)->constvalue);
	*s = VARDATA_ANY(t);
	*len = VARSIZE_ANY_EXHDR(t);
	return true;
}

void
register_pred(AggExec *ex, SiftAggPred *p)
{
	PredState  *st;

	if (p == NULL)
		return;
	st = &ex->ps[p->id];
	if (p->kind == SAP_LUT)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);

		st->expr = init_expr(ex, p->expr);
		st->param = find_param(p->expr);
		if (!text_eq_literal(p->expr, &st->eq_s, &st->eq_len))
			st->eq_s = NULL;
		rt_init(&st->rt, 1);
		if (ex->lay->leaf[p->leaf].vtype == SCV_I32)
			st->null_result = eval_bool(ex, st->expr, (Datum) 0, true);
		MemoryContextSwitchTo(old);
	}
	if (p->kind == SAP_STEP)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);

		st->expr = init_expr(ex, p->expr);
		st->param = find_param(p->expr);
		rt_init(&st->rt, 1);
		st->null_result = eval_bool(ex, st->expr, (Datum) 0, true);
		st->step_us = sift_aggexpr_utc_shadowed(p->expr) ? 1 : (int64) p->step * USECS_PER_SEC;
		st->step_zone = p->zoned ? zone_open(ex->cxt, session_timezone) : NULL;
		MemoryContextSwitchTo(old);
	}
	for (int i = 0; i < p->nchild; i++)
		register_pred(ex, p->child[i]);
}

/* every predicate the scan evaluates: the conjuncts of the rows and of the sets, the FILTER of the aggregates and of the inner aggregates of the arms */
void
register_preds(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	for (int i = 0; i < s->npred; i++)
		register_pred(ex, s->pred[i]);
	for (int i = 0; i < s->npost; i++)
		register_pred(ex, s->post[i]);
	for (int i = 0; s->setpred && i < s->nsets; i++)
		register_pred(ex, s->setpred[i]);
	for (int i = 0; i < s->naggs; i++)
		register_pred(ex, s->agg[i].filter);
	for (int k = 0; k < s->narms; k++)
		for (int i = 0; i < s->arm[k].ninner; i++)
			register_pred(ex, s->arm[k].inner[i].filter);
}

/* The interval each leaf's comparisons leave: a unit whose zone misses it holds no row that passes (docs/COL-PLAN.md, "Zone pruning") */

/* what a key keeps for the whole scan: its expression and the type of its input; the ids are the table's (table_init) */
void
register_key(AggExec *ex, int ki)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	const SiftSrcLeaf *lf = key_lf(ex, ki);
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	if (sk->kind == SAK_SET)
		k->ident = true;
	else if (sk->kind == SAK_LUT)
	{
		k->expr = init_expr(ex, sk->expr);
		k->param = find_param(sk->expr);
		/* the steps were planned for the UTC clock: with another meaning of UTC in this session's zone each value of the leaf is a step of its own, on no zone's clock (a key can name UTC over a call of the session zone's clock) */
		if (sk->bucket > 0 && sift_aggexpr_utc_shadowed(sk->expr))
			k->bucket = 1;
		else
		{
			k->bucket = (int64) sk->bucket * USECS_PER_SEC;
			k->zone = sk->zoned ? zone_open(ex->cxt, session_timezone) : NULL;
			k->zcut = sk->zoned == SIFT_ZONE_CUT;
		}
		k->cal = k->param != NULL && k->param->paramtype == DATEOID;
	}
	else if (sk->kind == SAK_NUM)
	{
		k->numk = true;
		idmap_init(&k->im);
	}
	else if (sk->kind == SAK_LEAF)
	{
		/* the leaf itself: the placeholder has the key's type */
		k->param = makeNode(Param);
		k->param->paramkind = PARAM_EXEC;
		k->param->paramtype = sk->typid;
		k->param->paramtypmod = sk->typmod;
		k->param->paramcollid = sk->collid;
		k->ident = lf->vtype == SCV_I32 || lf->vtype == SCV_F64;
		if (lf->vtype == SCV_F64)
			ident_go_wide(ex, k);
	}
	MemoryContextSwitchTo(old);
}

void
register_combos(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	bool		preserved = false;
	int			nj = 0;

	join_first_rows(ex);
	for (int j = 0; j < s->njoin; j++)
		preserved |= sift_aggjoin_preserves(s->join[j].kind);

	ex->comcxt = AllocSetContextCreate(ex->cxt, "sift agg combo", ALLOCSET_SMALL_SIZES);
	ex->cs = palloc0(Max(s->ncombo, 1) * sizeof(ComboSt));
	ex->clbase = palloc0(Max(s->ncombo, 1) * sizeof(int));
	ex->xcout = palloc0(Max(s->ncombo, 1) * sizeof(uint32 *));
	ex->cfree = palloc0(Max(s->ncombo, 1) * sizeof(uint32 *));
	ex->cfree_ok = palloc0(Max(s->ncombo, 1) * sizeof(bool));
	for (int ci = 0; ci < s->ncombo; ci++)
		nj += s->combo[ci].join >= 0 && !s->join[s->combo[ci].join].scoped;
	ex->jcombo = palloc0(Max(nj, 1) * sizeof(int));
	ex->stage = palloc0(Max(nj, 1) * sizeof(Tup));
	ex->alcout = palloc0(Max(nj, 1) * sizeof(uint32 **));
	ex->seid = palloc0(Max(nj, 1) * sizeof(uint32 *));
	ex->t0.cout = palloc0(Max(s->ncombo, 1) * sizeof(uint32 *));
	for (int x = 0; x < nj; x++)
	{
		ex->stage[x].cout = palloc0(Max(s->ncombo, 1) * sizeof(uint32 *));
		ex->alcout[x] = palloc0(Max(s->ncombo, 1) * sizeof(uint32 *));
	}
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		SiftAggCombo *cb = &s->combo[ci];
		ComboSt    *cs = &ex->cs[ci];

		cs->ninputs = Max(cb->nleaves, 1);
		cs->cxt = AllocSetContextCreate(ex->cxt, "sift agg combo cache", ALLOCSET_DEFAULT_SIZES);
		cs->resettable = cb->join < 0;
		cs->by_id = !preserved && cb->join < 0 && cb->nleaves == 1 && cb->leaf[0] >= 0 && leaf_wide(&ex->lay->leaf[cb->leaf[0]]);
		cs->fstep = sift_agg_float_step && cs->by_id && ex->lay->leaf[cb->leaf[0]].vtype == SCV_F64 && combo_fstep(cb, &cs->fs);
		cs->slot = palloc(Max(cb->nslots, 1) * sizeof(ExprState *));
		cs->tlen = palloc0(Max(cb->nslots, 1) * sizeof(int16));
		cs->tbyval = palloc0(Max(cb->nslots, 1) * sizeof(bool));
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			cs->slot[sl] = init_expr(ex, (Node *) cb->slot[sl].expr);
			get_typlenbyval(exprType(cb->slot[sl].expr), &cs->tlen[sl], &cs->tbyval[sl]);
		}
		for (int p = 0; p < cb->nleaves; p++)
		{
			const SiftSrcLeaf *lf;

			if (cb->leaf[p] < 0)
			{
				cs->resettable = false;
				continue;
			}
			lf = &ex->lay->leaf[cb->leaf[p]];

			cs->resettable &= lf->vtype != SCV_CODE;
			if (lf->typid == NUMERICOID && lf->vtype == SCV_I64 && lf->scale > 0)
				cs->unit[p] = DirectFunctionCall3(numeric_in, CStringGetDatum(psprintf("0.%0*d", lf->scale, 1)), ObjectIdGetDatum(InvalidOid),
												  Int32GetDatum(-1));
		}
		for (int ai = 0; ai < s->naggs; ai++)
			if (saa_x_sumavg(s->agg[ai].kind) && s->agg[ai].restype == NUMERICOID && s->agg[ai].combo == ci)
			{
				if (!cs->decode)
					cs->decode = palloc0(Max(cb->nslots, 1) * sizeof(bool));
				cs->decode[s->agg[ai].slot] = true;
			}
		combo_reset(ex, ci);
		if (cb->join >= 0)
			foreach_ptr(Node, arg, s->join[cb->join].args)
				cs->args = lappend(cs->args, init_expr(ex, arg));
		if (cb->join >= 0 && ex->fjoin && ex->fjoin[cb->join] && s->join[cb->join].first.pqual)
			cs->fpq = init_expr(ex, first_param_expr(&s->join[cb->join]));
		for (int k = 0; cb->join >= 0 && k < s->join[cb->join].nkey; k++)
			if (s->join[cb->join].keyexpr[k])
				cs->kx[k] = init_expr(ex, (Node *) s->join[cb->join].keyexpr[k]);
		ex->clbase[ci] = ex->ncl;
		ex->ncl += cb->nleaves;
		if (cb->join >= 0 && !s->join[cb->join].scoped)
			ex->jcombo[ex->nj++] = ci;
	}
	MemoryContextSwitchTo(old);
}

void
scan_buffers(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			maxv = VEC + PADW;

	arms_init(ex);

	ex->t0.sel = MemoryContextAlloc(ex->cxt, (maxv + 16) * sizeof(uint32));
	ex->t0.tidx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->t0.ridx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	for (int x = 0; x < ex->nj; x++)
	{
		Tup		   *st = &ex->stage[x];

		st->sel = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->tidx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->ridx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->cout[ex->jcombo[x]] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		ex->seid[x] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	}
	for (int ci = 0; ci < s->ncombo; ci++)
		for (int p = 0; p < s->combo[ci].nleaves; p++)
			if (ex->cvid[p] == NULL)
				ex->cvid[p] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint64));
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		ex->cfree[ci] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		ex->xcout[ci] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		if (s->combo[ci].join >= 0)
		{
			if (ex->cs[ci].map == NULL)
				ex->cs[ci].map = map_load(ex, ci);
			if (ex->fjoin && ex->fjoin[s->combo[ci].join] && ex->cs[ci].fmap == NULL)
			{
				ex->cs[ci].fmap = map_first_peek(ex, ci);
				ex->cs[ci].fthresh = Max(s->join[s->combo[ci].join].first.rows / 8, 1024);
			}
			if (s->join[s->combo[ci].join].params == NIL)
				semi_lut_build(ex, ci);
		}
	}
	ex->calive = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->peid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->pti = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->pri = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->gid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->kids = MemoryContextAlloc(ex->cxt, (Size) maxv * Max(s->nkeys, 1) * sizeof(uint32));
	ex->knul = MemoryContextAlloc(ex->cxt, (Size) Max(s->nkeys, 1) * (VECW + PADW) * sizeof(uint64));
	ex->cu32 = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->c64 = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint64));
	ex->okb = MemoryContextAlloc(ex->cxt, maxv);
	ex->ncnt = MemoryContextAlloc(ex->cxt, (s->naggs + 1) * sizeof(int64 *));
	ex->fwin = MemoryContextAllocZero(ex->cxt, maxv);
	ex->nb = MemoryContextAlloc(ex->cxt, maxv);
	if (s->naggs > 0)
		ex->vstk = MemoryContextAlloc(ex->cxt, (Size) SIFT_AGG_MAX_VX * maxv * sizeof(double));
	ex->pix = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->okm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm2 = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpu32 = MemoryContextAlloc(ex->cxt, (Size) maxv * Max(s->nkeys, 1) * sizeof(uint32));
	if (s->nsets > 1)
		ex->setkid = MemoryContextAlloc(ex->cxt, (Size) maxv * s->nkeys * sizeof(uint32));
}
