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
} Fuse;

/* the arm's join with what an arm may differ in left out: an inner join is a left join whose arm drops the tuples without a row (SAP_MATCHED), its columns (the union's), its plan numbers (a plan with the same query is the same), the slot of its ON clause */
static List *
join_shell(const SiftAggJoin *j)
{
	SiftAggJoin x = *j;
	SiftAggSpec t = {.njoin = 1, .join = &x};

	x.kind = x.kind == SJ_INNER ? SJ_LEFT : x.kind;
	x.ncol = 0;
	x.subplan = 0;
	x.params = NIL;
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
} ColMap;

/* the Params of a slot that read a column of the joined table, numbered over the columns of the pass */
static Node *
col_mutator(Node *n, ColMap *m)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid >= m->nleaves)
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
		if (f->u.njoin == SIFT_AGG_MAX_JOINS)
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
			if (u->ncol == SIFT_AGG_MAX_JCOLS)
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

/* the combo of the arm in the pass, with the slots the arm asks for added to the pass's */
static bool
fuse_combo(Fuse *f, FuseArm *arm, int ci)
{
	const SiftAggCombo *mc = &arm->s->combo[ci];
	SiftAggCombo x = *mc;
	SiftAggCombo *u;
	int			at = 0;
	ColMap		cm = {mc->nleaves, mc->join >= 0 ? arm->col[mc->join] : NULL};

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
		if (at == SIFT_AGG_MAX_COMBOS)
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

		if (cm.col)
			sl.expr = col_mutator(sl.expr, &cm);
		while (sat < u->nslots && !(u->slot[sat].is_key == sl.is_key && u->slot[sat].direct == sl.direct && u->slot[sat].agg == sl.agg &&
									equal(u->slot[sat].expr, sl.expr)))
			sat++;
		if (sat == u->nslots)
			u->slot[u->nslots++] = sl;
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
	if (p->kind == SAP_COMBO)
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

static SiftAggPred *
pred_noid(const SiftAggPred *p)
{
	SiftAggPred *o = palloc(sizeof(SiftAggPred));

	*o = *p;
	o->id = 0;
	if (p->nchild > 0)
		o->child = palloc(p->nchild * sizeof(SiftAggPred *));
	for (int i = 0; i < p->nchild; i++)
		o->child[i] = pred_noid(p->child[i]);
	return o;
}

/* a conjunct as a spec of its own: two conjuncts of arms over the same combos are the same when these are equal */
static List *
post_pack(const SiftAggPred *p)
{
	SiftAggPred *q = pred_noid(p);
	SiftAggSpec t = {.npost = 1, .post = &q};

	return sift_aggspec_pack(&t);
}

/* a conjunct of the arm: added to the pass's, with the arm among those that apply it */
static void
fuse_post(Fuse *f, const SiftAggPred *p, int arm)
{
	List	   *k = post_pack(p);
	int			at = 0;

	while (at < f->npost && !equal(post_pack(f->post[at]), k))
		at++;
	if (at == f->npost)
	{
		f->post[f->npost] = (SiftAggPred *) p;
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
	for (int i = 0; i < n; i++)
		for (int j = 0; j < m[i]->nleaves; j++)
		{
			int			at = 0;

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
	f->n = n;
	f->arm = palloc0(n * sizeof(FuseArm));
	memset(&f->u, 0, sizeof(f->u));
	f->u.join = palloc0(SIFT_AGG_MAX_JOINS * sizeof(SiftAggJoin));
	f->u.combo = palloc0(SIFT_AGG_MAX_COMBOS * sizeof(SiftAggCombo));
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
	if (!fuse_posts(f))
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
	for (int i = 0; i < s->npost; i++)
		if (s->post[i]->kind == SAP_ELEM_EXISTS)
			return false;
	for (int i = 0; i < s->njoin; i++)
		if (s->join[i].child >= 0 || sift_aggjoin_preserves(s->join[i].kind) || (s->join[i].params != NIL && s->join[i].sig == NULL))
			return false;
	return s->nkeys > 0 && s->narms == 0 && s->nsets == 0 && s->nprepost == 0;
}

/* what the arms must share: everything but their keys */
static List *
fuse_shape(const SiftAggSpec *a)
{
	SiftAggSpec t = *a;

	t.nkeys = 0;
	t.key = NULL;
	t.nworkers = 0;
	t.est_groups = 0;
	return sift_aggspec_pack(&t);
}

bool
sift_aggspec_fusable(SiftAggSpec *const *m, int n)
{
	Fuse		f;
	List	   *shape,
			   *keys;

	for (int i = 0; i < n; i++)
		if (!fuse_candidate(m[i]))
			return false;
	if (!fuse_build(m, n, &f))
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
sift_aggspec_fuse(SiftAggSpec *const *m, int n, int **keymap)
{
	Fuse		f;
	SiftAggSpec *spec = palloc(sizeof(SiftAggSpec));
	List	   *keys;
	int			nk;

	if (!fuse_build(m, n, &f))
		elog(ERROR, "sift: arms that cannot share a pass");
	keys = fused_keys(&f, keymap);
	nk = list_length(keys);
	fuse_key_slots(&f, keys);
	*spec = f.arm[0].a;
	Assert(nk < SIFT_AGG_MAX_KEYS && n <= SIFT_AGG_MAX_KEYS);
	spec->key = palloc0((nk + 1) * sizeof(SiftAggKey));
	spec->setmask = palloc0(n * sizeof(uint32));
	spec->nsets = n;
	spec->est_groups = 0;
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
	}
	spec->npost = f.npost;
	spec->nprepost = 0;
	spec->post = f.post;
	spec->postsets = f.postsets;
	spec->key[nk] = (SiftAggKey) {.kind = SAK_SET, .leaf = -1, .typid = INT4OID, .typmod = -1, .eqop = Int4EqualOperator, .desc = "grouping set"};
	spec->nkeys = nk + 1;
	return spec;
}
