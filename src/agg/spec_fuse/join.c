/*
 * agg/spec_fuse/join.c - the joins and combos of the arms over the pass
 * Which join and which combo slot of the pass each join and slot of an arm is, and the
 * slot that holds the rest of an ON clause.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"

#include "agg/spec_fuse/int.h"

typedef struct ColMap
{
	int			nleaves;
	const int  *col;
	List	   *subparams;		/* the output Params of the sub-plans around the expression: the planner's ids, not columns */
} ColMap;

/* the child's Params numbered by their place in the join's list, not by the planner's ids (the arms of a statement number them apart) */
static Node *
param_place_mutator(Node *n, const List *params)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
	{
		Param	   *p = copyObject((Param *) n);

		for (int i = 0; i < list_length(params); i++)
			if (list_nth_int(params, i) == p->paramid)
			{
				p->paramid = i;
				break;
			}
		p->location = -1;
		return (Node *) p;
	}
	return expression_tree_mutator(n, param_place_mutator, (void *) params);
}

/* the arm's join with what an arm may differ in left out: an inner join is a left join whose arm drops the tuples without a row (SAP_MATCHED), its columns (the union's), its plan numbers (a plan with the same query is the same), the slot of its ON clause */
static List *
join_shell(const SiftAggJoin *j)
{
	SiftAggJoin x = *j;
	SiftAggSpec t = {.njoin = 1, .join = &x};

	x.kind = x.kind == SJ_INNER ? SJ_LEFT : x.kind;
	x.child = x.child >= 0 ? 0 : -1;
	x.ncol = 0;
	x.subplan = 0;
	x.params = NIL;
	x.first.use = false;
	x.first.ncol = 0;
	x.first.pqual = (Node *) param_place_mutator(x.first.pqual, j->params);
	x.fslot = -1;
	x.desc = NULL;
	return sift_aggspec_pack(&t);
}

static bool
combo_same(const SiftAggCombo *a, const SiftAggCombo *b)
{
	if (a->nleaves != b->nleaves || a->join != b->join || a->level != b->level)
		return false;
	for (int p = 0; p < a->nleaves; p++)
		if (a->leaf[p] != b->leaf[p] || a->refcombo[p] != b->refcombo[p] || a->refcol[p] != b->refcol[p] || a->ptype[p] != b->ptype[p] ||
			a->ptypmod[p] != b->ptypmod[p] || a->pcoll[p] != b->pcoll[p])
			return false;
	return true;
}

/* the Params of a slot that read a column of the joined table, numbered over the columns of the pass */
static Node *
col_mutator(Node *n, ColMap *m)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, SubPlan))
	{
		SubPlan    *sp = (SubPlan *) n;
		List	   *outer = m->subparams;
		SubPlan    *r = makeNode(SubPlan);

		*r = *sp;
		r->args = (List *) col_mutator((Node *) sp->args, m);
		m->subparams = list_concat_copy(outer, sp->paramIds);
		r->testexpr = col_mutator(sp->testexpr, m);
		m->subparams = outer;
		return (Node *) r;
	}
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid >= m->nleaves &&
		!list_member_int(m->subparams, ((Param *) n)->paramid))
	{
		Param	   *p = copyObject((Param *) n);

		p->paramid = m->nleaves + m->col[p->paramid - m->nleaves];
		return (Node *) p;
	}
	return expression_tree_mutator(n, col_mutator, m);
}

/* the join of the arm in the pass: found by its shell, with the columns the arm reads added to the pass's */
bool
fuse_join(Fuse *f, FuseArm *arm, int j)
{
	const SiftAggJoin *mj = &arm->s->join[j];
	SiftAggJoin *u = NULL;
	List	   *shell = join_shell(mj);

	arm->join[j] = -1;
	for (int k = 0; k < f->u.njoin; k++)
		if (equal(join_shell(&f->u.join[k]), shell))
		{
			arm->join[j] = k;
			u = &f->u.join[k];
			break;
		}
	if (u == NULL)
	{
		if (f->u.njoin == sift_limit(joins, SIFT_AGG_MAX_JOINS))
			return false;
		arm->join[j] = f->u.njoin;
		u = &f->u.join[f->u.njoin++];
		*u = *mj;
		u->ncol = 0;
		u->fslot = -1;
	}
	if (mj->kind == SJ_LEFT)
		u->kind = SJ_LEFT;
	arm->col[j] = palloc(Max(mj->ncol, 1) * sizeof(int));
	for (int c = 0; c < mj->ncol; c++)
	{
		int			at = 0;

		while (at < u->ncol && !(u->colatt[at] == mj->colatt[c] && u->coltype[at] == mj->coltype[c] && u->coltypmod[at] == mj->coltypmod[c] &&
								 u->colcoll[at] == mj->colcoll[c]))
			at++;
		if (at == u->ncol)
		{
			if (u->ncol == sift_limit(join_cols, SIFT_AGG_MAX_JCOLS))
				return false;
			u->colatt[at] = mj->colatt[c];
			u->coltype[at] = mj->coltype[c];
			u->coltypmod[at] = mj->coltypmod[c];
			u->colcoll[at] = mj->colcoll[c];
			u->ncol++;
		}
		arm->col[j][c] = at;
	}
	return true;
}

SiftAggPred *
pred_noid(const SiftAggPred *p)
{
	SiftAggPred *o = palloc(sizeof(SiftAggPred));

	*o = *p;
	o->id = 0;
	o->desc = NULL;
	if (p->nchild > 0)
		o->child = palloc(p->nchild * sizeof(SiftAggPred *));
	for (int i = 0; i < p->nchild; i++)
		o->child[i] = pred_noid(p->child[i]);
	return o;
}

/* an aggregate as a spec of its own: two aggregates of arms over the same combos are the same when these are equal */
List *
agg_pack(const SiftAggAgg *a)
{
	SiftAggAgg	x = *a;
	SiftAggSpec t = {.naggs = 1, .agg = &x};

	x.filter = a->filter ? pred_noid(a->filter) : NULL;
	x.desc = NULL;
	return sift_aggspec_pack(&t);
}

/* the aggregate that a slot gives the ids for: the same in two arms when they are the same but for the slot they read */
static List *
slot_agg_key(const SiftAggAgg *a)
{
	SiftAggAgg	x = *a;

	x.combo = x.slot = 0;
	return agg_pack(&x);
}

static const SlotOwn *
slot_owner(const Fuse *f, int combo, int slot)
{
	foreach_ptr(SlotOwn, o, f->owners)
		if (o->combo == combo && o->slot == slot)
			return o;
	return NULL;
}

/* the combo of the arm in the pass, with the slots the arm asks for added to the pass's */
bool
fuse_combo(Fuse *f, FuseArm *arm, int ci)
{
	const SiftAggCombo *mc = &arm->s->combo[ci];
	SiftAggCombo x = *mc;
	SiftAggCombo *u;
	int			at = 0;
	ColMap		cm = {mc->nleaves, mc->join >= 0 ? arm->col[mc->join] : NULL, NIL};

	x.join = mc->join >= 0 ? arm->join[mc->join] : -1;
	for (int p = 0; p < x.nleaves; p++)
		if (x.leaf[p] < 0)
		{
			const SiftAggCombo *prior = &arm->s->combo[x.refcombo[p]];

			x.refcol[p] = arm->col[prior->join][x.refcol[p]];
			x.refcombo[p] = arm->combo[x.refcombo[p]];
		}
	while (at < f->u.ncombo && !combo_same(&f->u.combo[at], &x))
		at++;
	if (at == f->u.ncombo)
	{
		if (at == sift_limit(combos, SIFT_AGG_MAX_COMBOS))
			return false;
		f->u.combo[f->u.ncombo++] = x;
		f->u.combo[at].nslots = 0;
		f->u.combo[at].slot = NULL;
	}
	u = &f->u.combo[at];
	u->slot = u->slot ? repalloc(u->slot, Max(u->nslots + mc->nslots, 1) * sizeof(SiftAggSlot)) : palloc(Max(mc->nslots, 1) * sizeof(SiftAggSlot));
	arm->combo[ci] = at;
	arm->slot[ci] = palloc(Max(mc->nslots, 1) * sizeof(int));
	for (int s = 0; s < mc->nslots; s++)
	{
		SiftAggSlot sl = mc->slot[s];
		int			sat = 0;
		List	   *own;

		if (cm.col)
			sl.expr = col_mutator(sl.expr, &cm);
		own = sl.agg >= 0 ? slot_agg_key(&arm->s->agg[sl.agg]) : NIL;
		while (sat < u->nslots && !(u->slot[sat].is_key == sl.is_key && u->slot[sat].direct == sl.direct && equal(u->slot[sat].expr, sl.expr) &&
									(sl.agg < 0 ? u->slot[sat].agg < 0 : u->slot[sat].agg >= 0 && equal(slot_owner(f, at, sat)->key, own))))
			sat++;
		if (sat == u->nslots)
		{
			SlotOwn    *o = palloc(sizeof(SlotOwn));

			*o = (SlotOwn) {at, sat, arm - f->arm, sl.agg, own};
			if (sl.agg >= 0)
				f->owners = lappend(f->owners, o);
			u->slot[u->nslots++] = sl;
		}
		arm->slot[ci][s] = sat;
	}
	return true;
}

/* the ON clause of a join is a slot of its combo: the arms that share the join must have the same one */
void
fuse_fslot(FuseArm *arm, int j)
{
	const SiftAggJoin *mj = &arm->s->join[j];

	arm->fslot[j] = -1;
	for (int ci = 0; mj->fslot >= 0 && ci < arm->s->ncombo; ci++)
		if (arm->s->combo[ci].join == j)
		{
			arm->fslot[j] = arm->slot[ci][mj->fslot];
			arm->fcombo[j] = arm->combo[ci];
		}
}

/* the rest of the ON clause of a join: the pass's when every arm has the same, else the inner joins of the arms that have it test it on the rows (posts) */
bool
fuse_fslots(Fuse *f)
{
	for (int k = 0; k < f->u.njoin; k++)
	{
		int			fs = -2;
		bool		same = true,
					outer = false;

		for (int i = 0; i < f->n; i++)
			for (int j = 0; j < f->arm[i].s->njoin; j++)
				if (f->arm[i].join[j] == k)
				{
					same &= fs == -2 || fs == f->arm[i].fslot[j];
					fs = f->arm[i].fslot[j];
					outer |= f->arm[i].fslot[j] >= 0 && f->arm[i].s->join[j].kind != SJ_INNER;
				}
		if (!same && outer)
			return false;
		f->u.join[k].fslot = same ? fs : -1;
	}
	return true;
}
