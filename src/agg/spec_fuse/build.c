/*
 * agg/spec_fuse/build.c - the spec of arms that run in one pass
 * The leaves, the children and the keys of the pass, the shape the arms must share, and the
 * two entries the planner calls: sift_aggspec_fusable and sift_aggspec_fuse.
 */

/*
 * The arms of a UNION ALL that scan the same rows for the same aggregates and differ in their group keys (a time series at four
 * granularities) run in one pass as the grouping sets of one spec: the distinct keys of the arms, then the number of the set. The arms may
 * read different leaves, load different columns of a joined table and ask for different slots of a combo; the pass has the union of them,
 * and every arm is rewritten over it (its keys, aggregates and conjuncts name the slots of the pass) before it is compared with the others.
 */
#include "postgres.h"

#include "catalog/pg_operator.h"

#include "agg/spec_fuse/int.h"

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
	t.serial = NULL;
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
