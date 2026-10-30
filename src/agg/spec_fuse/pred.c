/*
 * agg/spec_fuse/pred.c - the conjuncts and aggregates of the arms over the pass
 * The conjuncts over the joined rows each once, the aggregates the arms share, and the
 * split of the conjuncts over the rows between the pass and the arms.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"

#include "agg/spec_fuse/int.h"

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
SiftAggPred *
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
bool
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
	if (p->expr && runs_plan(p->expr, (void *) (p->kind == SAP_LUT || p->kind == SAP_STEP ? p : NULL)))
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
bool
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
void
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
void
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

SiftAggPred *
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
bool
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
