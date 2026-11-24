/*
 * agg/v6/par/run_group.c - the grouped aggregation ORDER BY count or sum LIMIT K of SiftAggV6 on the compute threads (agg/v6/par/run.h), by integer and text keys in the order of the statement: the
 * keys and sums a thread decodes of a granule are given to agg/v6/par/group.h as arrays of 64-bit words and of texts. The key word of an integer is the value with its sign bit flipped, so that the
 * unsigned order of the words is the signed order of the values (the K best are ranked by the count or a sum, then by the keys in ascending order, as the statement says).
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/lsyscache.h"

#include "agg/v6/par/run_int.h"
#include "core/compat.h"

#define STATE_MIN		(8u << 20)

/* ---- the shape of the statement ---- */

static bool
keys_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	sh->nk = sp->nkeys;
	sh->gs.nk = sp->nkeys;
	if (sp->nkeys < 1 || sp->nkeys > SIFT_PAR_GROUP_MAX_KEYS)
		return false;
	for (int i = 0; i < sp->nkeys; i++)
	{
		const SiftV6Col *c = &sp->key[i];
		const SiftV6Class cls = sift_v6_class(c->typid);
		const bool	text = cls == SV6C_TEXT;

		if (!par_plain_col(c) || !(cls == SV6C_I32 || cls == SV6C_I64 || text))
			return false;
		sh->gs.strs |= text << i;
		sh->key_at[i] = par_add_leaf(sh, par_leaf_of(first, c), text);
	}
	return true;
}

/* The count and the sums the aggregates need, and the leaves they read */
static bool
aggs_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	for (int j = 0; j < sp->naggs; j++)
	{
		const SiftV6Agg *a = &sp->agg[j];

		if (a->fclause != 0 || a->nxcols != 0)
			return false;
		if (a->kind == SV6_COUNT_STAR)
			continue;
		if (!par_plain_col(&a->arg))
			return false;
		if (a->kind == SV6_COUNT)
		{
			par_add_leaf(sh, par_leaf_of(first, &a->arg), false);
			continue;
		}
		if ((a->kind != SV6_SUM && a->kind != SV6_AVG) || (a->arg.typid != INT2OID && a->arg.typid != INT4OID) || sh->nsum == SIFT_PAR_GROUP_MAX_SUM)
			return false;
		sh->sum_at[sh->nsum++] = par_add_leaf(sh, par_leaf_of(first, &a->arg), false);
	}
	return true;
}

/* The metric of the item, as the K best rank by it */
static bool
metric_of(const SiftV6Spec *sp, const SiftV6TopKItem *m, ParShape *sh)
{
	const SiftV6Agg *a;
	Oid			opfamily,
				opcintype;
	CompareType cmptype;
	int			sum = 0;

	if (m->pos >= sp->nkeys + sp->naggs)
		return false;
	a = &sp->agg[m->pos - sp->nkeys];
	for (int j = 0; j < m->pos - sp->nkeys; j++)
		sum += sp->agg[j].kind == SV6_SUM || sp->agg[j].kind == SV6_AVG;
	if (a->kind == SV6_COUNT_STAR || a->kind == SV6_COUNT)
		sh->gs.order = 0;
	else if (a->kind == SV6_SUM)
		sh->gs.order = 1 + sum;
	else
		return false;
	get_ordering_op_properties(m->sortop, &opfamily, &opcintype, &cmptype);
	sh->gs.desc = cmptype == COMPARE_GT;
	return true;
}

/* The item is key `key` in ascending order (a text in the collation C, which is the order of its bytes) */
static bool
key_item(const SiftV6TopKItem *it, int key, const ParShape *sh)
{
	Oid			opfamily,
				opcintype;
	CompareType cmptype;

	get_ordering_op_properties(it->sortop, &opfamily, &opcintype, &cmptype);
	return it->pos == key && cmptype == COMPARE_LT && !it->nulls_first && (!sift_par_group_is_str(&sh->gs, key) || sift_collate_is_c(it->collid));
}

/* The K best groups: by the metric of the first item of ORDER BY (none: by the keys alone) and then by the keys, ascending, after it */
static bool
order_of(const SiftV6Spec *sp, ParShape *sh)
{
	const int	metric = sp->ntopk > 0 && sp->topk_item[0].pos >= sp->nkeys;

	if (sp->topk < 1 || sp->topk > SIFT_PAR_GROUP_TOPK_MAX || sp->ntopk < 1 || sp->ntopk > metric + sp->nkeys)
		return false;
	sh->gs.topk = sp->topk;
	sh->gs.order = SIFT_PAR_GROUP_BY_KEY;
	if (metric && !metric_of(sp, &sp->topk_item[0], sh))
		return false;
	for (int i = metric; i < sp->ntopk; i++)
		if (!key_item(&sp->topk_item[i], i - metric, sh))
			return false;
	return true;
}

static bool
group_shape(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	if (sp->rows || sp->nsets != 0 || sp->nbins != 0 || sp->nderive != 0 || sp->nfcols != 0 || sp->nfilters != 0 || sp->lock_k != 0 || sp->affine || sp->naggs < 1)
		return false;
	if (!keys_of(sp, first, sh) || !aggs_of(sp, first, sh) || !order_of(sp, sh))
		return false;
	sh->gs.nsum = sh->nsum;
	return sift_par_group_spec_holds(&sh->gs);
}

static void
group_sizes(const ParShape *sh, uint32 groups_est, uint64 rows, int nthreads, ParSizes *sz)
{
	const uint32 words = sh->nk + sh->nsum;
	const uint32 texts = __builtin_popcount(sh->gs.strs);
	const uint64 rec = texts > 0 ? 8 * words + 56 * texts : 8 * (words <= 1 ? 1 : pg_nextpower2_32(words));

	sz->scratch = 8u << 20;
	sz->work = 8u << 20;
	sz->state_min = STATE_MIN;
	sz->state_want = STATE_MIN + 4 * rec * (groups_est > SIFT_PAR_GROUP_LOW ? rows / nthreads : Min(rows / nthreads, (uint64) groups_est));
}

/* ---- a thread ---- */

static void *
scratch_array(SiftArena *scratch, uint32 n, size_t size, SiftErr *err)
{
	void	   *a = sift_arena_alloc(scratch, n * size, 8);

	return a != NULL ? a : (par_arena_full(scratch, err), NULL);
}

/* The key words (the integers with their sign bit flipped) and the texts of the rows that pass, and their sums, into the batch */
static bool
group_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	SiftV6ParScan *p = arg;
	const ParShape *sh = &p->sh;
	ParLoad		ld;

	if (!par_load(sh, sift_pin_win_slot(p->win, lo), scratch, err, &ld))
		return false;
	if (ld.nact == 0)
		return true;
	for (int c = 0; c < sh->nleaves; c++)
		if (par_has_null(&ld, &ld.cv[c]))
			return par_serial(err, "a NULL in a column of the aggregate");
	for (int j = 0; j < sh->nk; j++)
	{
		const ColView *cv = &ld.cv[sh->key_at[j]];

		if (sift_par_group_is_str(&sh->gs, j))
		{
			SiftStrRef *str = scratch_array(scratch, ld.nact, sizeof(SiftStrRef), err);

			if (str == NULL)
				return false;
			PAR_EACH_ROW(&ld, r, i)
				str[i] = par_str_at(cv, r);
			out->str[j] = str;
		}
		else
		{
			uint64	   *key = scratch_array(scratch, ld.nact, sizeof(uint64), err);

			if (key == NULL)
				return false;
			PAR_EACH_ROW(&ld, r, i)
				key[i] = (uint64) cv->i[r] ^ PAR_SIGN_FLIP;
			out->key[j] = key;
		}
	}
	for (int m = 0; m < sh->nsum; m++)
	{
		const int64 *v = ld.cv[sh->sum_at[m]].i;
		uint64	   *sum = scratch_array(scratch, ld.nact, sizeof(uint64), err);

		if (sum == NULL)
			return false;
		PAR_EACH_ROW(&ld, r, i)
			sum[i] = (uint64) v[r];
		out->arg[m] = sum;
	}
	p->w[worker].rows += ld.nact;
	out->n = ld.nact;
	return true;
}

/* ---- the leader ---- */

/* A group of the K best, or a partial aggregate of one, into the groups of the statement */
static void
group_emit(void *arg, int worker, const SiftParGroupRow *row)
{
	SiftV6ParScan *p = arg;
	const SiftV6Spec *sp = p->spec;
	int64		key[SIFT_V6_MAX_KEYS] = {0};
	SiftV6StrIn str[SIFT_V6_MAX_KEYS] = {{0}};
	SiftV6AccIn acc[SIFT_V6_MAX_KEYS + 16];
	uint64		nulls = 0;
	int			m = 0;

	if (p->finishing && p->npending > 0)
		return;
	Assert(sp->naggs <= lengthof(acc));
	for (int i = 0; i < p->sh.nk; i++)
		if (sift_par_group_is_str(&p->sh.gs, i))
		{
			str[i] = (SiftV6StrIn) {(const char *) row->str[i].p, row->str[i].len};
			nulls |= (uint64) (row->str[i].len == SIFT_STR_NULL) << i;
		}
		else
			key[i] = (int64) (row->key[i] ^ PAR_SIGN_FLIP);
	for (int j = 0; j < sp->naggs; j++)
	{
		acc[j] = (SiftV6AccIn) {.n = (int64) row->count};
		if (sp->agg[j].kind == SV6_SUM || sp->agg[j].kind == SV6_AVG)
		{
			acc[j].v0 = row->sum[m++];
			acc[j].v1 = (int64) acc[j].v0 < 0 ? -1 : 0;
		}
	}
	sift_v6_group_fold_rec(p->g, key, str, nulls, acc);
}

static void
group_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
}

static void *
group_create(SiftV6ParScan *p, SiftParArenas *ar, uint64 rows, SiftErr *err)
{
	const SiftParGroupHooks hooks = {.fill = group_fill,.emit = group_emit,.overflow = group_overflow,.arg = p};

	return sift_par_group_create(ar, &p->sh.gs, p->spec->groups_est > SIFT_PAR_GROUP_LOW, rows, &hooks, err);
}

const ParKind par_kind_group = {.shape = group_shape,.sizes = group_sizes,.create = group_create,.ops = &sift_par_group_ops,.drains = true};
