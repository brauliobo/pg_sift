/*
 * agg/v6/par/run_group.c - the grouped aggregation ORDER BY count or sum LIMIT K of SiftAggV6 on the compute threads (agg/v6/par/run.h), by integer and text keys in the order of the statement: the
 * keys and sums a thread decodes of a granule are given to agg/v6/par/group.h as arrays of 64-bit words and of texts. The key word of an integer is the value with its sign bit flipped, so that the
 * unsigned order of the words is the signed order of the values (the K best are ranked by the count or a sum, then by the keys in ascending order, as the statement says).
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/lsyscache.h"

#include "agg/v6/par/run_int.h"
#include "agg/v6/v6_expr_str.h"
#include "core/compat.h"
#include "core/popcount.h"

#define STATE_MIN		(8u << 20)
/* The groups a statement with no K may have for the threads to give them all to the leader, which folds each one in turn (more are merged by the workers of the plan, in parallel) */
#define ALL_GROUPS_MAX	(1u << 20)

static bool
keys_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	if (sp->nkeys < 1 || !par_keys_of(sp, first, sh))
		return false;
	sh->gs.nk = sh->nk;
	for (int i = 0; i < sh->nk; i++)
		sh->gs.strs |= sh->ktext[i] << i;
	return true;
}

/* The count and the sums the aggregates need, and the leaves they read */
static bool
aggs_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	for (int j = 0; j < sp->naggs; j++)
		if (!par_count_sum_of(&sp->agg[j], sp, first, sh))
			return false;
	return true;
}

/* The metric of the item, as the K best rank by it */
static bool
metric_of(const SiftV6Spec *sp, const SiftV6TopKItem *m, ParShape *sh)
{
	const SiftV6Agg *a;
	Oid			opfamily,
				opcintype;
	SiftCmpStrategy cmptype;
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
	sh->gs.desc = cmptype == SIFT_CMP_GT;
	return true;
}

/* The item is key `key` in ascending order (a text in the collation C, which is the order of its bytes) */
static bool
key_item(const SiftV6TopKItem *it, int key, const ParShape *sh)
{
	Oid			opfamily,
				opcintype;
	SiftCmpStrategy cmptype;

	get_ordering_op_properties(it->sortop, &opfamily, &opcintype, &cmptype);
	return it->pos == key && cmptype == SIFT_CMP_LT && !it->nulls_first && (!sift_par_group_is_str(&sh->gs, key) || sift_collate_is_c(it->collid));
}

/* The K best groups, by the metric of the first item of ORDER BY (none: by the keys alone) and then by the keys, ascending, after it */
static bool
rank_of(const SiftV6Spec *sp, ParShape *sh)
{
	const int	metric = sp->ntopk > 0 && sp->topk_item[0].pos >= sp->nkeys;

	if (sp->topk > SIFT_PAR_GROUP_TOPK_MAX || sp->ntopk < 1 || sp->ntopk > metric + sp->nkeys)
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

/* The K best of the threads when the statement keeps the best groups of all and the order is one they rank; else every group, when the planner expects few: the node ranks them, and runs its HAVING */
static bool
order_of(const SiftV6Spec *sp, ParShape *sh)
{
	if (sp->topk > 0 && !sp->having && rank_of(sp, sh))
		return true;
	sh->gs.topk = sh->gs.order = sh->gs.desc = 0;
	return sp->groups_est > 0 && sp->groups_est <= ALL_GROUPS_MAX;
}

static bool
group_shape(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	if (sp->rows || sp->nsets != 0 || sp->nbins != 0 || sp->nfcols != 0 || sp->nfilters != 0 || sp->lock_k != 0 || sp->naggs < 1)
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
	const uint32 texts = sift_pc64(sh->gs.strs);
	const uint64 rec = texts > 0 ? 8 * words + 56 * texts : 8 * (words <= 1 ? 1 : pg_nextpower2_32(words));

	sz->scratch = 8u << 20;
	sz->work = 8u << 20;
	sz->state_min = STATE_MIN;
	sz->state_want = STATE_MIN + 4 * rec * (groups_est > SIFT_PAR_GROUP_LOW ? rows / nthreads : Min(rows / nthreads, (uint64) groups_est)) +
		(sh->gs.topk == 0 ? 2 * rec * Min(groups_est, ALL_GROUPS_MAX) / nthreads : 0);
}

static void *
scratch_array(SiftArena *scratch, uint32 n, size_t size, SiftErr *err)
{
	void	   *a = sift_arena_alloc(scratch, n * size, 8);

	return a != NULL ? a : (par_arena_full(scratch, err), NULL);
}

/* The words of key j (the integers with their sign bit flipped) or its texts, for the rows that pass */
static bool
fill_key(const ParShape *sh, int j, const ParLoad *ld, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	if (sift_par_group_is_str(&sh->gs, j))
	{
		SiftStrRef *str = scratch_array(scratch, ld->nact, sizeof(SiftStrRef), err);

		if (str == NULL)
			return false;
		par_src_strs(&sh->key_src[j], ld, str);
		out->str[j] = str;
	}
	else
	{
		uint64	   *key = scratch_array(scratch, ld->nact, sizeof(uint64), err);

		if (key == NULL)
			return false;
		if (!par_src_ints(&sh->key_src[j], ld, PAR_SIGN_FLIP, key))
			return par_serial(err, "a key with no value");
		out->key[j] = key;
	}
	return true;
}

/* The first key is in the range where none of the keys that were elided (the first key plus a constant) overflows: else the scan raises the error of PostgreSQL */
static bool
affine_holds(const SiftV6Spec *sp, const uint64 *key, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		if (!sift_vx_affine_holds(sp, (int64) (key[i] ^ PAR_SIGN_FLIP)))
			return false;
	return true;
}

/* The keys and the sums of the rows that pass, into the batch */
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
		if (!fill_key(sh, j, &ld, scratch, out, err))
			return false;
	if (p->spec->affine && !affine_holds(p->spec, out->key[0], ld.nact))
		return par_serial(err, "a first key out of the range of the keys elided");
	for (int m = 0; m < sh->nsum; m++)
	{
		uint64	   *sum = scratch_array(scratch, ld.nact, sizeof(uint64), err);

		if (sum == NULL)
			return false;
		if (!par_src_ints(&sh->sum_src[m], &ld, 0, sum))
			return par_serial(err, "an argument with no value");
		out->arg[m] = sum;
	}
	p->w[worker].rows += ld.nact;
	out->n = ld.nact;
	return true;
}

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

static void *
group_create(SiftV6ParScan *p, SiftParArenas *ar, uint64 rows, SiftErr *err)
{
	const SiftParGroupHooks hooks = {.fill = group_fill,.emit = group_emit,.arg = p};

	return sift_par_group_create(ar, &p->sh.gs, p->spec->groups_est > SIFT_PAR_GROUP_LOW, rows, &hooks, err);
}

const ParKind par_kind_group = {.shape = group_shape,.sizes = group_sizes,.create = group_create,.ops = &sift_par_group_ops,.drains = true};
