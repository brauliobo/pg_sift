/*
 * col/f2/f2_v6_preagg.c - SiftAggV6 answering granules from the cubes of SEC_PREAGG version 2 (col/f2/f2_v6_preagg.h).
 */
/* check-memory: allow-allocs every array here follows the keys, aggregates and predicates of one statement or the groups of one granule, not the rows of the table */
#include "postgres.h"

#include <math.h>

#include "access/visibilitymap.h"
#include "utils/float.h"
#include "utils/memutils.h"

#include "agg/agg.h"
#include "col/f2/f2_v6_preagg.h"
#include "agg/v6/group_int.h"
#include "agg/v6/v6_cube.h"
#include "col/f2/f2.h"
#include "col/f2/f2_pred.h"
#include "col/f2/preagg_delta.h"
#include "core/memtrack.h"

#define PA_VM_SPAN_MAX	4096

typedef enum AggSrc
{
	ASRC_ROWS,					/* count(*) */
	ASRC_MEAS,					/* a measure of the cube */
	ASRC_DIM,					/* count(dimension): the rows whose dimension is not NULL */
	ASRC_DIST					/* count(DISTINCT col): the set of the cube */
} AggSrc;

typedef struct AggBind
{
	AggSrc		src;
	int			idx;
	int			leaf;			/* the column of the part the aggregate reads, -1 for count(*) */
} AggBind;

typedef struct KeyBatch
{
	int			n;
	SiftVec		vec[SIFT_V6_MAX_KEYS];
	SiftVec    *vp[SIFT_V6_MAX_KEYS];
	void	   *data[SIFT_V6_MAX_KEYS];
	uint64		valid[SIFT_V6_MAX_KEYS][SIFT_VEC_WORDS];
	bool		any_null[SIFT_V6_MAX_KEYS];
} KeyBatch;

/* What the statement makes of the cubes of one part: the cube that holds it and where each key, aggregate and predicate is in it */
typedef struct PartBind
{
	uint64		gen;
	bool		bound;
	bool		usable;
	const SiftPreaggPart *pp;
	int			cube;
	int			key_dim[SIFT_V6_MAX_KEYS];
	AggBind    *agg;
	int		   *pred_dim;		/* the dimension a predicate is on, -1: none, or the cube cannot test it */
	bool	   *pred_ok;
	SiftGranuleDict dict[SIFT_PREAGG_MAX_DIMS];
	const SiftPartDicts *dicts;
	SiftPreaggReads reads;		/* what the aggregates read of the measures */
	bool		has_dist;
} PartBind;

struct SiftV6Preagg
{
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6Vet  *vet;
	SiftV6ScanStats *stats;
	int			key_leaf[SIFT_V6_MAX_KEYS];
	bool		key_text[SIFT_V6_MAX_KEYS];
	int		   *agg_leaf;		/* of each aggregate, -1 for count(*) */
	MemoryContext pcx;			/* what the part in hand is bound to */
	MemoryContext gcx;			/* what a granule reads */
	PartBind	pb;
	KeyBatch   *kb;
	SiftVec		dvec;
	void	   *ddata;
	int64	   *scratch;
};

static bool
def_plain(const SiftColDef *d)
{
	return d->parent < 0 && d->lane == 0 && !sift_col_def_is_wide(d) && !(d->flags & SIFT_COLD_LOSSY) && (d->path == NULL || d->path[0] == '\0');
}

/* The column of the part that holds a leaf of the scan, -1: none */
static int
part_leaf(const SiftF2GranOffer *o, int leaf)
{
	return o->phys == NULL ? leaf : o->phys[leaf];
}

SiftV6Preagg *
sift_v6_preagg_begin(const SiftV6Spec *spec, SiftV6Group *g, const int *first, SiftV6Vet *vet, SiftV6ScanStats *stats)
{
	SiftV6Preagg *pa;
	bool		dist = false;

	if (!sift_agg_preagg || !sift_v6_spec_cubeable(spec))
		return NULL;
	pa = palloc0(sizeof(SiftV6Preagg));
	pa->spec = spec;
	pa->group = g;
	pa->vet = vet;
	pa->stats = stats;
	pa->agg_leaf = palloc(spec->naggs * sizeof(int));
	for (int i = 0; i < spec->nkeys; i++)
	{
		pa->key_leaf[i] = first[spec->key[i].attno];
		pa->key_text[i] = sift_v6_class(spec->key[i].typid) == SV6C_TEXT;
	}
	for (int j = 0; j < spec->naggs; j++)
	{
		pa->agg_leaf[j] = spec->agg[j].kind == SV6_COUNT_STAR ? -1 : first[spec->agg[j].arg.attno];
		dist |= spec->agg[j].kind == SV6_COUNT_DISTINCT;
	}
	pa->pcx = sift_mem_allocset_create(CurrentMemoryContext, "sift v6 preagg part");
	pa->gcx = sift_mem_allocset_create(CurrentMemoryContext, "sift v6 preagg granule");
	pa->kb = palloc0(sizeof(KeyBatch));
	for (int i = 0; i < spec->nkeys; i++)
		pa->kb->data[i] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	if (dist)
	{
		pa->ddata = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		pa->scratch = palloc(SIFT_PAG_DIST_BM_BITS * sizeof(int64));
	}
	return pa;
}

void
sift_v6_preagg_set_group(SiftV6Preagg *pa, SiftV6Group *g)
{
	pa->group = g;
}

void
sift_v6_preagg_end(SiftV6Preagg *pa)
{
	MemoryContextDelete(pa->pcx);
	MemoryContextDelete(pa->gcx);
}

/* ---- the cube of a part ---- */

static int
find_dim(const SiftPreaggCubeDesc *cd, int leaf, SiftPreaggDimKind kind)
{
	for (int d = 0; d < cd->ndims; d++)
		if (cd->dim_leaf[d] == leaf && cd->dim_kind[d] == kind)
			return d;
	return -1;
}

static int
find_meas(const SiftPreaggCubeDesc *cd, int leaf, bool is_float, bool numeric)
{
	for (int m = 0; m < cd->nmeasures; m++)
		if (cd->meas_leaf[m] == leaf && (cd->meas_kind[m] >= SIFT_PMEAS_F4) == is_float && (cd->meas_kind[m] == SIFT_PMEAS_NUM64) == numeric)
			return m;
	return -1;
}

static int
find_dist(const SiftPreaggCubeDesc *cd, int leaf)
{
	for (int k = 0; k < cd->ndistinct; k++)
		if (cd->dist_leaf[k] == leaf)
			return k;
	return -1;
}

/* A test the cube can make on the keys of a dimension */
static bool
pred_on_dim(const SiftColDef *d, const SiftColPred *p, SiftPreaggDimKind kind)
{
	if (!def_plain(d) || (kind != SIFT_PDIM_DICT32 ? d->store != SIFT_COLS_INT : (d->store != SIFT_COLS_STR || d->type != SIFT_COLT_TEXT)))
		return false;
	if (kind == SIFT_PDIM_NULLFLAG)
		return p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL;
	for (int j = 0; j < p->nvals; j++)
		if (p->vals[j].isnull || p->vals[j].overflow)
			return false;
	switch (p->op)
	{
		case SIFT_COLOP_ISNULL:
		case SIFT_COLOP_NOTNULL:
			return true;
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			return kind == SIFT_PDIM_INT64 || !sift_f2_collation_undecided(p);
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
			return kind == SIFT_PDIM_INT64;
		case SIFT_COLOP_PREFIX:
		case SIFT_COLOP_CONTAINS:
		case SIFT_COLOP_NOTCONTAINS:
		case SIFT_COLOP_SUFFIX:
			return kind == SIFT_PDIM_DICT32 && p->nvals == 1;
		default:
			return false;
	}
}

/* Where the aggregate is in the cube: false when it is not */
static bool
bind_agg(const SiftV6Preagg *pa, const SiftF2GranOffer *o, const SiftPreaggCubeDesc *cd, int j, AggBind *out)
{
	const SiftColSchema *schema = o->schema;
	const SiftV6Agg *a = &pa->spec->agg[j];
	const int	today = pa->agg_leaf[j],
				leaf = today < 0 ? today : part_leaf(o, today);
	const SiftV6Class cls = sift_v6_class(a->arg.typid);
	const bool	is_float = cls == SV6C_F64,
				numeric = a->arg.typid == NUMERICOID;
	int			m;

	if (a->kind == SV6_COUNT_STAR)
		*out = (AggBind) {ASRC_ROWS, 0, -1};
	else if (leaf < 0 || !def_plain(&schema->cols[today]))
		return false;
	else if (a->kind == SV6_COUNT_DISTINCT)
	{
		const int	k = find_dist(cd, leaf);

		if (k < 0 || (cls == SV6C_TEXT) != (schema->cols[today].store == SIFT_COLS_STR))
			return false;
		*out = (AggBind) {ASRC_DIST, k, leaf};
	}
	else if ((m = find_meas(cd, leaf, is_float, numeric)) >= 0)
	{
		if (numeric && cd->meas_kind[m] == SIFT_PMEAS_NUM64 && schema->cols[today].scale != a->arg.scale)
			return false;
		*out = (AggBind) {ASRC_MEAS, m, leaf};
	}
	else if (a->kind == SV6_COUNT && (m = find_dim(cd, leaf, cls == SV6C_TEXT ? SIFT_PDIM_DICT32 : SIFT_PDIM_INT64)) >= 0)
		*out = (AggBind) {ASRC_DIM, m, leaf};
	else
		return false;
	return true;
}

/* How well the cube holds the statement: lower is better, -1 when it does not */
static int
bind_cube(const SiftV6Preagg *pa, const SiftF2GranOffer *o, const SiftPreaggCubeDesc *cd, PartBind *pb)
{
	const SiftColSchema *schema = o->schema;
	int			score = cd->ndims;

	if (cd->flags & SIFT_PCUBE_ELEMENTS)
		return -1;
	for (int i = 0; i < pa->spec->nkeys; i++)
	{
		const SiftPreaggDimKind kind = pa->key_text[i] ? SIFT_PDIM_DICT32 : SIFT_PDIM_INT64;
		const SiftColDef *d = &schema->cols[pa->key_leaf[i]];

		pb->key_dim[i] = find_dim(cd, part_leaf(o, pa->key_leaf[i]), kind);
		if (pb->key_dim[i] < 0 || !def_plain(d) || (pa->key_text[i] ? d->type != SIFT_COLT_TEXT : d->store != SIFT_COLS_INT))
			return -1;
	}
	pb->reads = (SiftPreaggReads) {0};
	pb->has_dist = false;
	for (int j = 0; j < pa->spec->naggs; j++)
	{
		const SiftV6Agg *a = &pa->spec->agg[j];

		if (!bind_agg(pa, o, cd, j, &pb->agg[j]))
			return -1;
		pb->has_dist |= pb->agg[j].src == ASRC_DIST;
		if (pb->agg[j].src == ASRC_MEAS)
		{
			const uint32 bit = UINT32_C(1) << pb->agg[j].idx;
			const bool	sum = a->kind == SV6_SUM || a->kind == SV6_AVG;

			pb->reads.meas |= bit;
			pb->reads.minmax |= a->kind == SV6_MIN || a->kind == SV6_MAX ? bit : 0;
			pb->reads.float_sum |= sum && sift_v6_class(a->arg.typid) == SV6C_F64 ? bit : 0;
		}
	}
	for (int k = 0; k < o->npreds; k++)
	{
		const SiftColPred *p = &o->preds[k];
		const SiftColDef *d = &schema->cols[p->col];
		const int	leaf = part_leaf(o, p->col);
		int			dim = find_dim(cd, leaf, d->store == SIFT_COLS_STR ? SIFT_PDIM_DICT32 : SIFT_PDIM_INT64);

		if (dim < 0 && (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL))
			dim = find_dim(cd, leaf, SIFT_PDIM_NULLFLAG);
		pb->pred_dim[k] = dim;
		pb->pred_ok[k] = dim >= 0 && pred_on_dim(d, p, cd->dim_kind[dim]);
		score += pb->pred_ok[k] ? 0 : 1000;
	}
	return score;
}

static bool
bind_part(SiftV6Preagg *pa, const SiftF2GranOffer *o)
{
	PartBind   *pb = &pa->pb;
	MemoryContext old;
	int			best = -1,
				best_score = INT_MAX;

	MemoryContextReset(pa->pcx);
	old = MemoryContextSwitchTo(pa->pcx);
	*pb = (PartBind) {.gen = o->desc->part_gen,.bound = true};
	pb->agg = palloc(pa->spec->naggs * sizeof(AggBind));
	pb->pred_dim = palloc(Max(o->npreds, 1) * sizeof(int));
	pb->pred_ok = palloc(Max(o->npreds, 1));
	pb->pp = sift_preagg_part_open(o->rel, o->part);
	pb->dicts = sift_part_dicts(o->rel, o->part);
	for (int k = 0; pb->pp != NULL && k < pb->pp->hdr.ncubes; k++)
	{
		const int	score = bind_cube(pa, o, &pb->pp->cube[k], pb);

		if (score >= 0 && score < best_score)
		{
			best = k;
			best_score = score;
		}
	}
	if (best >= 0)
	{
		const SiftPreaggCubeDesc *cd = &pb->pp->cube[best];

		pb->cube = best;
		(void) bind_cube(pa, o, cd, pb);
		pb->usable = true;
		for (int d = 0; d < cd->ndims; d++)
		{
			const SiftPartDict *pd = cd->dim_kind[d] == SIFT_PDIM_DICT32 ? sift_part_dict_find(pb->dicts, cd->dim_leaf[d], SIFT_DICT_KIND_STR) : NULL;

			if (cd->dim_kind[d] == SIFT_PDIM_DICT32 && pd == NULL)
				pb->usable = false;
			else if (pd != NULL)
				pb->dict[d] = (SiftGranuleDict) {.n = pd->n,.off = pd->off,.bytes = pd->bytes,.part = true};
		}
	}
	MemoryContextSwitchTo(old);
	return pb->usable;
}

/* ---- the tests of a granule ---- */

/* Whether the zone of an integer column says that every row of the granule passes the test (its least and greatest, no NULL, no value the column could not hold) */
static bool
zone_all(const SiftColDef *d, const SiftColPred *p, const SiftZone *z, bool exceptions)
{
	if (z == NULL || exceptions || !def_plain(d) || d->store != SIFT_COLS_INT || z->nnull != 0 ||
		(z->flags & (SIFT_ZONE_NONE | SIFT_ZONE_PROMO | SIFT_ZONE_PROMO_EXC | SIFT_ZONE_FLOAT | SIFT_ZONE_HAS_NAN)) != 0)
		return false;
	for (int j = 0; j < p->nvals; j++)
		if (p->vals[j].isnull)
			return false;
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			return z->min == z->max && z->min == p->vals[0].v.i;
		case SIFT_COLOP_NE:
			return p->vals[0].v.i < z->min || p->vals[0].v.i > z->max;
		case SIFT_COLOP_LT:
			return z->max < p->vals[0].v.i;
		case SIFT_COLOP_LE:
			return z->max <= p->vals[0].v.i;
		case SIFT_COLOP_GT:
			return z->min > p->vals[0].v.i;
		case SIFT_COLOP_GE:
			return z->min >= p->vals[0].v.i;
		case SIFT_COLOP_IN:
			for (int j = 0; z->min == z->max && j < p->nvals; j++)
				if (p->vals[j].v.i == z->min)
					return true;
			return false;
		default:
			return false;
	}
}

/* Every predicate is one the cube tests on its keys or one the granule's zones or skipping indexes found true of every row */
static bool
preds_answerable(const PartBind *pb, const SiftF2GranOffer *o)
{
	for (int k = 0; k < o->npreds; k++)
		if (!pb->pred_ok[k] && !o->decided[k] && !zone_all(&o->schema->cols[o->preds[k].col], &o->preds[k], o->zones[k], o->exceptions[k]))
			return false;
	return true;
}

static bool
has_substring(const char *s, uint32 len, const char *sub, uint32 sublen)
{
	for (uint32 i = 0; i + sublen <= len; i++)
		if (memcmp(s + i, sub, sublen) == 0)
			return true;
	return false;
}

static bool
str_pred(const SiftColPred *p, const char *s, uint32 len)
{
	const SiftColValue *v = &p->vals[0];

	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
			return (len == v->len && memcmp(s, v->v.s, len) == 0) == (p->op == SIFT_COLOP_EQ);
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			{
				bool		in = false;

				for (int j = 0; !in && j < p->nvals; j++)
					in = len == p->vals[j].len && memcmp(s, p->vals[j].v.s, len) == 0;
				return in == (p->op == SIFT_COLOP_IN);
			}
		case SIFT_COLOP_PREFIX:
			return len >= v->len && memcmp(s, v->v.s, v->len) == 0;
		case SIFT_COLOP_SUFFIX:
			return len >= v->len && memcmp(s + len - v->len, v->v.s, v->len) == 0;
		case SIFT_COLOP_CONTAINS:
			return has_substring(s, len, v->v.s, v->len);
		default:
			return !has_substring(s, len, v->v.s, v->len);
	}
}

static bool
int_pred(const SiftColPred *p, int64 x)
{
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			return x == p->vals[0].v.i;
		case SIFT_COLOP_NE:
			return x != p->vals[0].v.i;
		case SIFT_COLOP_LT:
			return x < p->vals[0].v.i;
		case SIFT_COLOP_LE:
			return x <= p->vals[0].v.i;
		case SIFT_COLOP_GT:
			return x > p->vals[0].v.i;
		case SIFT_COLOP_GE:
			return x >= p->vals[0].v.i;
		default:
			{
				bool		in = false;

				for (int j = 0; !in && j < p->nvals; j++)
					in = x == p->vals[j].v.i;
				return in == (p->op == SIFT_COLOP_IN);
			}
	}
}

/* Whether the rows of a cell pass the predicates the cube tests (the keys of its group say it for all of them) */
static bool
cell_passes(const PartBind *pb, const SiftPreaggCubeDesc *cd, const SiftF2GranOffer *o, const SiftPreaggGroupHdr *grp)
{
	for (int k = 0; k < o->npreds; k++)
	{
		const SiftColPred *p = &o->preds[k];
		const int	d = pb->pred_dim[k];
		const bool	null = d >= 0 && (cd->dim_kind[d] == SIFT_PDIM_NULLFLAG ? grp->keys[d] == 0 : (grp->dim_null_mask >> d) & 1);

		if (!pb->pred_ok[k] || o->decided[k])
			continue;
		if (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL)
		{
			if (null != (p->op == SIFT_COLOP_ISNULL))
				return false;
		}
		else if (null)
			return false;
		else if (cd->dim_kind[d] == SIFT_PDIM_INT64 ? !int_pred(p, grp->keys[d]) : !str_pred(p, pb->dict[d].bytes + pb->dict[d].off[grp->keys[d]], pb->dict[d].off[grp->keys[d] + 1] - pb->dict[d].off[grp->keys[d]]))
			return false;
	}
	return true;
}

/* ---- the rows of the granule ---- */

bool
sift_v6_preagg_rows_known(SiftV6Vet *vet, const SiftF2GranOffer *o)
{
	const BlockNumber lo = (BlockNumber) (o->gd->tid_min >> 16),
				hi = (BlockNumber) (o->gd->tid_max >> 16);

	if (o->vis != SIFT_GVIS_HEAP)
		return o->vis == SIFT_GVIS_SETTLED;
	if (o->gd->tid_max == 0 || hi < lo || (uint64) hi - lo + 1 > PA_VM_SPAN_MAX)
		return false;
	for (BlockNumber b = lo; b <= hi; b++)
		if (!VM_ALL_VISIBLE(vet->heap, b, &vet->vmbuf))
			return false;
	return true;
}

/* ---- cells to states ---- */

static V6Acc
float_sum_acc(const SiftPreaggMeasure *pm)
{
	if ((pm->flags & SIFT_PM_HAS_NAN) || ((pm->flags & SIFT_PM_HAS_PINF) && (pm->flags & SIFT_PM_HAS_NINF)))
		return v6g_acc_of_f64_sum(get_float8_nan(), 0, pm->count_nonnull);
	if (pm->flags & SIFT_PM_HAS_PINF)
		return v6g_acc_of_f64_sum(get_float8_infinity(), 0, pm->count_nonnull);
	if (pm->flags & SIFT_PM_HAS_NINF)
		return v6g_acc_of_f64_sum(-get_float8_infinity(), 0, pm->count_nonnull);
	if (!isfinite(pm->u.f.sum_hi) || !isfinite(pm->u.f.sum_lo))
		float_overflow_error();
	return v6g_acc_of_f64_sum(pm->u.f.sum_hi, pm->u.f.sum_lo, pm->count_nonnull);
}

void
sift_v6_preagg_meas_acc(const SiftV6Agg *a, const SiftPreaggMeasure *pm, V6Acc *out)
{
	const bool	is_float = sift_v6_class(a->arg.typid) == SV6C_F64;

	out->n = pm->count_nonnull;
	if (a->kind == SV6_COUNT || pm->count_nonnull == 0)
		return;
	if (a->kind == SV6_SUM || a->kind == SV6_AVG)
	{
		if (is_float)
			*out = float_sum_acc(pm);
		else
			v6acc_set_i128(out, (SiftI128) {(uint64) pm->u.i.sum_lo, pm->u.i.sum_hi});
	}
	else if (!is_float)
		out->v0 = (uint64) (a->kind == SV6_MIN ? pm->u.i.min_i : pm->u.i.max_i);
	else if (a->kind == SV6_MAX)
		v6acc_set_f64(out, (pm->flags & SIFT_PM_HAS_NAN) ? get_float8_nan() : pm->u.f.max_f);
	else
		v6acc_set_f64(out, (pm->flags & SIFT_PM_HAS_NONNAN) ? pm->u.f.min_f : get_float8_nan());
}

static void
cell_acc(const PartBind *pb, const SiftV6Spec *spec, const SiftPreaggCubeDesc *cd, const SiftPreaggGroupHdr *grp, V6Acc *out)
{
	for (int j = 0; j < spec->naggs; j++)
	{
		const AggBind *b = &pb->agg[j];

		out[j] = (V6Acc) {0};
		switch (b->src)
		{
			case ASRC_ROWS:
				out[j].n = grp->row_count;
				break;
			case ASRC_DIM:
				out[j].n = ((grp->dim_null_mask >> b->idx) & 1) ? 0 : grp->row_count;
				break;
			case ASRC_MEAS:
				sift_v6_preagg_meas_acc(&spec->agg[j], sift_pag_measure(cd, grp, b->idx), &out[j]);
				break;
			case ASRC_DIST:
				break;
		}
	}
}

/* ---- batches of keys ---- */

static void
kb_reset(SiftV6Preagg *pa, const PartBind *pb)
{
	KeyBatch   *kb = pa->kb;

	kb->n = 0;
	memset(kb->valid, 0, sizeof(kb->valid));
	memset(kb->any_null, 0, sizeof(kb->any_null));
	for (int i = 0; i < pa->spec->nkeys; i++)
	{
		const int	d = pb->key_dim[i];

		kb->vec[i] = (SiftVec) {.data = kb->data[i],.dense = true,.type = pa->key_text[i] ? SVEC_DICT32 : SVEC_I64,.dict = pa->key_text[i] ? &pb->dict[d] : NULL};
		kb->vp[i] = &kb->vec[i];
	}
}

static void
kb_put(SiftV6Preagg *pa, const PartBind *pb, const SiftPreaggGroupHdr *grp)
{
	KeyBatch   *kb = pa->kb;
	const int	r = kb->n++;

	for (int i = 0; i < pa->spec->nkeys; i++)
	{
		const int	d = pb->key_dim[i];
		const bool	null = (grp->dim_null_mask >> d) & 1;

		if (pa->key_text[i])
			((uint32 *) kb->data[i])[r] = null ? pb->dict[d].n : (uint32) grp->keys[d];
		else
			((int64 *) kb->data[i])[r] = null ? 0 : grp->keys[d];
		if (!null)
			kb->valid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
		kb->any_null[i] |= null;
	}
}

static SiftVec *const *
kb_finish(SiftV6Preagg *pa)
{
	KeyBatch   *kb = pa->kb;

	for (int i = 0; i < pa->spec->nkeys; i++)
	{
		kb->vec[i].len = (uint16) kb->n;
		kb->vec[i].all_valid = !kb->any_null[i];
		kb->vec[i].valid = kb->any_null[i] ? kb->valid[i] : NULL;
	}
	return kb->vp;
}

static SiftStrView
strview_of(const char *p, uint32 len)
{
	SiftStrView v = {.len = len};

	if (len == 0)
		return v;
	memcpy(v.prefix, p, Min(len, 4));
	if (len <= 12)
		memcpy(v.u.tail, p + 4, len > 4 ? len - 4 : 0);
	else
		v.u.ptr = p;
	return v;
}

/* ---- the sets of distinct values ---- */

typedef struct DistSet
{
	uint32		n;
	int64	   *v;
} DistSet;

/* The distinct values of the set of each passing cell for aggregate j; false when a set is one the cube left out */
static bool
read_dist(SiftV6Preagg *pa, const PartBind *pb, const SiftF2GranOffer *o, const SiftPreaggGran *gran, const int *cells, int ncells, int j, DistSet *out)
{
	const SiftPreaggCubeDesc *cd = &pb->pp->cube[pb->cube];
	const bool	text = sift_v6_class(pa->spec->agg[j].arg.typid) == SV6C_TEXT;

	for (int i = 0; i < ncells; i++)
	{
		const SiftPreaggDistRef *ref = sift_pag_dist(cd, sift_pag_group(cd, gran->cells, cells[i]), pb->agg[j].idx);
		uint8	   *bytes;

		if (ref->mode == SIFT_PDIST_OVERFLOW || (ref->mode != SIFT_PDIST_EMPTY && (ref->mode == SIFT_PDIST_I64_LIST) == text))
			return false;
		bytes = sift_preagg_dist_read(o->rel, pb->pp, ref);
		out[i].n = sift_preagg_dist_values(ref, bytes, pa->scratch, SIFT_PAG_DIST_BM_BITS);
		out[i].v = out[i].n > 0 ? memcpy(palloc(out[i].n * sizeof(int64)), pa->scratch, out[i].n * sizeof(int64)) : NULL;
	}
	return true;
}

static void
dist_flush(SiftV6Preagg *pa, const PartBind *pb, int j)
{
	SiftVec    *v = &pa->dvec;
	SiftVec *const *keys = kb_finish(pa);

	v->len = (uint16) pa->kb->n;
	v->all_valid = true;
	v->valid = NULL;
	v->dense = true;
	sift_v6_group_merge_dist(pa->group, j, keys, v, (uint16) pa->kb->n);
	kb_reset(pa, pb);
}

static void
merge_dist(SiftV6Preagg *pa, const PartBind *pb, const SiftPreaggCubeDesc *cd, const SiftPreaggGran *gran, const int *cells, int ncells, int j, const DistSet *sets)
{
	const bool	text = sift_v6_class(pa->spec->agg[j].arg.typid) == SV6C_TEXT;
	const SiftPartDict *pd = text ? sift_part_dict_find(pb->dicts, pb->agg[j].leaf, SIFT_DICT_KIND_STR) : NULL;

	pa->dvec = (SiftVec) {.data = pa->ddata,.dense = true,.type = text ? SVEC_STRVIEW : SVEC_I64};
	kb_reset(pa, pb);
	for (int i = 0; i < ncells; i++)
		for (uint32 k = 0; k < sets[i].n; k++)
		{
			const int	r = pa->kb->n;

			if (text)
				((SiftStrView *) pa->ddata)[r] = strview_of(pd->bytes + pd->off[sets[i].v[k]], pd->off[sets[i].v[k] + 1] - pd->off[sets[i].v[k]]);
			else
				((int64 *) pa->ddata)[r] = sets[i].v[k];
			kb_put(pa, pb, sift_pag_group(cd, gran->cells, cells[i]));
			if (pa->kb->n == SIFT_VEC_LEN)
				dist_flush(pa, pb, j);
		}
	if (pa->kb->n > 0)
		dist_flush(pa, pb, j);
}

/* ---- a granule ---- */

static void
cells_flush(SiftV6Preagg *pa, const PartBind *pb, const V6Acc *acc)
{
	sift_v6_group_merge_cells(pa->group, kb_finish(pa), acc, (uint16) pa->kb->n);
	kb_reset(pa, pb);
}

/* The cells that pass the tests of the granule folded into the groups; false, with nothing folded, when a set of distinct values it needs is one the cube left out */
static bool
fold_cells(SiftV6Preagg *pa, const PartBind *pb, const SiftF2GranOffer *o, const SiftPreaggGran *gran)
{
	const SiftPreaggCubeDesc *cd = &pb->pp->cube[pb->cube];
	const int	naggs = pa->spec->naggs;
	int		   *cells = palloc(Max(gran->ncells, 1) * sizeof(int));
	int			ncells = 0;
	V6Acc	   *acc = palloc(Max(gran->ncells, 1) * naggs * sizeof(V6Acc));
	DistSet   **sets = palloc0(naggs * sizeof(DistSet *));

	for (uint32 c = 0; c < gran->ncells; c++)
	{
		const SiftPreaggGroupHdr *grp = sift_pag_group(cd, gran->cells, c);

		if (grp->row_count == 0 || !cell_passes(pb, cd, o, grp))
			continue;
		cell_acc(pb, pa->spec, cd, grp, &acc[ncells * naggs]);
		cells[ncells++] = (int) c;
	}
	for (int j = 0; j < naggs; j++)
		if (pb->agg[j].src == ASRC_DIST)
		{
			sets[j] = palloc(Max(ncells, 1) * sizeof(DistSet));
			if (!read_dist(pa, pb, o, gran, cells, ncells, j, sets[j]))
				return false;
		}
	kb_reset(pa, pb);
	for (int i = 0; i < ncells; i++)
	{
		kb_put(pa, pb, sift_pag_group(cd, gran->cells, cells[i]));
		if (pa->kb->n == SIFT_VEC_LEN || i == ncells - 1)
			cells_flush(pa, pb, &acc[(i + 1 - pa->kb->n) * naggs]);
	}
	for (int j = 0; j < naggs; j++)
		if (sets[j] != NULL)
			merge_dist(pa, pb, cd, gran, cells, ncells, j, sets[j]);
	return true;
}

/* The cells less the dead rows; false when the granule has to be scanned */
static bool
correct_cells(const SiftV6Preagg *pa, const PartBind *pb, const SiftF2GranOffer *o, SiftPreaggGran *gran, const uint16 *dead, int ndead)
{
	SiftGranuleDesc gd;
	bool	   *want = palloc0(o->part->hdr.ncols);
	char	   *rec;
	SiftGranuleView v;

	sift_preagg_v2_subtract_cols(&pb->pp->cube[pb->cube], &pb->reads, want);
	rec = sift_part_read_granule_cols(o->rel, o->part, o->g, want, &gd);
	v = sift_granule_open_part(o->rel, o->part, rec, gd.data_len, &gd);
	return sift_preagg_v2_subtract(&pb->pp->cube[pb->cube], gran, &v, dead, (uint16) ndead, &pb->reads);
}

bool
sift_v6_preagg_try_granule(void *arg, const SiftF2GranOffer *o)
{
	SiftV6Preagg *pa = arg;
	PartBind   *pb = &pa->pb;
	MemoryContext old;
	SiftPreaggGran gran;
	uint16		dead[SIFT_PREAGG_DEAD_MAX];
	int			ndead;
	bool		done = false;

	if (!(o->desc->ext_flags & SIFT_PART_EXT_PREAGG) || o->patched)
		return false;
	if ((!pb->bound || pb->gen != o->desc->part_gen) && !bind_part(pa, o))
		return false;
	if (!pb->usable || !preds_answerable(pb, o) || !sift_v6_preagg_rows_known(pa->vet, o) || (ndead = sift_preagg_dead_rows(o, dead)) < 0)
		return false;
	if (ndead > 0 && (pb->has_dist || pb->reads.float_sum != 0))
		return false;
	MemoryContextReset(pa->gcx);
	old = MemoryContextSwitchTo(pa->gcx);
	sift_preagg_gran_read(o->rel, pb->pp, pb->cube, o->g, &gran);
	if (gran.ncells > 0 && (ndead == 0 || correct_cells(pa, pb, o, &gran, dead, ndead)))
		done = fold_cells(pa, pb, o, &gran);
	MemoryContextSwitchTo(old);
	if (done)
	{
		pa->stats->grans_preagg++;
		pa->stats->grans_preagg_delta += ndead > 0;
	}
	return done;
}
