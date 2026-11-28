/*
 * col/v1_engine/store_ops_preagg.c - SiftAggV6 answering the stored units of a Format 1 index or sift_table from their pre-aggregates (col/v1/store_ops.h SiftV1UnitSink).
 *
 * The source offers each unit it is about to read. A unit is answered when the statement is one the cubes hold (no key, or one key that is a dimension of the unit's histogram, and aggregates that are
 * counts, sums (of a leaf or of a line of one), least and greatest of the leaves the histogram has measures of, or of the zone of the unit when there is no key, and the distinct values of
 * a histogram dimension), the unit stands for exactly the rows the snapshot sees (stored, every heap page all-visible, no dead row or the pre-aggregates
 * net of them: SiftV1ReaderOps.unit_rows) and its zones decide every test of the statement for all its rows (or for none: it adds nothing). Any other unit is read as it
 * was, rows and visibility checks included, so an answer is the scan's.
 */
/* check-memory: allow-allocs every array here follows the keys, aggregates and tests of one statement or the groups of one unit, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/vexpr/str.h"
#include "col/source_fold.h"
#include "col/v1/preagg/preagg.h"
#include "col/v1/store_ops.h"
#include "col/v1_engine/store_ops_cal.h"
#include "col/v1_engine/store_ops_preagg_ext.h"
#include "col/v1_engine/store_ops_preagg_pred.h"
#include "col/v1_engine/store_ops_rollup.h"
#include "core/memtrack.h"

#define CUBE_KEYS	2

typedef struct CubeAgg
{
	const SiftV6Agg *a;
	int			leaf;			/* -1: count(*) */
	bool		is_float;
	int			slot;			/* the measure of the histogram that holds the leaf */
	bool		linear;			/* the sum or average of a line of the leaf */
	SiftV6Linear lin;
	int			nf;				/* the conjuncts of its FILTER (WHERE ..), tests of the histogram's dimension each group passes or not; 0: none */
	SiftV1Pred *f;
} CubeAgg;

typedef struct SiftV1Cube
{
	SiftV1UnitSink sink;
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6ScanStats *stats;
	int			nkeys;
	int			key_leaf[CUBE_KEYS];	/* the leaves of the keys */
	bool		key_text[CUBE_KEYS];
	SiftV1CalKey *cal_key;		/* the one key is an expression of its leaf the calendar cells give (col/v1_engine/store_ops_cal.h); NULL: it is the leaf */
	SiftV1Rollup *rollup;		/* the rollup of the rows a unit is folded from when no histogram of one leaf holds the statement (col/v1_engine/store_ops_rollup.h); NULL: none */
	int			dist_leaf;		/* no key: the leaf COUNT(DISTINCT) reads (its histogram gives the values), -1: none */
	bool		dist_text;
	int			hist_leaf;		/* the dimension of the histogram a unit is folded from: the key, else the leaf of COUNT(DISTINCT), else the leaf of the first test; -1: none */
	bool		hist_text;		/* hist_leaf is a string leaf: its groups are codes of the dictionary of the histogram */
	bool		by_group;		/* the unit's zones leave a test of hist_leaf undecided: each group of the histogram is tested */
	bool		filtered;		/* an aggregate has a FILTER: the units are folded from the groups of the histogram */
	Oid			cal_type;		/* hist_leaf is a date or timestamp leaf a unit with no histogram of it is folded from the calendar cells of, by their spans; InvalidOid: none */
	bool	   *cell_keep;		/* ... the cells of the unit the tests keep (SIFT_PAGG_CAL_GROUPS and its NULL group) */
	CubeAgg    *agg;
	int			npred;
	SiftV1Pred *pred;
	SiftSrcRollupNeed need;
	V6Acc	   *acc;
	void	   *kdata;
	uint64		kvalid[SIFT_VEC_WORDS];
	SiftVec		kvec;
	SiftVec    *kvp[1];
} SiftV1Cube;

/* Whether group r passes the FILTER of aggregate g */
static bool
filter_passes(const CubeAgg *g, const SiftSrcHist *h, uint32 r)
{
	for (int i = 0; i < g->nf; i++)
		if (!sift_v1_pred_passes(&g->f[i], h, r))
			return false;
	return true;
}

/* Whether group r passes every test of the histogram's dimension (SiftV1HistKeep) */
static bool
group_kept(const void *arg, const SiftSrcHist *h, uint32 r)
{
	const SiftV1Cube *c = arg;

	if (h->grain > 0)
		return c->cell_keep[r];
	for (int i = 0; i < c->npred; i++)
		if (c->pred[i].leaf == c->hist_leaf && !sift_v1_pred_passes(&c->pred[i], h, r))
			return false;
	return true;
}

/* A leaf whose stored measures the aggregate reads: a numeric one is held as the integer of its scale, which must be the scale of the aggregate's argument, and has no set of distinct values */
static bool
leaf_measure(const SiftSrcLeaf *l, const SiftV6Agg *a)
{
	return l != NULL && !l->wide && !l->lossy && (l->typid != NUMERICOID || (a->kind != SV6_COUNT_DISTINCT && l->vtype == SCV_I64 && l->scale == a->arg.scale));
}

/*
 * What the tests of the statement say of every row of the unit: SV1V_NONE when one of them passes none. A test of the histogram's dimension that the zone leaves undecided
 * is made on each group (by_group). *except: the leaf of a test `leaf IS NULL` of another leaf the zones leave undecided, which the exceptions of the unit may decide, -1: none;
 * *alone: it is the only test they leave undecided
 */
static SiftV1Verdict
unit_verdict(SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, int *except, bool *alone)
{
	SiftV1Verdict all = SV1V_ALL;
	int			some = 0;

	c->by_group = false;
	*except = -1;
	for (int i = 0; i < c->npred; i++)
	{
		const bool	dim = c->pred[i].leaf == c->hist_leaf;
		SiftSrcUnitAgg z;
		SiftV1Verdict v;

		if (!rd->unit_agg(src, unit, c->pred[i].leaf, &z) || z.nvals != unit->nrow)
			v = SV1V_SOME;
		else if ((v = sift_v1_pred_verdict(&c->pred[i], &z)) == SV1V_NONE)
			return SV1V_NONE;
		c->by_group |= dim && v == SV1V_SOME;
		all = dim ? all : Min(all, v);
		some += !dim && v == SV1V_SOME;
		if (!dim && v == SV1V_SOME && *except < 0 && c->pred[i].kind == SV1P_NULL)
			*except = c->pred[i].leaf;
	}
	*alone = some == 1;
	return all;
}

/* ---- cells to states ---- */

static void
acc_sum(const CubeAgg *g, int64 n, SiftSrcI128 isum, double fsum, V6Acc *out)
{
	out->n = n;
	if (n == 0)
		return;
	if (g->is_float)
		*out = v6g_acc_of_f64_sum(fsum, 0, n);
	else
		v6acc_set_i128(out, (SiftI128) {(uint64) isum, (int64) (isum >> 64)});
}

static void
acc_extreme(const CubeAgg *g, int64 n, int64 imin, int64 imax, double fmin, double fmax, V6Acc *out)
{
	const bool	min = g->a->kind == SV6_MIN;

	out->n = n;
	if (n == 0)
		return;
	if (g->is_float)
		v6acc_set_f64(out, min ? fmin : fmax);
	else
		out->v0 = (uint64) (min ? imin : imax);
}

/* The state of the unit's whole rows for aggregate g from the zone of its leaf; false when the unit has no such zone */
static bool
global_acc(const SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, const CubeAgg *g, V6Acc *out)
{
	SiftSrcUnitAgg z;
	int64		n;

	*out = (V6Acc) {0};
	if (g->a->kind == SV6_COUNT_STAR)
	{
		out->n = unit->nrow;
		return true;
	}
	if (g->a->kind == SV6_COUNT_DISTINCT)
		return true;
	if (!rd->unit_agg(src, unit, g->leaf, &z) || z.nvals != unit->nrow || z.loose)
		return false;
	n = z.nvals - z.nnull;
	if (g->linear)
		return (n == 0 || (z.has_sums && z.has_minmax)) && sift_v1_line_acc(&g->lin, n, z.sum, z.min, z.max, out);
	switch (g->a->kind)
	{
		case SV6_COUNT:
			out->n = n;
			return true;
		case SV6_SUM:
		case SV6_AVG:
			if (n > 0 && !g->is_float && !z.has_sums)
				return false;
			acc_sum(g, n, z.sum, z.sum_f, out);
			return true;
		default:
			if (n > 0 && !z.has_minmax)
				return false;
			acc_extreme(g, n, z.min, z.max, z.min_f, z.max_f, out);
			return true;
	}
}

static void
merge(SiftV1Cube *c, SiftVec *const *keys, int n)
{
	sift_v6_group_merge_cells(c->group, keys, c->acc, (uint16) n);
}

/* The state of aggregate g over the groups of h that pass the tests of its dimension; false for an aggregate other than count(*) and COUNT(DISTINCT) of the dimension */
static bool
kept_acc(const SiftV1Cube *c, const SiftSrcHist *h, const CubeAgg *g, V6Acc *out)
{
	*out = (V6Acc) {0};
	if (g->a->kind == SV6_COUNT_STAR)
		for (uint32 r = 0; r < h->ngroups; r++)
			out->n += group_kept(c, h, r) ? h->count[r] : 0;
	return g->a->kind == SV6_COUNT_STAR || g->a->kind == SV6_COUNT_DISTINCT;
}

static bool
fold_global(SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	SiftSrcHist h;

	if (c->dist_leaf >= 0 && (!rd->unit_hist(src, unit, c->dist_leaf, &(SiftSrcRollupNeed) {0}, &h) || (c->dist_text && h.dict == NULL)))
		return false;
	for (int j = 0; j < c->spec->naggs; j++)
		if (!(c->by_group ? kept_acc(c, &h, &c->agg[j], &c->acc[j]) : global_acc(c, rd, src, unit, &c->agg[j], &c->acc[j])))
			return false;
	merge(c, NULL, 1);
	for (int j = 0; j < c->spec->naggs; j++)
		if (c->agg[j].a->kind == SV6_COUNT_DISTINCT)
			sift_v1_hist_merge_dist(c->group, j, &h, c->dist_text, c->kdata, c->by_group ? group_kept : NULL, c);
	return true;
}

/* ---- the groups of a histogram ---- */

/* Where each aggregate is in the histogram; false when it lacks what the aggregate reads */
static bool
bind_hist(SiftV1Cube *c, const SiftSrcHist *h)
{
	for (int j = 0; j < c->spec->naggs; j++)
	{
		CubeAgg    *g = &c->agg[j];
		int			m = 0;

		g->slot = -1;
		if (g->leaf < 0 || g->leaf == c->key_leaf[0])
			continue;
		while (m < h->nmeas && h->meas[m] != g->leaf)
			m++;
		if (m == h->nmeas)
			return false;
		g->slot = m;
		if (g->linear && (h->sum[m] == NULL || h->min[m] == NULL || h->max[m] == NULL))
			return false;
		if (g->a->kind == SV6_SUM || g->a->kind == SV6_AVG)
		{
			if (g->is_float ? h->sumf[m] == NULL : h->sum[m] == NULL)
				return false;
		}
		else if ((g->a->kind == SV6_MIN || g->a->kind == SV6_MAX) && (g->is_float || h->min[m] == NULL || h->max[m] == NULL))
			return false;
	}
	return true;
}

/* The states of the aggregates of group r; false when one is not the group's (a line that may leave its type) */
static bool
group_acc(const SiftV1Cube *c, const SiftSrcHist *h, uint32 r, V6Acc *out)
{
	const bool	null_group = h->has_null && r == h->ngroups - 1;

	for (int j = 0; j < c->spec->naggs; j++)
	{
		const CubeAgg *g = &c->agg[j];
		const int	m = g->slot;

		out[j] = (V6Acc) {0};
		if (!filter_passes(g, h, r))
			continue;
		if (g->a->kind == SV6_COUNT_STAR)
			out[j].n = h->count[r];
		else if (g->a->kind == SV6_COUNT_DISTINCT)
			continue;
		else if (g->linear && !sift_v1_line_acc(&g->lin, h->nn[m][r], h->sum[m][r], h->min[m][r], h->max[m][r], &out[j]))
			return false;
		else if (g->linear)
			continue;
		else if (m < 0)
			out[j].n = null_group ? 0 : h->count[r];
		else if (g->a->kind == SV6_COUNT)
			out[j].n = h->nn[m][r];
		else if (g->a->kind == SV6_MIN || g->a->kind == SV6_MAX)
			acc_extreme(g, h->nn[m][r], h->min[m][r], h->max[m][r], 0, 0, &out[j]);
		else
			acc_sum(g, h->nn[m][r], g->is_float ? 0 : h->sum[m][r], g->is_float ? h->sumf[m][r] : 0, &out[j]);
	}
	return true;
}

static void
put_key(SiftV1Cube *c, const SiftSrcHist *h, uint32 r, int row)
{
	const bool	null = h->has_null && r == h->ngroups - 1;

	if (c->cal_key != NULL)
	{
		sift_v1_cal_key_put(c->cal_key, h, r, row);
		return;
	}
	if (!null)
		c->kvalid[row >> 6] |= UINT64CONST(1) << (row & 63);
	if (c->key_text[0])
	{
		const SiftSrcDict *d = h->dict;

		((SiftStrView *) c->kdata)[row] = null ? (SiftStrView) {0} : sift_vstr_make(d->bytes + d->off[h->key[r]], d->off[h->key[r] + 1] - d->off[h->key[r]]);
	}
	else
		((int64 *) c->kdata)[row] = null ? 0 : h->key[r];
}

static void
batch_start(SiftV1Cube *c, bool any_null)
{
	memset(c->kvalid, 0, sizeof(c->kvalid));
	c->kvec = (SiftVec) {.data = c->kdata,.dense = true,.type = c->key_text[0] ? SVEC_STRVIEW : SVEC_I64,.all_valid = !any_null,.valid = any_null ? c->kvalid : NULL};
}

/* The key vectors of the n groups of a histogram put since the last merge */
static SiftVec *const *
hist_keys(SiftV1Cube *c, int n)
{
	if (c->nkeys == 0)
		return NULL;
	c->kvec.len = (uint16) n;
	if (c->cal_key != NULL)
		c->kvp[0] = sift_v1_cal_key_vec(c->cal_key, n);
	return c->kvp;
}

/* The cells of h the tests of the histogram's dimension keep, each wholly inside them or outside: false when one with rows is cut by a bound */
static bool
cells_decided(SiftV1Cube *c, const SiftSrcHist *h)
{
	for (uint32 r = 0; r < h->ngroups; r++)
	{
		const bool	null = h->has_null && r == h->ngroups - 1;
		SiftV1Verdict v = SV1V_ALL;

		for (int i = 0; i < c->npred; i++)
			if (c->pred[i].leaf == c->hist_leaf)
				v = Min(v, null ? (sift_v1_pred_passes(&c->pred[i], h, r) ? SV1V_ALL : SV1V_NONE) : sift_v1_pred_cell(&c->pred[i], c->cal_type, h, r));
		if (v == SV1V_SOME && h->count[r] > 0)
			return false;
		c->cell_keep[r] = v == SV1V_ALL;
	}
	return true;
}

/* The cells of h give the key of their rows: a calendar key of a grain it keeps over, a date key the first day of a cell of days */
static bool
cells_key(const SiftV1Cube *c, const SiftSrcHist *h)
{
	if (c->cal_key != NULL)
		return sift_v1_cal_key_fits(c->cal_key, h);
	return c->nkeys == 0 || h->grain == SIFT_CAL_DAY;
}

/* The groups of the unit by the histogram's dimension: its histogram, else its calendar cells when the tests decide every one of them */
static bool
unit_groups(SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, SiftSrcHist *h)
{
	if (rd->unit_hist(src, unit, c->hist_leaf, &c->need, h))
		return true;
	return OidIsValid(c->cal_type) && rd->unit_hist(src, unit, SIFT_SRC_CAL + c->hist_leaf, &c->need, h) && cells_key(c, h) && cells_decided(c, h);
}

static bool
fold_hist(SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	SiftSrcHist h;
	int			n = 0;

	if (!unit_groups(c, rd, src, unit, &h) || (c->hist_text && h.dict == NULL) || !bind_hist(c, &h))
		return false;
	for (uint32 r = 0; r < h.ngroups; r++)
		if (h.count[r] > 0 && !group_acc(c, &h, r, c->acc))
			return false;
	batch_start(c, h.has_null);
	for (uint32 r = 0; r < h.ngroups; r++)
	{
		if (h.count[r] == 0 || (c->by_group && !group_kept(c, &h, r)))
			continue;
		if (c->nkeys > 0)
			put_key(c, &h, r, n);
		group_acc(c, &h, r, &c->acc[n * c->spec->naggs]);
		if (++n == SIFT_VEC_LEN)
		{
			merge(c, hist_keys(c, n), n);
			batch_start(c, h.has_null);
			n = 0;
		}
	}
	if (n > 0)
		merge(c, hist_keys(c, n), n);
	for (int j = 0; j < c->spec->naggs; j++)
		if (c->agg[j].a->kind == SV6_COUNT_DISTINCT)
			sift_v1_hist_merge_dist(c->group, j, &h, c->key_text[0], c->kdata, c->by_group ? group_kept : NULL, c);
	return true;
}

/* A unit from the rollup of its rows: the tuples the tests keep fold into the groups of their keys */
static bool
fold_rollup(SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	SiftSrcHist h;
	int			n = 0;

	if (c->rollup == NULL || !sift_v1_rollup_read(c->rollup, rd, src, unit, &h) || !bind_hist(c, &h))
		return false;
	for (uint32 t = 0; t < h.ngroups; t++)
		if (h.count[t] > 0 && !group_acc(c, &h, t, c->acc))
			return false;
	for (uint32 t = 0; t < h.ngroups; t++)
	{
		if (h.count[t] == 0 || !sift_v1_rollup_kept(c->rollup, t))
			continue;
		sift_v1_rollup_put(c->rollup, t, n);
		group_acc(c, &h, t, &c->acc[n * c->spec->naggs]);
		if (++n == SIFT_VEC_LEN)
		{
			merge(c, c->nkeys > 0 ? sift_v1_rollup_keys(c->rollup, n) : NULL, n);
			n = 0;
		}
	}
	if (n > 0)
		merge(c, c->nkeys > 0 ? sift_v1_rollup_keys(c->rollup, n) : NULL, n);
	return true;
}

/*
 * A unit the test `leaf IS NULL` leaves undecided, from its histogram net of the exceptions it keeps of the leaf, when no other test is undecided (alone); false: it keeps none, or they do not
 * answer the statement, and the unit is read (counted)
 */
static bool
fold_except(SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, int leaf, bool alone)
{
	bool		done;

	if (leaf < 0 || rd->unit_except == NULL || !rd->unit_except(src, unit, leaf))
		return false;
	done = alone && c->dist_leaf < 0 && fold_hist(c, rd, src, unit);
	rd->unit_except(src, unit, -1);
	c->stats->grans_preagg_net += done;
	c->stats->grans_except_read += !done;
	return done;
}

/* ---- the unit ---- */

static bool
offer(void *arg, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	SiftV1Cube *c = arg;
	SiftSrcUnit live = *unit;
	SiftV1Verdict v;
	int			except;
	bool		alone,
				done;

	if (!sift_v6_group_takes_cells(c->group) || (live.nrow = rd->unit_rows(src, unit)) < 0)
		return false;
	if ((v = unit_verdict(c, rd, src, &live, &except, &alone)) == SV1V_NONE)
		done = true;
	else if (c->nkeys > 1)
		done = fold_rollup(c, rd, src, &live);
	else if (v == SV1V_SOME)
		done = fold_rollup(c, rd, src, &live) || fold_except(c, rd, src, &live, except, alone);
	else
		done = (c->nkeys == 0 && !c->filtered && (c->dist_leaf >= 0 || !c->by_group) ? fold_global(c, rd, src, &live) : fold_hist(c, rd, src, &live)) ||
			fold_rollup(c, rd, src, &live);
	c->stats->grans_preagg += done ? Max(unit->nsu, 1) : 0;
	c->stats->grans_preagg_super += done && unit->nsu > 0;
	c->stats->grans_preagg_delta += done && live.nrow < unit->nrow;
	return done;
}

/* ---- the statement ---- */

/* COUNT(DISTINCT leaf) is the dimension of a histogram: of the key, or of one leaf under no key */
static bool
bind_dist(SiftV1Cube *c, const SiftSrcLeaf *l)
{
	if (l->vtype != SCV_CODE && l->vtype != SCV_I32 && l->vtype != SCV_I64)
		return false;
	if (c->nkeys > 0)
		return l->id == c->key_leaf[0] && c->cal_key == NULL;
	if (c->dist_leaf >= 0 && c->dist_leaf != l->id)
		return false;
	c->dist_leaf = l->id;
	c->dist_text = l->vtype == SCV_CODE;
	return true;
}

/* An attribute of the clause is no column of host (the presence of a key) or an index expression (a path of a document): no column of the heap holds it */
static bool
on_expression(Node *node, Relation host)
{
	const Var  *v = (const Var *) node;

	if (node != NULL && IsA(node, Var))
		return v->varattno < 1 || v->varattno > RelationGetNumberOfAttributes(host) ||
			(host->rd_index != NULL && host->rd_index->indkey.values[v->varattno - 1] == 0);
	return expression_tree_walker(node, on_expression, host);
}

/* The conjuncts of the FILTER of g as tests of one leaf each; false for a clause the groups of a histogram do not decide, or a FILTER of COUNT(DISTINCT) */
static bool
bind_filter(SiftV1Cube *c, Relation host, const SiftSrcLayout *lay, CubeAgg *g)
{
	List	   *clause = list_nth(c->spec->filter, g->a->fclause - 1);
	const List *quals;

	if (g->a->kind == SV6_COUNT_DISTINCT || on_expression((Node *) clause, host))
		return false;
	quals = sift_srcfold_heap_quals(host, clause);
	g->f = palloc0(Max(list_length(quals), 1) * sizeof(SiftV1Pred));
	foreach_ptr(Node, q, (List *) quals)
		if (!sift_v1_pred_of(host, lay, q, &g->f[g->nf++]))
			return false;
	c->filtered = true;
	return true;
}

static bool
bind_aggs(SiftV1Cube *c, Relation host, const SiftSrcLayout *lay)
{
	c->dist_leaf = -1;
	for (int j = 0; j < c->spec->naggs; j++)
	{
		const SiftV6Agg *a = &c->spec->agg[j];
		const SiftSrcLeaf *l = a->kind == SV6_COUNT_STAR ? NULL : sift_v1_att_leaf(lay, a->arg.attno);
		const SiftV6Class cls = sift_v6_class(a->arg.typid);
		CubeAgg    *g = &c->agg[j];

		*g = (CubeAgg) {.a = a,.leaf = l == NULL ? -1 : l->id,.is_float = cls == SV6C_F64,.slot = -1};
		g->linear = sift_v6_agg_linear(c->spec, a, &g->lin);
		if (a->fclause > 0 && !bind_filter(c, host, lay, g))
			return false;
		if (a->kind != SV6_COUNT_STAR && (!leaf_measure(l, a) || (cls == SV6C_F64) != (l->vtype == SCV_F64) || (cls == SV6C_TEXT && l->vtype != SCV_CODE)))
			return false;
		if (a->kind == SV6_COUNT_DISTINCT)
		{
			if (!bind_dist(c, l))
				return false;
			continue;
		}
		if ((g->linear && !sift_v1_leaf_int(l)) || ((a->kind == SV6_SUM || a->kind == SV6_AVG) && cls == SV6C_F64))
			return false;
		if (c->nkeys > 0 && l != NULL && l->id == c->key_leaf[0] && a->kind != SV6_COUNT)
			return false;
		if ((a->kind == SV6_MIN || a->kind == SV6_MAX || a->kind == SV6_SUM || a->kind == SV6_AVG) && cls == SV6C_TEXT)
			return false;
		if (l != NULL && l->id != c->key_leaf[0])
		{
			int			m = 0;

			while (m < c->need.nmeas && c->need.meas[m].leaf != l->id)
				m++;
			if (m == SIFT_SRC_MAX_MEAS)
				return false;
			c->need.meas[m].leaf = l->id;
			c->need.meas[m].sums |= a->kind == SV6_SUM || a->kind == SV6_AVG;
			c->need.meas[m].minmax |= a->kind == SV6_MIN || a->kind == SV6_MAX || g->linear;
			c->need.nmeas = Max(c->need.nmeas, m + 1);
		}
	}
	return true;
}

/* The keys as leaves the cubes hold, or the one key as a calendar key */
static bool
bind_keys(SiftV1Cube *c, const SiftSrcLayout *lay, MemoryContext cx)
{
	if (c->nkeys == 1 && !sift_v6_key_cubeable(c->spec, 0))
		return (c->cal_key = sift_v1_cal_key_open(cx, c->spec, lay, &c->key_leaf[0])) != NULL;
	for (int i = 0; i < c->nkeys; i++)
	{
		const SiftV6Col *k = &c->spec->key[i];
		const SiftSrcLeaf *l = sift_v1_att_leaf(lay, k->attno);

		c->key_text[i] = sift_v6_class(k->typid) == SV6C_TEXT;
		if (!sift_v6_key_cubeable(c->spec, i) || !sift_v1_leaf_plain(l) || sift_v6_class(k->typid) == SV6C_BOOL ||
			(c->key_text[i] ? l->vtype != SCV_CODE : (l->vtype != SCV_I32 && l->vtype != SCV_I64)))
			return false;
		c->key_leaf[i] = l->id;
	}
	return true;
}

/* The tests of the quals of the scan and of the clauses no scan key says (SiftV6Spec.vqual, a text test of a sift_table) */
static bool
bind_preds(SiftV1Cube *c, Relation host, const SiftSrcLayout *lay, const List *quals)
{
	c->pred = palloc0(Max(list_length(quals) + list_length(c->spec->vqual), 1) * sizeof(SiftV1Pred));
	foreach_ptr(Node, q, list_concat_copy(quals, c->spec->vqual))
		if (!sift_v1_pred_of(host, lay, q, &c->pred[c->npred++]))
			return false;
	return true;
}

static int
first_filter_leaf(const SiftV1Cube *c)
{
	for (int j = 0; j < c->spec->naggs; j++)
		if (c->agg[j].nf > 0)
			return c->agg[j].f[0].leaf;
	return -1;
}

/* Every FILTER tests the dimension the units are folded from, under no COUNT(DISTINCT) of a statement without a key (whose values come from the zone of the unit) */
static bool
filters_on_hist(const SiftV1Cube *c)
{
	if (c->filtered && c->nkeys == 0 && c->dist_leaf >= 0)
		return false;
	for (int j = 0; j < c->spec->naggs; j++)
		for (int i = 0; i < c->agg[j].nf; i++)
			if (c->agg[j].f[i].leaf != c->hist_leaf)
				return false;
	return true;
}

/*
 * The dimension a unit is folded from: the key, else the leaf of COUNT(DISTINCT), else of the first FILTER, else the leaf of the first test, whose groups pass it or not. A calendar key, a date
 * key, and the first test of a date or timestamp leaf with no key, may be answered from the calendar cells of a unit too, under no FILTER or COUNT(DISTINCT)
 */
static void
hist_of(SiftV1Cube *c, const SiftSrcLayout *lay, MemoryContext cx)
{
	Oid			t;

	c->hist_leaf = c->nkeys > 0 ? c->key_leaf[0] : c->dist_leaf >= 0 ? c->dist_leaf : c->filtered ? first_filter_leaf(c) : c->npred > 0 ? c->pred[0].leaf : -1;
	c->hist_text = c->hist_leaf >= 0 && lay->leaf[c->hist_leaf].vtype == SCV_CODE;
	t = c->hist_leaf >= 0 && c->hist_leaf < SIFT_SRC_CAL ? lay->leaf[c->hist_leaf].typid : InvalidOid;
	if (c->dist_leaf >= 0 || c->filtered || (c->cal_key == NULL && (c->nkeys > 0 ? t != DATEOID : c->npred == 0 || (t != DATEOID && t != TIMESTAMPOID && t != TIMESTAMPTZOID))))
		return;
	c->cal_type = t;
	c->cell_keep = MemoryContextAlloc(cx, (SIFT_PAGG_CAL_GROUPS + 1) * sizeof(bool));
}

/*
 * The rollup of the rows a unit is folded from when no histogram of one leaf holds the statement (two keys, tests of other leaves), under no FILTER, COUNT(DISTINCT), calendar key or
 * aggregate of a key (whose NULL rows the tuples of other dims do not tell apart)
 */
static void
rollup_of(SiftV1Cube *c, MemoryContext cx)
{
	if (c->filtered || c->dist_leaf >= 0 || c->cal_key != NULL)
		return;
	for (int j = 0; j < c->spec->naggs; j++)
		for (int k = 0; k < c->nkeys; k++)
			if (c->agg[j].leaf == c->key_leaf[k])
				return;
	c->rollup = sift_v1_rollup_open(cx, c->key_leaf, c->key_text, c->nkeys, c->pred, c->npred, &c->need);
}

SiftV1UnitSink *
sift_v1_cube_begin(const SiftSrcFoldArgs *a, const SiftV1Scan *scan, MemoryContext cx)
{
	const SiftV1ReaderOps *rd;
	const SiftSrcLayout *lay;
	SiftV1Cube *c;

	if (a->emit != NULL || a->star != NULL || a->gate != NULL || a->stats == NULL || a->spec->nkeys > CUBE_KEYS || !sift_v6_spec_cube_aggs(a->spec) ||
		(scan != NULL && (scan->keep != NULL || scan->bridge != NULL || scan->ntypes > 0 || scan->tid_range)))
		return NULL;
	rd = sift_v1_reader_of(a->host->rd_index == NULL);
	lay = rd->layout(a->host);
	c = MemoryContextAllocZero(cx, sizeof(SiftV1Cube));
	c->spec = a->spec;
	c->group = a->group;
	c->stats = a->stats;
	c->nkeys = a->spec->nkeys;
	c->key_leaf[0] = -1;
	c->agg = MemoryContextAlloc(cx, a->spec->naggs * sizeof(CubeAgg));
	c->acc = MemoryContextAlloc(cx, (Size) Max(SIFT_VEC_LEN, 1) * a->spec->naggs * sizeof(V6Acc));
	c->kdata = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	c->kvp[0] = &c->kvec;
	c->sink = (SiftV1UnitSink) {.offer = offer,.arg = c};
	if (!bind_keys(c, lay, cx) || !bind_aggs(c, a->host, lay) || !bind_preds(c, a->host, lay, a->quals))
		return NULL;
	hist_of(c, lay, cx);
	rollup_of(c, cx);
	return filters_on_hist(c) && (c->nkeys < 2 || c->rollup != NULL) ? &c->sink : NULL;
}

void
sift_v1_cube_set_group(SiftV1UnitSink *sink, SiftV6Group *g)
{
	((SiftV1Cube *) sink->arg)->group = g;
}
