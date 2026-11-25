/*
 * col/f2/f2_v6_proj.c - the match of a SiftAggV6 query against the projections of a relation and the cost of each source (col/f2/f2_v6_proj.h).
 */
#include "core/host.h"

#include <math.h>

#include "col/f2/f2_v6_proj.h"

static bool
same_expr(const SiftProjKey *a, const SiftProjKey *b)
{
	return a->leaf == b->leaf && a->fn == b->fn && a->unit == b->unit;
}

static bool
same_order(const SiftProjKey *a, const SiftProjKey *b)
{
	return same_expr(a, b) && a->desc == b->desc && a->nulls_first == b->nulls_first;
}

static const SiftProjKey *
find_key(const SiftProjSpec *p, int16 leaf, uint8 fn, uint8 unit)
{
	for (int i = 0; i < p->nkeys; i++)
		if (p->key[i].leaf == leaf && p->key[i].fn == fn && p->key[i].unit == unit)
			return &p->key[i];
	return NULL;
}

static bool
same_filter(const SiftProjMeasure *a, const SiftProjMeasure *b)
{
	if (a->nfilter != b->nfilter)
		return false;
	for (int i = 0; i < a->nfilter; i++)
		if (!sift_proj_spec_pred_equal(&a->filter[i], &b->filter[i]))
			return false;
	return true;
}

/* The measure of the projection that is fn of leaf with the filter given (none: an unfiltered one) */
static int
find_measure(const SiftProjSpec *p, const SiftProjMeasure *want, uint8 fn, bool filtered)
{
	for (int i = 0; i < p->nmeas; i++)
		if (p->meas[i].fn == fn && p->meas[i].leaf == want->leaf && (filtered ? same_filter(&p->meas[i], want) : p->meas[i].nfilter == 0))
			return i;
	return -1;
}

/* Every column a FILTER tests is a group column of the projection kept as it is: the filter can be tested on the stored rows */
static bool
filter_on_keys(const SiftProjSpec *p, const SiftProjMeasure *m)
{
	for (int f = 0; f < m->nfilter; f++)
		if (find_key(p, m->filter[f].leaf, SPF_COLUMN, SPU_NONE) == NULL)
			return false;
	return true;
}

/*
 * The measure of the projection a measure of the query is made of: a sum or an avg keeps the values it is of, so it serves count(x) as well. A FILTER is met by a measure that holds the same
 * filter (baked: its cells are already of the rows that pass) or, when every column it tests is a group column of the projection, by the unfiltered measure with the filter tested on each stored row.
 */
static bool
source_measure(const SiftProjSpec *p, const SiftProjMeasure *m, int *src, uint8 *mode)
{
	static const uint8 from_sum[] = {SPM_SUM, SPM_AVG};
	uint8		fns[3] = {m->fn, SPM_SUM, SPM_AVG};
	int			nfns = m->fn == SPM_COUNT ? 3 : 1;

	*mode = SPFM_NONE;
	if (m->fn == SPM_COUNT_DISTINCT)
	{
		*src = -1;
		*mode = m->nfilter > 0 ? SPFM_ON_KEY : SPFM_NONE;
		return find_key(p, m->leaf, SPF_COLUMN, SPU_NONE) != NULL && (m->nfilter == 0 || filter_on_keys(p, m));
	}
	if (m->fn != SPM_COUNT && m->fn != SPM_COUNT_STAR && m->fn != SPM_MIN && m->fn != SPM_MAX)
	{
		fns[0] = from_sum[0];
		fns[1] = from_sum[1];
		nfns = 2;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		bool		filtered = pass == 0 && m->nfilter > 0;

		if (pass == 1 && (m->nfilter == 0 || !filter_on_keys(p, m)))
			break;
		for (int i = 0; i < nfns; i++)
			if ((*src = find_measure(p, m, fns[i], filtered)) >= 0)
			{
				*mode = m->nfilter == 0 ? SPFM_NONE : (filtered ? SPFM_BAKED : SPFM_ON_KEY);
				return true;
			}
		if (m->fn == SPM_COUNT_STAR && !filtered)
		{
			*src = -1;
			*mode = m->nfilter == 0 ? SPFM_NONE : SPFM_ON_KEY;
			return true;
		}
	}
	return false;
}

/* The query's predicate on a column the projection keeps as date_trunc: the bucket test the bound maps to, when the bound is an edge of a bucket */
static bool
bucket_pred(const SiftProjSpec *p, const SiftProjColumns *cols, const SiftV6ProjPred *qp, SiftProjRollup *map, int i)
{
	const SiftProjPred *c = qp->cell;

	if (c == NULL || qp->fn != SPF_COLUMN || c->bytes || c->nvals != 1 || cols == NULL || c->op > SPO_GT)
		return false;
	for (int k = 0; k < p->nkeys; k++)
	{
		SiftProjOp	op;
		int64		val;

		if (p->key[k].leaf == qp->leaf && p->key[k].fn == SPF_TRUNC &&
			sift_proj_const_aligned((SiftProjTime) cols->col[qp->leaf].time, (SiftProjOp) c->op, c->vals[0].i, (SiftProjUnit) p->key[k].unit, &op, &val))
		{
			map->pred_use[i] = SPPU_MAPPED;
			map->pred_op[i] = op;
			map->pred_val[i] = val;
			return true;
		}
	}
	return false;
}

bool
sift_v6_proj_where_covers(const SiftProjSpec *p, const SiftV6ProjQuery *q)
{
	for (int w = 0; w < p->nwhere; w++)
	{
		bool		covered = false;

		for (int i = 0; i < q->npred && !covered; i++)
			covered = q->pred[i].fn == SPF_COLUMN && q->pred[i].cell != NULL && sift_proj_pred_implies(q->pred[i].cell, &p->where[w]);
		if (!covered)
			return false;
	}
	return true;
}

/* A predicate the projection's WHERE states exactly holds of every stored row, and its column need not be one of the projection's */
static bool
stated_by_where(const SiftProjSpec *p, const SiftV6ProjPred *qp)
{
	for (int w = 0; w < p->nwhere; w++)
		if (qp->fn == SPF_COLUMN && qp->cell != NULL && sift_proj_pred_is_spec(qp->cell, &p->where[w]))
			return true;
	return false;
}

bool
sift_v6_proj_match_agg(const SiftProjSpec *p, const SiftProjColumns *cols, const SiftV6ProjQuery *q, SiftProjRollup *map)
{
	if (p->kind != SIFT_PROJ_AGG || q->wants_rows || !sift_v6_proj_where_covers(p, q))
		return false;
	memset(map, 0, sizeof(*map));
	map->nkeys = q->ngroup;
	map->nmeas = q->nagg;
	for (int i = 0; i < q->ngroup; i++)
	{
		const SiftProjKey *k = &q->group[i];
		const SiftProjKey *pk = find_key(p, k->leaf, k->fn, k->unit);

		if (pk == NULL && k->fn == SPF_TRUNC)
			for (int j = 0; j < p->nkeys && pk == NULL; j++)
				if (p->key[j].leaf == k->leaf && (p->key[j].fn == SPF_COLUMN || sift_proj_unit_refines((SiftProjUnit) p->key[j].unit, (SiftProjUnit) k->unit)))
					pk = &p->key[j];
		if (pk == NULL)
			return false;
		map->src_key[i] = (int) (pk - p->key);
		map->unit[i] = (k->fn == SPF_TRUNC && !(pk->fn == SPF_TRUNC && pk->unit == k->unit)) ? k->unit : SPU_NONE;
	}
	for (int i = 0; i < q->npred; i++)
	{
		const SiftV6ProjPred *qp = &q->pred[i];

		if (qp->op == SV6P_OTHER)
			return false;
		if (stated_by_where(p, qp))
			map->pred_use[i] = SPPU_SKIP;
		else if (find_key(p, qp->leaf, qp->fn, qp->unit) == NULL && !bucket_pred(p, cols, qp, map, i))
			return false;
	}
	for (int j = 0; j < q->nagg; j++)
		if (!source_measure(p, &q->agg[j], &map->src_meas[j], &map->filter_mode[j]))
			return false;
	return true;
}

SiftV6SortUse
sift_v6_sort_use(const SiftProjSpec *order, const SiftV6ProjQuery *q)
{
	SiftV6SortUse use = {.prefix_sel = 1.0};
	int			eqs = 0,
				j = 0;
	bool		ranged = false;

	for (int k = 0; k < order->nkeys && !ranged; k++)
	{
		const SiftV6ProjPred *hit = NULL;

		for (int i = 0; i < q->npred && hit == NULL; i++)
			if (q->pred[i].leaf == order->key[k].leaf && q->pred[i].fn == order->key[k].fn && q->pred[i].unit == order->key[k].unit &&
				(q->pred[i].op == SV6P_EQ || q->pred[i].op == SV6P_RANGE || (q->pred[i].op == SV6P_IN && q->pred[i].nvals <= SIFT_PROJ_IN_SEEK_MAX)))
				hit = &q->pred[i];
		if (hit == NULL)
			break;
		use.prefix++;
		use.prefix_sel *= hit->sel;
		if (hit->op == SV6P_EQ && eqs == k)
			eqs++;
		else if (hit->op == SV6P_IN && eqs == k && !use.in_merge && hit->nvals > 0)
		{
			eqs++;
			use.in_merge = true;
			use.in_key_idx = k;
		}
		else
			ranged = true;
	}
	use.order = q->norder > 0;
	for (int i = 0; i < q->norder && use.order; i++)
	{
		while (j < eqs && !same_order(&q->order[i], &order->key[j]))
			j++;
		use.order = j < order->nkeys && same_order(&q->order[i], &order->key[j]);
		j++;
	}
	return use;
}

bool
sift_v6_proj_match_sort(const SiftProjSpec *p, const SiftProjColumns *cols, const SiftV6ProjQuery *q, SiftV6SortUse *use)
{
	if (p->kind != SIFT_PROJ_SORT || !q->wants_rows || !sift_v6_proj_where_covers(p, q))
		return false;
	for (int i = 0; i < q->nread; i++)
	{
		int16		leaf = q->read[i];
		bool		kept = p->all_columns && cols->col[leaf].usable;

		for (int k = 0; k < p->nkeys; k++)
			kept |= p->key[k].leaf == leaf;
		for (int k = 0; k < p->ninclude; k++)
			kept |= p->include[k] == leaf;
		if (!kept)
			return false;
	}
	*use = sift_v6_sort_use(p, q);
	return use->prefix > 0 || use->order;
}

int
sift_v6_proj_pick_sort(const SiftProjSpec *const *spec, int n, const SiftProjColumns *cols, const SiftV6ProjQuery *q)
{
	int			best = -1;
	SiftV6SortUse best_use = {0};

	for (int i = 0; i < n; i++)
	{
		SiftV6SortUse use;

		if (!sift_v6_proj_match_sort(spec[i], cols, q, &use) || (q->norder > 0 && !use.order))
			continue;
		if (best < 0 || (use.order && !best_use.order) || (use.order == best_use.order && use.prefix > best_use.prefix))
		{
			best = i;
			best_use = use;
		}
	}
	return best;
}

/* ---- cost ---- */

typedef struct Stream
{
	double		rows;
	double		bytes;
	double		col_frac;		/* the part of the bytes the query reads */
	double		prune;			/* the part of the rows the sort order lets the scan reach */
	bool		order_ok;		/* the stream comes in the order the query asks for */
	int			width;			/* the columns each row is tested and folded on */
} Stream;

static double
overall_sel(const SiftV6ProjQuery *q)
{
	double		sel = 1.0;

	for (int i = 0; i < q->npred; i++)
		sel *= q->pred[i].sel;
	return sel;
}

static double
stream_cost(const SiftV6ProjInput *in, const Stream *s)
{
	const SiftV6ProjQuery *q = in->q;
	const SiftV6ProjCost *c = &in->cost;
	double		sel_in = Min(overall_sel(q) / Max(s->prune, 1e-9), 1.0),
				scanned = s->rows * s->prune,
				cost,
				out;

	if (q->limit >= 0 && q->wants_rows && s->order_ok)
		scanned = Min(scanned, Max((double) q->limit / Max(sel_in, 1e-9), 1.0));
	out = scanned * sel_in;
	cost = c->seq_page * (s->bytes * s->col_frac * (s->rows > 0 ? scanned / s->rows : 0.0) / BLCKSZ) + c->cpu_tuple * scanned + c->cpu_operator * scanned * (s->width + 1);
	if (q->norder > 0 && !s->order_ok)
		cost += c->cpu_operator * (q->limit >= 0 ? out * log2((double) q->limit + 2) : 2 * out * log2(out + 2));
	return cost;
}

static double
col_frac(const SiftV6ProjQuery *q, int ncols)
{
	int			touched = q->wants_rows ? q->nread : q->ngroup + q->nagg + q->npred;

	return Min(1.0, (double) Max(touched, 1) / Max(ncols, 1));
}

static int
width_of(const SiftV6ProjQuery *q)
{
	return q->npred + q->ngroup + q->nagg;
}

static const SiftPartDesc *
proj_part(const SiftProjParts *p, uint64 gen)
{
	int			lo = 0,
				hi = p->nparts - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (p->parts[mid].part_gen == gen)
			return &p->parts[mid];
		if (p->parts[mid].part_gen < gen)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

static void
add_base(Stream *s, const SiftPartDesc *d)
{
	s->rows += (double) (d->nrows - d->ndead);
	s->bytes += (double) d->nbytes;
}

static Stream
base_stream(const SiftV6ProjInput *in, const SiftV6SortUse *use)
{
	Stream		s = {.col_frac = col_frac(in->q, in->ncols),.prune = in->base_order != NULL ? use->prefix_sel : 1.0,.order_ok = use->order,.width = width_of(in->q)};

	return s;
}

/* The cost of reading the base parts the projection does not stand for (all of them without one); *rows: what they hold */
static double
base_cost(const SiftV6ProjInput *in, const SiftProjPartServe *serve, const SiftV6SortUse *use, double *rows)
{
	Stream		s = base_stream(in, use);

	for (int i = 0; i < in->nbase; i++)
		if (serve == NULL || serve[i].serve == SPS_BASE)
			add_base(&s, &in->base[i]);
	*rows = s.rows;
	return stream_cost(in, &s);
}

static double
agg_cost(const SiftV6ProjInput *in, const SiftV6ProjCand *c, const SiftV6SortUse *base_use, int *served)
{
	const SiftV6ProjQuery *q = in->q;
	SiftV6SortUse pu = sift_v6_sort_use(c->spec, q);
	double		cost = 0,
				base_rows;

	*served = 0;
	for (int i = 0; i < in->nbase; i++)
	{
		const SiftPartDesc *pp = c->serve[i].serve == SPS_BASE ? NULL : proj_part(c->parts, c->serve[i].proj_gen);
		Stream		s = {.col_frac = 1.0,.prune = pu.prefix_sel,.order_ok = true,.width = q->ngroup + q->nagg};

		if (pp == NULL)
			continue;
		s.rows = (double) pp->nrows;
		s.bytes = (double) pp->nbytes;
		cost += stream_cost(in, &s) + in->cost.cpu_operator * s.rows * s.prune * q->ngroup;
		if (c->serve[i].serve == SPS_OVERLAY)
			cost += in->cost.random_page * Min((double) in->base[i].ndead, (double) in->base[i].nbytes / BLCKSZ + 1) + in->cost.cpu_tuple * (double) in->base[i].ndead;
		(*served)++;
	}
	return cost + base_cost(in, c->serve, base_use, &base_rows);
}

static double
sort_cost(const SiftV6ProjInput *in, const SiftV6ProjCand *c, const SiftV6SortUse *use, const SiftV6SortUse *base_use, int *served)
{
	const SiftV6ProjQuery *q = in->q;
	double		cost = 0,
				served_rows = 0,
				base_rows;

	*served = 0;
	for (int i = 0; i < in->nbase; i++)
	{
		const SiftPartDesc *pp = c->serve[i].serve == SPS_BASE ? NULL : proj_part(c->parts, c->serve[i].proj_gen);
		Stream		s = {.col_frac = Min(1.0, (double) Max(q->nread, 1) / Max(c->spec->all_columns ? in->ncols : c->spec->nkeys + c->spec->ninclude, 1)),
		.prune = use->prefix_sel,.order_ok = use->order,.width = width_of(q)};

		if (pp == NULL)
			continue;
		s.rows = (double) pp->nrows;
		s.bytes = (double) pp->nbytes;
		cost += stream_cost(in, &s);
		served_rows += s.rows * s.prune;
		(*served)++;
	}
	cost += base_cost(in, c->serve, base_use, &base_rows);
	if (base_rows > 0 && served_rows > 0 && q->norder > 0)
		cost += in->cost.cpu_operator * (served_rows + base_rows);
	return cost;
}

SiftV6ProjChoice
sift_v6_proj_choose(const SiftV6ProjInput *in, const SiftProjColumns *cols)
{
	SiftV6ProjChoice ch = {.cand = -1};
	SiftV6SortUse base_use = in->base_order != NULL ? sift_v6_sort_use(in->base_order, in->q) : (SiftV6SortUse) {.prefix_sel = 1.0};
	double		best,
				rows;

	base_use.order &= !base_use.in_merge;
	ch.base_cost = base_cost(in, NULL, &base_use, &rows);
	best = ch.base_cost;
	ch.cost = best;
	for (int k = 0; k < in->ncand && k < SIFT_PROJ_MAX; k++)
	{
		const SiftV6ProjCand *c = &in->cand[k];
		SiftProjRollup map;
		SiftV6SortUse use = {0};
		int			served = 0;
		double		cost;

		if (c->spec->kind == SIFT_PROJ_AGG)
		{
			ch.covers[k] = sift_v6_proj_match_agg(c->spec, cols, in->q, &map);
			if (!ch.covers[k])
				continue;
			cost = agg_cost(in, c, &base_use, &served);
		}
		else
		{
			ch.covers[k] = sift_v6_proj_match_sort(c->spec, cols, in->q, &use);
			if (!ch.covers[k])
				continue;
			cost = sort_cost(in, c, &use, &base_use, &served);
		}
		if (served > 0 && cost < best)
		{
			best = cost;
			ch.cand = k;
			ch.cost = cost;
			ch.served = served;
			if (c->spec->kind == SIFT_PROJ_AGG)
				ch.rollup = map;
		}
	}
	if (ch.cand >= 0)
		for (int i = 0; i < in->nbase; i++)
			if (in->cand[ch.cand].serve[i].serve == SPS_BASE)
				ch.fallback[in->cand[ch.cand].serve[i].why]++;
	return ch;
}
