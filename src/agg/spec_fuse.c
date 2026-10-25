/*
 * agg/spec_fuse.c - the spec of arms that run in one pass
 *
 * The arms of a UNION ALL that scan the same rows for the same aggregates and differ in their group keys (a time series at four
 * granularities) run in one pass as the grouping sets of one spec: the distinct keys of the arms, then the number of the set. The arms may
 * read different leaves, load different columns of a joined table and ask for different slots of a combo; the pass has the union of them,
 * and every arm is rewritten over it (its keys, aggregates and conjuncts name the slots of the pass) before it is compared with the others.
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"

#include "agg/plan_int.h"
#include "agg/spec.h"

typedef struct FuseArm
{
	const SiftAggSpec *s;
	int		   *join;			/* per join of the arm: its join in the pass */
	int		   *combo;			/* per combo */
	int		  **col;			/* per join: the column of each of its columns in the pass */
	int		  **slot;			/* per combo: the slot of each of its slots in the pass */
	int		   *fslot;			/* per join: the slot of the rest of its ON clause in the pass, -1 none */
	int		   *fcombo;			/* per join: the combo of that slot in the pass */
	SiftAggSpec a;				/* the arm over the joins and combos of the pass */
} FuseArm;

typedef struct Fuse
{
	int			n;
	FuseArm	   *arm;
	SiftAggSpec u;				/* the joins and combos of the pass, the leaves of all the arms */
	int			npost;			/* the conjuncts over the joined rows of the arms, each once, and the arms (bit i) that apply it; 0: all of them */
	SiftAggPred **post;
	uint32	   *postsets;
	int			npreds;			/* predicates of the pass: those of the arms (ids below npreds_total of the arm with most) and its own conjuncts */
	int			ncommon;		/* the conjuncts over the rows that every arm has: the pass applies them */
	SiftAggPred **common;
	SiftAggPred **resid;		/* per arm: its other conjuncts as one node, NULL none: they are applied to its set (spec.setpred) */
	int			nagg;			/* the aggregates of the pass: those of the arms, each once */
	SiftAggAgg *agg;
	uint32	   *aggsets;		/* per aggregate: the arms that read it; NULL when every arm has the same aggregates */
	int		  **aggmap;			/* per arm: the aggregate of the pass that is each of its aggregates */
	List	   *owners;			/* the slots that give the ids of an aggregate's values (SlotOwn): their aggregate is the pass's when it is known */
} Fuse;

typedef struct SlotOwn
{
	int			combo;
	int			slot;
	int			arm;
	int			agg;			/* the aggregate of the arm */
	List	   *key;			/* that aggregate, apart from the slot it reads */
} SlotOwn;

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

typedef struct ColMap
{
	int			nleaves;
	const int  *col;
	List	   *subparams;		/* the output Params of the sub-plans around the expression: the planner's ids, not columns */
} ColMap;

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
static bool
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

static SiftAggPred *
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
static List *
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
static bool
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
static void
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
static bool
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

static SiftAggPred *
remap_pred(const SiftAggPred *p, const FuseArm *arm)
{
	SiftAggPred *o;

	if (p == NULL)
		return NULL;
	o = palloc(sizeof(SiftAggPred));
	*o = *p;
	if (p->kind == SAP_COMBO || p->kind == SAP_ELEM_EXISTS)
	{
		o->leaf = arm->combo[p->leaf];
		o->op = arm->slot[p->leaf][p->op];
	}
	if (p->nchild > 0)
	{
		o->child = palloc(p->nchild * sizeof(SiftAggPred *));
		for (int i = 0; i < p->nchild; i++)
			o->child[i] = remap_pred(p->child[i], arm);
	}
	return o;
}

static bool
agg_reads_combo(const SiftAggAgg *a)
{
	return (a->kind >= SAA_M_COUNT && a->kind <= SAA_M_AVG) || a->kind == SAA_M_PG || (a->kind >= SAA_X_COUNT && a->kind <= SAA_X_AVG);
}

/* the conjunct with its own ids in the pass (an executor state each) */
static SiftAggPred *
pred_number(const SiftAggPred *p, int *next)
{
	SiftAggPred *o = palloc(sizeof(SiftAggPred));

	*o = *p;
	o->id = (*next)++;
	if (p->nchild > 0)
		o->child = palloc(p->nchild * sizeof(SiftAggPred *));
	for (int i = 0; i < p->nchild; i++)
		o->child[i] = pred_number(p->child[i], next);
	return o;
}

/* a conjunct as a spec of its own: two conjuncts of arms over the same combos are the same when these are equal */
static List *
pred_pack(const SiftAggPred *p)
{
	SiftAggPred *q = pred_noid(p);
	SiftAggSpec t = {.npred = 1, .pred = &q};

	return sift_aggspec_pack(&t);
}

/* a conjunct of the arm: added to the pass's, with the arm among those that apply it */
static void
fuse_post(Fuse *f, const SiftAggPred *p, int arm)
{
	List	   *k = pred_pack(p);
	int			at = 0;

	while (at < f->npost && !equal(pred_pack(f->post[at]), k))
		at++;
	if (at == f->npost)
	{
		f->post[f->npost] = pred_number(p, &f->npreds);
		f->postsets[f->npost++] = 0;
	}
	f->postsets[at] |= (uint32) 1 << arm;
}

static SiftAggPred *
join_pred(SiftAggPredKind kind, int combo, int slot, char *desc)
{
	SiftAggPred *p = palloc0(sizeof(SiftAggPred));

	p->kind = kind;
	p->leaf = combo;
	p->leaf2 = -1;
	p->op = slot;
	p->level = 2;
	p->desc = desc;
	return p;
}

/* the conjuncts of the arms: their own, and a row of the join where the arm has an inner join and the pass a left one */
static bool
fuse_posts(Fuse *f)
{
	int			total = 0;

	for (int i = 0; i < f->n; i++)
		total += f->arm[i].s->npost + 2 * f->arm[i].s->njoin;
	f->npost = 0;
	f->npreds = 0;
	for (int i = 0; i < f->n; i++)
		f->npreds = Max(f->npreds, f->arm[i].s->npreds_total);
	f->post = palloc(Max(total, 1) * sizeof(SiftAggPred *));
	f->postsets = palloc0(Max(total, 1) * sizeof(uint32));
	for (int i = 0; i < f->n; i++)
	{
		const FuseArm *arm = &f->arm[i];

		for (int j = 0; j < arm->s->npost; j++)
			fuse_post(f, arm->a.post[j], i);
		for (int ci = 0; ci < arm->s->ncombo; ci++)
			if (arm->s->combo[ci].join >= 0 && arm->s->join[arm->s->combo[ci].join].kind != f->u.join[arm->join[arm->s->combo[ci].join]].kind)
				fuse_post(f, join_pred(SAP_MATCHED, arm->combo[ci], 0, "a row of the join"), i);
		for (int j = 0; j < arm->s->njoin; j++)
			if (arm->fslot[j] >= 0 && f->u.join[arm->join[j]].fslot < 0)
				fuse_post(f, join_pred(SAP_COMBO, arm->fcombo[j], arm->fslot[j], "join condition"), i);
	}
	for (int k = 0; k < f->npost; k++)
	{
		bool		all = f->postsets[k] == (((uint32) 1 << f->n) - 1);

		/* a conjunct that raises an error runs on the tuples that reach it: after the conjuncts of every set, as it does in its arm */
		if (!all && f->post[k]->late)
			return false;
		if (all)
			f->postsets[k] = 0;
	}
	return true;
}

/* does the expression run a plan (a sub-select, or a parameter an InitPlan sets); the Param 0 of a SAP_LUT is the leaf's value */
static bool
runs_plan(Node *n, void *lut)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubPlan) || IsA(n, AlternativeSubPlan))
		return true;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && !(lut && ((Param *) n)->paramid == 0))
		return true;
	return expression_tree_walker(n, runs_plan, lut);
}

static bool
pred_runs_plan(const SiftAggPred *p)
{
	if (p->expr && runs_plan(p->expr, (void *) (p->kind == SAP_LUT ? p : NULL)))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_runs_plan(p->child[i]))
			return true;
	return false;
}

/*
 * The aggregates of the pass: those of the arms when they have the same, else the union of them, each set updating its own (spec.setaggs). The
 * sorted values of a percentile and the keys of GROUPING are numbered by the arm's own lists: arms with different lists cannot have them.
 */
static bool
fuse_aggs(Fuse *f)
{
	const SiftAggSpec *a0 = &f->arm[0].a;
	List	   *keys = NIL;
	int			total = 0;
	bool		same = true;

	f->aggmap = palloc(f->n * sizeof(int *));
	for (int i = 0; i < f->n; i++)
	{
		const SiftAggSpec *a = &f->arm[i].a;

		total += a->naggs;
		f->aggmap[i] = palloc(Max(a->naggs, 1) * sizeof(int));
		for (int k = 0; k < a->naggs; k++)
			f->aggmap[i][k] = k;
		same &= a->naggs == a0->naggs;
		for (int k = 0; same && k < a->naggs; k++)
			same = equal(agg_pack(&a->agg[k]), agg_pack(&a0->agg[k]));
	}
	f->nagg = a0->naggs;
	f->agg = a0->agg;
	f->aggsets = NULL;
	if (same)
		return true;
	f->nagg = 0;
	f->agg = palloc(Max(total, 1) * sizeof(SiftAggAgg));
	f->aggsets = palloc0(Max(total, 1) * sizeof(uint32));
	for (int i = 0; i < f->n; i++)
		for (int k = 0; k < f->arm[i].a.naggs; k++)
		{
			const SiftAggAgg *a = &f->arm[i].a.agg[k];
			List	   *key = agg_pack(a);
			int			at = 0;

			if (a->kind == SAA_PCT_CONT || a->kind == SAA_PCT_DISC || a->kind == SAA_GROUPING || (a->filter && pred_runs_plan(a->filter)))
				return false;
			while (at < f->nagg && !equal(list_nth(keys, at), key))
				at++;
			if (at == f->nagg)
			{
				keys = lappend(keys, key);
				f->agg[f->nagg] = *a;
				f->agg[f->nagg].filter = a->filter ? pred_number(a->filter, &f->npreds) : NULL;
				f->nagg++;
			}
			f->aggmap[i][k] = at;
			f->aggsets[at] |= (uint32) 1 << i;
		}
	foreach_ptr(SlotOwn, o, f->owners)
		f->u.combo[o->combo].slot[o->slot].agg = f->aggmap[o->arm][o->agg];
	return true;
}

/* the arm over the joins and combos of the pass */
static void
fuse_adapt(Fuse *f, FuseArm *arm)
{
	const SiftAggSpec *s = arm->s;
	SiftAggSpec *a = &arm->a;

	*a = *s;
	a->njoin = f->u.njoin;
	a->join = f->u.join;
	a->ncombo = f->u.ncombo;
	a->combo = f->u.combo;
	a->nleaves = f->u.nleaves;
	a->leaves = f->u.leaves;
	a->recheck = f->u.recheck;
	a->gate = f->u.gate;
	a->key = palloc(Max(s->nkeys, 1) * sizeof(SiftAggKey));
	memcpy(a->key, s->key, s->nkeys * sizeof(SiftAggKey));
	for (int j = 0; j < s->nkeys; j++)
		if (a->key[j].kind == SAK_COMBO)
		{
			a->key[j].slot = arm->slot[s->key[j].leaf][s->key[j].slot];
			a->key[j].leaf = arm->combo[s->key[j].leaf];
		}
	a->agg = palloc(Max(s->naggs, 1) * sizeof(SiftAggAgg));
	memcpy(a->agg, s->agg, s->naggs * sizeof(SiftAggAgg));
	for (int i = 0; i < s->naggs; i++)
	{
		a->agg[i].filter = remap_pred(s->agg[i].filter, arm);
		if (agg_reads_combo(&s->agg[i]) && s->agg[i].combo >= 0)
		{
			a->agg[i].slot = arm->slot[s->agg[i].combo][s->agg[i].slot];
			a->agg[i].combo = arm->combo[s->agg[i].combo];
		}
	}
	a->pred = palloc(Max(s->npred, 1) * sizeof(SiftAggPred *));
	for (int i = 0; i < s->npred; i++)
		a->pred[i] = remap_pred(s->pred[i], arm);
	a->post = palloc(Max(s->npost, 1) * sizeof(SiftAggPred *));
	for (int i = 0; i < s->npost; i++)
		a->post[i] = remap_pred(s->post[i], arm);
}

/* the arm without its conjuncts over the joined rows (the pass has them all, each set its own) */
static void
fuse_unpost(Fuse *f)
{
	for (int i = 0; i < f->n; i++)
	{
		f->arm[i].a.npost = 0;
		f->arm[i].a.nprepost = 0;
		f->arm[i].a.post = NULL;
	}
}

/*
 * does eval_pred test p on the rows of a chunk (the source and the joined tuples test the others); not one that runs a plan: the plan may read
 * the table with a node of its own that is another arm of the pass, which then waits for the pass
 */
static bool
routable(const SiftAggPred *p)
{
	if (pred_runs_plan(p))
		return false;
	switch (p->kind)
	{
		case SAP_AND:
		case SAP_OR:
			for (int i = 0; i < p->nchild; i++)
				if (!routable(p->child[i]))
					return false;
			break;
		case SAP_TRUE:
		case SAP_FALSE:
		case SAP_GATE:
		case SAP_CMP:
		case SAP_NCMP:
		case SAP_IN:
		case SAP_NULL:
		case SAP_LUT:
		case SAP_ANY_ELEM:
		case SAP_ELEM_ROW:
		case SAP_LEAFCMP:
			break;
		default:
			return false;
	}
	return !p->late && p->level <= 1;
}

static SiftAggPred *
junction(SiftAggPredKind kind, int n, SiftAggPred **child)
{
	SiftAggPred *p = palloc0(sizeof(SiftAggPred));

	p->kind = kind;
	p->leaf = p->leaf2 = -1;
	p->nchild = n;
	p->child = child;
	junction_done(p);
	return p;
}

/* the conjuncts over the rows that every arm has, and per arm those it has beyond them: false when an arm's cannot be tested on its own set */
static bool
fuse_split(Fuse *f)
{
	List	  **keys = palloc(f->n * sizeof(List *));
	List	   *common = NIL;

	for (int i = 0; i < f->n; i++)
	{
		keys[i] = NIL;
		for (int j = 0; j < f->arm[i].a.npred; j++)
			keys[i] = lappend(keys[i], pred_pack(f->arm[i].a.pred[j]));
	}
	f->ncommon = 0;
	f->common = palloc(Max(f->arm[0].a.npred, 1) * sizeof(SiftAggPred *));
	for (int j = 0; j < f->arm[0].a.npred; j++)
	{
		bool		all = true;

		for (int i = 1; i < f->n; i++)
			all &= list_member(keys[i], list_nth(keys[0], j));
		if (all)
		{
			common = lappend(common, list_nth(keys[0], j));
			f->common[f->ncommon++] = f->arm[0].a.pred[j];
		}
	}
	f->resid = palloc0(f->n * sizeof(SiftAggPred *));
	for (int i = 0; i < f->n; i++)
	{
		const SiftAggSpec *a = &f->arm[i].a;
		SiftAggPred **own = palloc(Max(a->npred, 1) * sizeof(SiftAggPred *));
		int			n = 0;

		for (int j = 0; j < a->npred; j++)
			if (!list_member(common, list_nth(keys[i], j)))
			{
				if (!routable(a->pred[j]))
					return false;
				own[n++] = a->pred[j];
			}
		f->resid[i] = n == 0 ? NULL : n == 1 ? own[0] : junction(SAP_AND, n, own);
	}
	return true;
}

/* the conjuncts of the pass: those every arm has, then the OR of what the arms have beyond them when each has some (it prunes the units and rows no arm needs); the sets' own */
static void
fuse_preds(Fuse *f, SiftAggSpec *spec)
{
	bool		all = true,
				any = false;

	for (int i = 0; i < f->n; i++)
	{
		all &= f->resid[i] != NULL;
		any |= f->resid[i] != NULL;
	}
	spec->npred = 0;
	spec->pred = palloc((f->ncommon + 1) * sizeof(SiftAggPred *));
	for (int k = 0; k < f->ncommon; k++)
		spec->pred[spec->npred++] = pred_number(f->common[k], &f->npreds);
	if (all)
	{
		SiftAggPred **any_of = palloc(f->n * sizeof(SiftAggPred *));

		memcpy(any_of, f->resid, f->n * sizeof(SiftAggPred *));
		spec->pred[spec->npred++] = pred_number(junction(SAP_OR, f->n, any_of), &f->npreds);
	}
	spec->setpred = NULL;
	if (any)
		spec->setpred = palloc0(f->n * sizeof(SiftAggPred *));
	for (int i = 0; any && i < f->n; i++)
		if (f->resid[i])
			spec->setpred[i] = pred_number(f->resid[i], &f->npreds);
}

/* the leaves the arms read together, and how: false when two arms read one leaf in different ways */
static bool
fuse_leaves(SiftAggSpec *const *m, int n, SiftAggSpec *out)
{
	int			total = 0,
				k = 0;
	int		   *leaves;
	uint8	   *recheck;

	for (int i = 0; i < n; i++)
		total += m[i]->nleaves;
	leaves = palloc(Max(total, 1) * sizeof(int));
	recheck = palloc0(Max(total, 1));
	out->gate = palloc0(Max(total, 1) * sizeof(uint64));
	for (int i = 0; i < n; i++)
		for (int j = 0; j < m[i]->nleaves; j++)
		{
			int			at = 0;

			/* the error is raised for the rows the common conjuncts keep, which an arm's own conjuncts would drop before it in the scan of the arm */
			if (n > 1 && m[i]->recheck[j] == SRK_CAST_TESTED)
				return false;
			/* the bits of a gate are the arm's aggregates */
			if (m[i]->gate[j] != 0)
				return false;
			while (at < k && leaves[at] < m[i]->leaves[j])
				at++;
			if (at < k && leaves[at] == m[i]->leaves[j])
			{
				if (recheck[at] != m[i]->recheck[j])
					return false;
				continue;
			}
			memmove(&leaves[at + 1], &leaves[at], (k - at) * sizeof(int));
			memmove(&recheck[at + 1], &recheck[at], k - at);
			leaves[at] = m[i]->leaves[j];
			recheck[at] = m[i]->recheck[j];
			k++;
		}
	out->nleaves = k;
	out->leaves = leaves;
	out->recheck = recheck;
	return true;
}

/*
 * The plan of a correlated child outputs the columns its arm reads (the others are NULL): the pass runs the child of an arm that reads all the
 * columns of the pass, and has none when no arm does.
 */
static bool
fuse_children(Fuse *f)
{
	for (int k = 0; k < f->u.njoin; k++)
	{
		SiftAggJoin *u = &f->u.join[k];
		bool		found = u->params == NIL;

		for (int i = 0; i < f->n && !found; i++)
			for (int j = 0; j < f->arm[i].s->njoin && !found; j++)
			{
				const SiftAggJoin *mj = &f->arm[i].s->join[j];

				if (f->arm[i].join[j] == k && mj->ncol == u->ncol)
				{
					u->subplan = mj->subplan;
					u->params = mj->params;
					u->first = mj->first;
					found = true;
				}
			}
		if (!found)
			return false;
	}
	return true;
}

static int
fuse_joined(const Fuse *f)
{
	int			n = 0;

	for (int ci = 0; ci < f->u.ncombo; ci++)
		n += f->u.combo[ci].join >= 0;
	return n;
}

/* the pass over the arms m[0..n): false when they cannot share one */
static bool
fuse_build(SiftAggSpec *const *m, int n, Fuse *f)
{
	int			nj = 0,
				nc = 0;

	f->n = n;
	f->owners = NIL;
	f->arm = palloc0(n * sizeof(FuseArm));
	memset(&f->u, 0, sizeof(f->u));
	for (int i = 0; i < n; i++)
	{
		nj += m[i]->njoin;
		nc += m[i]->ncombo;
	}
	f->u.join = palloc0(Max(Min(nj, sift_limit(joins, SIFT_AGG_MAX_JOINS)), 1) * sizeof(SiftAggJoin));
	f->u.combo = palloc0(Max(Min(nc, sift_limit(combos, SIFT_AGG_MAX_COMBOS)), 1) * sizeof(SiftAggCombo));
	if (!fuse_leaves(m, n, &f->u))
		return false;
	for (int i = 0; i < n; i++)
	{
		FuseArm    *arm = &f->arm[i];

		arm->s = m[i];
		arm->join = palloc(Max(m[i]->njoin, 1) * sizeof(int));
		arm->col = palloc0(Max(m[i]->njoin, 1) * sizeof(int *));
		arm->combo = palloc(Max(m[i]->ncombo, 1) * sizeof(int));
		arm->slot = palloc0(Max(m[i]->ncombo, 1) * sizeof(int *));
		arm->fslot = palloc(Max(m[i]->njoin, 1) * sizeof(int));
		arm->fcombo = palloc(Max(m[i]->njoin, 1) * sizeof(int));
		for (int j = 0; j < m[i]->njoin; j++)
		{
			if (!fuse_join(f, arm, j))
				return false;
		}
		for (int ci = 0; ci < m[i]->ncombo; ci++)
			if (!fuse_combo(f, arm, ci))
				return false;
		for (int j = 0; j < m[i]->njoin; j++)
			fuse_fslot(arm, j);
	}
	if (!fuse_fslots(f) || !fuse_children(f))
		return false;
	/* the joins repeat or drop the tuples of every set: an arm without one of them (or with another kind of it) is not a set of this pass */
	for (int i = 0; i < n; i++)
	{
		int			joined = 0;

		for (int ci = 0; ci < m[i]->ncombo; ci++)
			joined += m[i]->combo[ci].join >= 0;
		if (m[i]->njoin != f->u.njoin || joined != fuse_joined(f))
			return false;
	}
	for (int i = 0; i < n; i++)
		fuse_adapt(f, &f->arm[i]);
	if (!fuse_posts(f) || !fuse_split(f) || !fuse_aggs(f))
		return false;
	fuse_unpost(f);
	return true;
}

/* a key as a spec of its own: the keys of arms over the same combos are the same when these are equal */
static List *
key_pack(const SiftAggKey *k)
{
	SiftAggSpec t = {.nkeys = 1, .key = (SiftAggKey *) k};

	return sift_aggspec_pack(&t);
}

/* the distinct keys of the arms in the order of their first arm; per arm the position of each key in them (keymap, when asked for) */
static List *
fused_keys(const Fuse *f, int **keymap)
{
	List	   *packs = NIL,
			   *first = NIL;

	for (int i = 0; i < f->n; i++)
	{
		const SiftAggSpec *a = &f->arm[i].a;

		if (keymap)
			keymap[i] = palloc(Max(a->nkeys, 1) * sizeof(int));
		for (int j = 0; j < a->nkeys; j++)
		{
			List	   *k = key_pack(&a->key[j]);
			int			at = 0;

			foreach_ptr(List, p, packs)
			{
				if (equal(p, k))
					break;
				at++;
			}
			if (at == list_length(packs))
			{
				packs = lappend(packs, k);
				first = lappend(first, &a->key[j]);
			}
			if (keymap)
				keymap[i][j] = at;
		}
	}
	return first;
}

/* the table a slot of a key gives ids to is the key's own in the pass (slot.key): false when two keys of the pass read one slot */
static bool
fuse_key_slots(Fuse *f, List *keys)
{
	for (int k = 0; k < list_length(keys); k++)
	{
		const SiftAggKey *key = list_nth(keys, k);

		if (key->kind != SAK_COMBO)
			continue;
		for (int j = 0; j < k; j++)
		{
			const SiftAggKey *o = list_nth(keys, j);

			if (o->kind == SAK_COMBO && o->leaf == key->leaf && o->slot == key->slot)
				return false;
		}
		f->u.combo[key->leaf].slot[key->slot].key = k;
	}
	return true;
}

/* can an arm be one set of a pass */
static bool
fuse_candidate(const SiftAggSpec *s)
{
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN)
			return false;
	for (int i = 0; i < s->njoin; i++)
		if ((s->join[i].child >= 0 && s->join[i].sig == NULL) || sift_aggjoin_preserves(s->join[i].kind) || (s->join[i].params != NIL && s->join[i].sig == NULL))
			return false;
	return s->narms == 0 && s->nsets == 0 && s->nprepost == 0 && s->nord == 0;
}

/* what the arms must share: everything but their keys, aggregates and the conjuncts over the rows (fuse_aggs, fuse_split) */
static List *
fuse_shape(const SiftAggSpec *a)
{
	SiftAggSpec t = *a;

	t.nkeys = 0;
	t.key = NULL;
	t.nworkers = 0;
	t.est_groups = 0;
	t.est_rows = 0;
	/* the FILTERs are the same by what they test: not by their numbers (one more conjunct over the joined rows in an arm moves them) or their text; the conjuncts over the rows are split (fuse_split) */
	t.npreds_total = 0;
	t.desc_quals = NULL;
	t.npred = 0;
	t.pred = NULL;
	t.naggs = 0;
	t.agg = NULL;
	return sift_aggspec_pack(&t);
}

/*
 * the arms are one pass. Conjuncts of the arms' own cost the pass a filter of the chunk per set, which the arm alone gets from the source for
 * less: they are worth it when the arms share a lookup join, not when they only share a cheap decode. An arm that a unit's
 * pre-aggregates answer (preagg) is not given them either: the pass reads the rows
 */
bool
sift_aggspec_fusable(SiftAggSpec *const *m, int n, bool preagg)
{
	Fuse		f;
	List	   *shape,
			   *keys;
	bool		own;

	for (int i = 0; i < n; i++)
		if (!fuse_candidate(m[i]))
			return false;
	if (!fuse_build(m, n, &f))
		return false;
	own = f.aggsets != NULL;
	for (int i = 0; i < n; i++)
		own |= f.resid[i] != NULL;
	if (own && (preagg || f.u.njoin == 0))
		return false;
	keys = fused_keys(&f, NULL);
	if (list_length(keys) >= SIFT_AGG_MAX_KEYS || !fuse_key_slots(&f, keys))
		return false;
	shape = fuse_shape(&f.arm[0].a);
	for (int i = 1; i < n; i++)
		if (!equal(shape, fuse_shape(&f.arm[i].a)))
			return false;
	return true;
}

SiftAggSpec *
sift_aggspec_fuse(SiftAggSpec *const *m, int n, int **keymap, int ***aggmap)
{
	Fuse		f;
	SiftAggSpec *spec = palloc(sizeof(SiftAggSpec));
	List	   *keys;
	int			nk;

	SIFT_INVARIANT(fuse_build(m, n, &f), "sift: arms that cannot share a pass");
	keys = fused_keys(&f, keymap);
	nk = list_length(keys);
	fuse_key_slots(&f, keys);
	*spec = f.arm[0].a;
	Assert(nk < SIFT_AGG_MAX_KEYS && n <= SIFT_AGG_MAX_KEYS);
	spec->key = palloc0((nk + 1) * sizeof(SiftAggKey));
	spec->setmask = palloc0(n * sizeof(uint32));
	spec->nsets = n;
	spec->est_groups = 0;
	spec->est_rows = 0;
	spec->nworkers = 0;
	for (int k = 0; k < nk; k++)
		spec->key[k] = *(SiftAggKey *) list_nth(keys, k);
	for (int i = 0; i < n; i++)
	{
		for (int j = 0; j < f.arm[i].a.nkeys; j++)
			spec->setmask[i] |= (uint32) 1 << keymap[i][j];
		spec->setmask[i] |= (uint32) 1 << nk;
		spec->nworkers = Max(spec->nworkers, m[i]->nworkers);
		spec->est_groups += m[i]->est_groups;
		spec->est_rows += m[i]->est_rows;
	}
	spec->npost = f.npost;
	spec->naggs = f.nagg;
	spec->agg = f.agg;
	spec->setaggs = f.aggsets;
	*aggmap = f.aggmap;
	fuse_preds(&f, spec);
	spec->npreds_total = f.npreds;
	spec->nprepost = 0;
	spec->post = f.post;
	spec->postsets = f.postsets;
	spec->key[nk] = (SiftAggKey) {.kind = SAK_SET, .leaf = -1, .typid = INT4OID, .typmod = -1, .eqop = Int4EqualOperator, .desc = "grouping set"};
	spec->nkeys = nk + 1;
	/* the arms were planned with workers one by one: the aggregates of another arm (a count of the lookup's column) may be ones the leader cannot merge */
	if (!sift_aggspec_parallel_ok(spec))
		spec->nworkers = 0;
	return spec;
}
