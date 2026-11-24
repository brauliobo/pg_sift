/*
 * agg/v6/par/run_distinct.c - COUNT(DISTINCT x) of an integer column on the compute threads (agg/v6/par/run.h), alone in the statement: of all the rows (agg/v6/par/distinct.h, one set), or per group
 * of one integer key, when the keys of the granules lie in a range of at most DIST_GROUPS_MAX values (the group of a row is its key minus the least, so that no thread needs a table of keys). A
 * NULL is not counted; the NULL of a grouped statement is the scan's (a group may hold none of its rows' values).
 */
#include "postgres.h"

#include "agg/v6/par/run_int.h"
#include "agg/v6/v6_distinct_u64.h"

#define DIST_GROUPS_MAX		(1u << 16)

static bool
int_col(const SiftV6Col *c)
{
	const SiftV6Class cls = sift_v6_class(c->typid);

	return par_plain_col(c) && (cls == SV6C_I32 || cls == SV6C_I64);
}

static bool
distinct_shape(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	const SiftV6Agg *a;

	if (sp->rows || sp->nsets != 0 || sp->nbins != 0 || sp->nderive != 0 || sp->nfcols != 0 || sp->nfilters != 0 || sp->lock_k != 0 || sp->affine || sp->naggs != 1 || sp->nkeys > 1)
		return false;
	a = &sp->agg[0];
	if (a->kind != SV6_COUNT_DISTINCT || a->fclause != 0 || a->nxcols != 0 || !int_col(&a->arg) || (sp->nkeys == 1 && !int_col(&sp->key[0])))
		return false;
	sh->nk = sp->nkeys;
	if (sp->nkeys == 1)
		sh->key_at[0] = par_add_leaf(sh, par_leaf_of(first, &sp->key[0]), false);
	sh->val_at = par_add_leaf(sh, par_leaf_of(first, &a->arg), false);
	return true;
}

static void
distinct_sizes(const ParShape *sh, uint32 groups_est, uint64 rows, int nthreads, ParSizes *sz)
{
	const uint64 rec = sh->nk == 0 ? sizeof(uint64) : sizeof(SiftDuPair);

	sz->scratch = 8u << 20;
	sz->work = 8u << 20;
	sz->state_min = 8u << 20;
	sz->state_want = sz->state_min + 3 * rec * (rows / nthreads);
}

static void
distinct_setup(SiftV6ParScan *p)
{
	if (p->sh.nk == 1)
		sift_pin_win_track_zone(p->win, p->sh.leaf[p->sh.key_at[0]]);
}

static bool
distinct_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParDistinctBatch *out, SiftErr *err)
{
	SiftV6ParScan *p = arg;
	const ParShape *sh = &p->sh;
	ParLoad		ld;
	uint64	   *val;
	uint32	   *gid = NULL;
	uint32		n = 0;

	if (!par_load(sh, sift_pin_win_slot(p->win, lo), scratch, err, &ld))
		return false;
	if (ld.nact == 0)
		return true;
	val = sift_arena_alloc(scratch, ld.nact * sizeof(uint64), 8);
	if (val != NULL && sh->ngroups > 0)
		gid = sift_arena_alloc(scratch, ld.nact * sizeof(uint32), 8);
	if (val == NULL || (sh->ngroups > 0 && gid == NULL))
		return par_arena_full(scratch, err);
	PAR_EACH_ROW(&ld, r, i)
	{
		const ColView *v = &ld.cv[sh->val_at];

		if (sh->ngroups > 0)
		{
			const ColView *k = &ld.cv[sh->key_at[0]];
			const uint64 at = (uint64) k->i[r] - (uint64) sh->dmin;

			if (k->isnull[r] || v->isnull[r])
				return par_serial(err, "a NULL in a column of the aggregate");
			if (at >= sh->ngroups)
				return par_serial(err, "a key outside the zone of its granule");
			gid[n] = (uint32) at;
		}
		else if (v->isnull[r])
			continue;
		val[n++] = (uint64) v->i[r];
	}
	p->w[worker].rows += ld.nact;
	out->n = n;
	out->val = val;
	out->gid = gid;
	return true;
}

static void *
distinct_create(SiftV6ParScan *p, SiftParArenas *ar, uint64 rows, SiftErr *err)
{
	ParShape   *sh = &p->sh;
	const SiftParDistinctHooks hooks = {.fill = distinct_fill,.arg = p};
	int64		lo,
				hi;

	if (sh->nk == 1)
	{
		if (!sift_pin_win_zone_range(p->win, &lo, &hi) || (uint64) hi - (uint64) lo >= DIST_GROUPS_MAX)
			return sift_err_fill(err, SIFT_ERR_UNSUPPORTED, 0, 0, "the keys are not in a small range"), NULL;
		sh->dmin = lo;
		sh->ngroups = (uint32) ((uint64) hi - (uint64) lo + 1);
	}
	return sift_par_distinct_create(ar, sh->ngroups, &hooks, err);
}

/* The count of the set, or of each group, as the counts of disjoint groups the aggregate adds up */
static void
distinct_done(SiftV6ParScan *p)
{
	const SiftParDistinct *d = p->op;
	const ParShape *sh = &p->sh;
	int64		key[SIFT_V6_MAX_KEYS] = {0};
	SiftV6StrIn str[SIFT_V6_MAX_KEYS] = {{0}};
	SiftV6AccIn acc[1] = {{0}};
	uint64	   *count;

	if (sh->ngroups == 0)
	{
		acc[0].v0 = sift_par_distinct_count(d);
		sift_v6_group_fold_rec(p->g, key, str, 0, acc);
		return;
	}
	count = palloc(sizeof(uint64) * sh->ngroups);
	sift_par_distinct_groups(d, count);
	for (uint32 g = 0; g < sh->ngroups; g++)
	{
		if (count[g] == 0)
			continue;
		key[0] = (int64) ((uint64) sh->dmin + g);
		acc[0].v0 = count[g];
		sift_v6_group_fold_rec(p->g, key, str, 0, acc);
	}
	pfree(count);
}

const ParKind par_kind_distinct = {.shape = distinct_shape,.sizes = distinct_sizes,.create = distinct_create,.ops = &sift_par_distinct_ops,.setup = distinct_setup,.done = distinct_done};
