/*
 * col/f2/preagg_v2.c - the cubes of SEC_PREAGG version 2 (col/f2/preagg_v2.h): which cubes a part gets, and the cells of each granule.
 */
/* check-memory: allow-allocs the groups of a granule: at most the rows of one granule, 8192 */
#include <math.h>

#include "core/host.h"

#include "lib/stringinfo.h"

#include "col/f2/part_format.h"
#include "col/f2/preagg_v2.h"
#include "core/hash.h"
#include "core/memtrack.h"
#include "port/pg_bitutils.h"

#define PAG_AUTO_DIM_MAX_CARD	256		/* a column of at most this many distinct values is a dimension */
#define PAG_AUTO_DIST_MAX_CARD	4096	/* ... and of at most this many is one whose distinct values a cube counts */
#define PAG_AUTO_GROUPS			512		/* the most cells a cube of a granule has: a dimension and its NULL group, or the product of the dimensions of the joint cube */
#define PAG_AUTO_CELL_BYTES		65536	/* ... and the most bytes of them: the budget of SIFT_PAG_BUDGET_PERCENT would leave the cube out of most granules otherwise */
#define PAG_SPARSE_PERCENT		2		/* a column too wide to be a dimension that has a value in at most this share of its rows (a soft-delete stamp) is a dimension of whether it has one */
#define PAG_EXPLICIT_GROUPS		4096
#define PAG_SAMPLE_ROWS			16384

/* ---- which cubes ---- */

typedef struct ColInfo
{
	bool		numeric;		/* a measure */
	bool		dim;
	bool		dist;
	bool		chosen;			/* a dimension of the cubes: a column that is not is the better one to count the distinct values of */
	SiftPreaggDimKind dkind;
	SiftPreaggMeasKind mkind;
	uint32		card;
} ColInfo;

static bool
col_plain(const SiftColIn *c)
{
	return c->group1 == 0 && !c->key_leaf && !c->astext && c->hint != SIFT_CHINT_LIST && c->hint != SIFT_CHINT_KEY_PRESENT;
}

/* The rows of the sample of the column (every step-th) and those of them that are not NULL */
static uint32
sample_nonnull(const SiftColIn *col, int n, uint32 *sampled)
{
	const int	step = Max(1, n / PAG_SAMPLE_ROWS);
	uint32		nonnull = 0;

	*sampled = 0;
	for (int r = 0; r < n; r += step)
	{
		*sampled += 1;
		nonnull += col->isnull == NULL || !col->isnull[r];
	}
	return nonnull;
}

/* The distinct values of a sample of the column, at most cap + 1 */
static uint32
sample_distinct(const SiftColIn *col, int n, uint32 cap)
{
	const uint32 size = pg_nextpower2_32(cap * 4);
	const int	step = Max(1, n / PAG_SAMPLE_ROWS);
	int64	   *vals = palloc(size * sizeof(int64));
	bool	   *used = palloc0(size);
	uint32		nd = 0;

	for (int r = 0; r < n && nd <= cap; r += step)
	{
		uint32		h;

		if (col->isnull != NULL && col->isnull[r])
			continue;
		h = (uint32) sift_fmix64((uint64) col->i[r]) & (size - 1);
		while (used[h] && vals[h] != col->i[r])
			h = (h + 1) & (size - 1);
		if (!used[h])
		{
			used[h] = true;
			vals[h] = col->i[r];
			nd++;
		}
	}
	pfree(vals);
	pfree(used);
	return nd;
}

static bool
dict_codes_kept(const SiftColIn *c, const SiftDictEnc *d)
{
	return d != NULL && (c->pdict != NULL || d->row_codes != NULL);
}

static void
col_info(const SiftRows *rows, const SiftDictEnc *const *dicts, int c, ColInfo *ci)
{
	const SiftColIn *col = &rows->cols[c];

	*ci = (ColInfo) {0};
	if (!col_plain(col))
		return;
	if (col->kind == SIFT_CELL_INT && col->hint != SIFT_CHINT_NUM128)
	{
		const bool	num = col->hint == SIFT_CHINT_NUM64;
		const uint32 nd = sample_distinct(col, rows->nrows, PAG_AUTO_DIST_MAX_CARD);
		uint32		sampled;
		const uint32 nonnull = sample_nonnull(col, rows->nrows, &sampled);
		const bool	sparse = nonnull > 0 && (uint64) nonnull * 100 <= (uint64) sampled * PAG_SPARSE_PERCENT;

		ci->numeric = true;
		ci->mkind = num ? SIFT_PMEAS_NUM64 : SIFT_PMEAS_INT64;
		ci->card = nd;
		ci->dkind = SIFT_PDIM_INT64;
		ci->dim = !num && nd > 0 && nd <= PAG_AUTO_DIM_MAX_CARD;
		if (!num && !ci->dim && sparse)
		{
			ci->dkind = SIFT_PDIM_NULLFLAG;
			ci->card = 2;
			ci->dim = true;
		}
		ci->dist = !num && nd > 0 && nd <= PAG_AUTO_DIST_MAX_CARD;
	}
	else if (col->kind == SIFT_CELL_F8)
	{
		ci->numeric = true;
		ci->mkind = SIFT_PMEAS_F8;
	}
	else if (col->kind == SIFT_CELL_STR && col->hint == SIFT_CHINT_AUTO && dicts != NULL && dict_codes_kept(col, dicts[c]))
	{
		ci->card = dicts[c]->n;
		ci->dkind = SIFT_PDIM_DICT32;
		ci->dim = ci->card > 0 && ci->card <= PAG_AUTO_DIM_MAX_CARD;
		ci->dist = ci->card > 0 && ci->card <= SIFT_PAG_DIST_BM_BITS;
	}
}

/* The row r of a column as a group key: the code of the dictionary of a string, NULL as -1, whether it has a value for a NULL flag */
static int64
plan_key(const SiftColIn *col, const SiftDictEnc *dict, SiftPreaggDimKind kind, int r)
{
	if (kind == SIFT_PDIM_NULLFLAG)
		return col->isnull == NULL || !col->isnull[r];
	if (col->isnull != NULL && col->isnull[r])
		return -1;
	return col->kind == SIFT_CELL_STR ? (int64) (col->dcode != NULL ? col->dcode[r] : dict->row_codes[r]) : col->i[r];
}

/* The distinct combinations of the values of the columns in the first granule of the rows, at most limit + 1 */
static uint32
observed_groups(const SiftRows *rows, const SiftDictEnc *const *dicts, const ColInfo *ci, const int *cols, int n, uint32 limit)
{
	const uint32 size = pg_nextpower2_32((limit + 1) * 2);
	uint64	   *seen = palloc0(size * sizeof(uint64));
	uint32		groups = 0;

	for (int r = 0; r < Min(rows->nrows, SIFT_GRANULE_ROWS) && groups <= limit; r++)
	{
		uint64		h = 0;
		uint32		at;

		for (int i = 0; i < n; i++)
			h = sift_fmix64(h ^ (uint64) plan_key(&rows->cols[cols[i]], dicts != NULL ? dicts[cols[i]] : NULL, ci[cols[i]].dkind, r)) + i;
		h |= 1;
		for (at = (uint32) h & (size - 1); seen[at] != 0 && seen[at] != h; at = (at + 1) & (size - 1))
			;
		groups += seen[at] == 0;
		seen[at] = h;
	}
	pfree(seen);
	return groups;
}

static void
cube_measures(SiftPreaggCubeDesc *cd, const ColInfo *ci, int nc, const int *dims, int ndims)
{
	for (int pass = 0; pass < 2; pass++)
		for (int c = 0; c < nc && cd->nmeasures < SIFT_PREAGG_MAX_MEAS; c++)
		{
			bool		is_dim = false;

			for (int i = 0; i < ndims; i++)
				is_dim |= dims[i] == c;
			if (!ci[c].numeric || is_dim != (pass == 1))
				continue;
			cd->meas_leaf[cd->nmeasures] = (uint16) c;
			cd->meas_kind[cd->nmeasures++] = (uint8) ci[c].mkind;
		}
}

static void
add_cube(SiftPreaggPlan *plan, const ColInfo *ci, int nc, const int *dims, int ndims, const int *dist, int ndist, uint32 groups)
{
	SiftPreaggCubeDesc *cd = &plan->cube[plan->ncubes++];

	*cd = (SiftPreaggCubeDesc) {.ndims = (uint8) ndims,.max_groups_per_gran = groups};
	for (int i = 0; i < ndims; i++)
	{
		cd->dim_leaf[i] = (uint16) dims[i];
		cd->dim_kind[i] = (uint8) ci[dims[i]].dkind;
	}
	cube_measures(cd, ci, nc, dims, ndims);
	for (int i = 0; i < ndist; i++)
		cd->dist_leaf[cd->ndistinct++] = (uint16) dist[i];
}

/* The most bytes a cell of the cubes of the columns takes: the most measures and sets of distinct values it can have */
static uint32
cell_bytes_bound(const ColInfo *ci, int nc)
{
	int			nmeas = 0;

	for (int c = 0; c < nc; c++)
		nmeas += ci[c].numeric;
	return sizeof(SiftPreaggGroupHdr) + Min(nmeas, SIFT_PREAGG_MAX_MEAS) * sizeof(SiftPreaggMeasure) + SIFT_PREAGG_MAX_DIST * sizeof(SiftPreaggDistRef);
}

static int
card_cmp(const void *a, const void *b, void *arg)
{
	const ColInfo *ci = arg;
	const int	x = *(const int *) a,
				y = *(const int *) b;

	if (ci[x].chosen != ci[y].chosen)
		return ci[x].chosen ? 1 : -1;
	return ci[x].card != ci[y].card ? (ci[x].card < ci[y].card ? -1 : 1) : x - y;
}

/* The columns of ci that are flagged by pick, the least card first, at most max */
static int
ranked(const ColInfo *ci, int nc, bool dims, int max, int *out)
{
	int			n = 0;

	for (int c = 0; c < nc; c++)
		if (dims ? ci[c].dim : ci[c].dist)
			out[n++] = c;
	qsort_arg(out, n, sizeof(int), card_cmp, (void *) ci);
	return Min(n, max);
}

static bool
listed(const int *v, int n, int leaf)
{
	for (int i = 0; i < n; i++)
		if (v[i] == leaf)
			return true;
	return false;
}

/* The declared dimensions this part can hold (an integer, or a string it keeps a dictionary of: not a scaled numeric) */
static int
declared_dims(const SiftPreaggDecl *dc, const ColInfo *ci, int nc, int *dims)
{
	int			n = 0;

	for (int i = 0; i < dc->ndims; i++)
		if (dc->dim[i] < nc && ci[dc->dim[i]].dkind != 0 && ci[dc->dim[i]].mkind != SIFT_PMEAS_NUM64)
			dims[n++] = dc->dim[i];
	return n;
}

/* The cubes of a declaration: all the rows, each declared dimension, and the dimensions together when the groups of the first granule fit */
static void
plan_declared(const SiftRows *rows, const SiftDictEnc *const *dicts, ColInfo *ci, int *dims, SiftPreaggPlan *plan)
{
	const SiftPreaggDecl *dc = rows->preagg_decl;
	const int	nc = rows->ncols;
	int			nd;

	for (int c = 0; c < nc && dc->nmeas > 0; c++)
		ci[c].numeric &= listed(dc->meas, dc->nmeas, c);
	nd = declared_dims(dc, ci, nc, dims);
	add_cube(plan, ci, nc, dims, 0, NULL, 0, PAG_EXPLICIT_GROUPS);
	for (int i = 0; i < nd; i++)
		add_cube(plan, ci, nc, &dims[i], 1, NULL, 0, PAG_EXPLICIT_GROUPS);
	if (nd > 1 && observed_groups(rows, dicts, ci, dims, nd, PAG_EXPLICIT_GROUPS) <= PAG_EXPLICIT_GROUPS)
		add_cube(plan, ci, nc, dims, nd, NULL, 0, PAG_EXPLICIT_GROUPS);
}

bool
sift_preagg_v2_plan(const SiftRows *rows, const SiftDictEnc *const *dicts, SiftPreaggPlan *plan)
{
	const int	nc = rows->ncols;
	ColInfo    *ci = palloc0(Max(nc, 1) * sizeof(ColInfo));
	int		   *dims = palloc(Max(nc, 1) * sizeof(int)),
			   *dist = palloc(Max(nc, 1) * sizeof(int));
	int			nd,
				ndist,
				njoint = 0;
	uint32		groups;

	*plan = (SiftPreaggPlan) {0};
	for (int c = 0; c < nc; c++)
		col_info(rows, dicts, c, &ci[c]);
	if (rows->preagg_decl != NULL && rows->preagg_decl->ndims > 0)
		plan_declared(rows, dicts, ci, dims, plan);
	else
	{
		groups = Min(PAG_AUTO_GROUPS, PAG_AUTO_CELL_BYTES / cell_bytes_bound(ci, nc));
		for (int c = 0; c < nc; c++)
			ci[c].dim &= ci[c].card + 1 <= groups;
		nd = ranked(ci, nc, true, SIFT_PREAGG_MAX_DIMS, dims);
		for (int i = 0; i < nd; i++)
			ci[dims[i]].chosen = true;
		ndist = ranked(ci, nc, false, SIFT_PREAGG_MAX_DIST, dist);
		add_cube(plan, ci, nc, dims, 0, dist, ndist, groups);
		for (int i = 0; i < nd; i++)
			add_cube(plan, ci, nc, &dims[i], 1, dist, ndist, groups);
		for (njoint = nd; njoint > 1 && observed_groups(rows, dicts, ci, dims, njoint, groups) > groups; njoint--)
			;
		if (njoint > 1)
			add_cube(plan, ci, nc, dims, njoint, dist, 0, groups);
	}
	pfree(ci);
	pfree(dims);
	pfree(dist);
	return plan->ncubes > 1 || plan->cube[0].nmeasures > 0 || plan->cube[0].ndistinct > 0;
}

/* ---- the cells of a granule ---- */

typedef struct MeasAcc
{
	uint64		count;
	uint32		flags;
	uint64		sum_lo;
	int64		sum_hi;
	int64		mn,
				mx;
	double		fsum,
				fcomp,
				fmin,
				fmax;
} MeasAcc;

typedef struct DistAcc
{
	uint32		n;
	bool		over;
	int64		vals[SIFT_PAG_DIST_LIST_MAX];
	uint8	   *bm;
} DistAcc;

typedef struct Grp
{
	uint64		rows;
	uint8		nullmask;
	uint8		flags;
	int64		key[SIFT_PREAGG_MAX_DIMS];
	MeasAcc	   *m;
	DistAcc    *d;
} Grp;

typedef struct GranCtx
{
	const SiftPreaggCubeDesc *cd;
	const SiftColIn *cols;
	const SiftDictEnc *const *dicts;
	int			nrows;
	Grp		   *grp;
	int			ngrp;
	int		   *slot;
	uint32		mask;
} GranCtx;

struct SiftPreaggBuild
{
	MemoryContext cx;
	MemoryContext gcx;
	SiftPreaggPlan plan;
	uint32		ng;
	uint64		used;
	StringInfoData cells[SIFT_PREAGG_MAX_CUBES];
	uint32	   *first[SIFT_PREAGG_MAX_CUBES];
	uint32		ncells[SIFT_PREAGG_MAX_CUBES];
	StringInfoData dist;
	uint32		flags;
	uint8		cube_flags[SIFT_PREAGG_MAX_CUBES];
};

SiftPreaggBuild *
sift_preagg_v2_begin(const SiftPreaggPlan *plan, uint32 ngranules)
{
	SiftPreaggBuild *b = palloc0(sizeof(SiftPreaggBuild));

	b->cx = CurrentMemoryContext;
	b->gcx = sift_mem_allocset_create(CurrentMemoryContext, "sift preagg v2 granule");
	b->plan = *plan;
	b->ng = ngranules;
	initStringInfo(&b->dist);
	for (int k = 0; k < plan->ncubes; k++)
	{
		const SiftPreaggCubeDesc *cd = &plan->cube[k];

		initStringInfo(&b->cells[k]);
		b->first[k] = palloc0((ngranules + 1) * sizeof(uint32));
		b->flags |= (cd->ndims > 1 ? SIFT_PAG_HAS_MULTIKEY : 0) | (cd->ndistinct > 0 ? SIFT_PAG_HAS_DISTINCT : 0);
		for (int m = 0; m < cd->nmeasures; m++)
			b->flags |= cd->meas_kind[m] >= SIFT_PMEAS_F4 ? SIFT_PAG_HAS_FLOAT : 0;
	}
	return b;
}

static void
meas_add_int(MeasAcc *a, int64 v)
{
	if (a->count == 0)
		a->mn = a->mx = v;
	else
	{
		a->mn = Min(a->mn, v);
		a->mx = Max(a->mx, v);
	}
	a->count++;
	sift_pag_sum_add(&a->sum_lo, &a->sum_hi, v);
}

static void
meas_add_float(MeasAcc *a, double v)
{
	double		t;

	a->count++;
	if (isnan(v))
	{
		a->flags |= SIFT_PM_HAS_NAN;
		return;
	}
	if (isinf(v))
		a->flags |= v > 0 ? SIFT_PM_HAS_PINF : SIFT_PM_HAS_NINF;
	else
	{
		t = a->fsum + v;
		a->fcomp += fabs(a->fsum) >= fabs(v) ? (a->fsum - t) + v : (v - t) + a->fsum;
		a->fsum = t;
	}
	if (!(a->flags & SIFT_PM_HAS_NONNAN))
		a->fmin = a->fmax = v;
	else
	{
		a->fmin = v < a->fmin ? v : a->fmin;
		a->fmax = v > a->fmax ? v : a->fmax;
	}
	a->flags |= SIFT_PM_HAS_NONNAN;
}

static void
dist_add(DistAcc *a, int64 v, const SiftDictEnc *dict)
{
	if (dict != NULL)
	{
		if (a->bm == NULL)
			a->bm = palloc0((dict->n + 7) / 8);
		a->bm[v >> 3] |= (uint8) (1 << (v & 7));
		return;
	}
	if (a->over)
		return;
	for (uint32 i = 0; i < a->n; i++)
		if (a->vals[i] == v)
			return;
	if (a->n == SIFT_PAG_DIST_LIST_MAX)
		a->over = true;
	else
		a->vals[a->n++] = v;
}

/* The value of row r of a dimension or distinct column: false for NULL; a NULL flag always has one */
static bool
col_value(const SiftColIn *c, SiftPreaggDimKind kind, int r, int64 *v)
{
	if (kind == SIFT_PDIM_NULLFLAG)
	{
		*v = c->isnull == NULL || !c->isnull[r];
		return true;
	}
	if (c->isnull != NULL && c->isnull[r])
		return false;
	*v = c->kind == SIFT_CELL_STR ? (int64) c->dcode[r] : c->i[r];
	return true;
}

static uint64
key_hash(const GranCtx *x, uint8 nullmask, const int64 *key)
{
	uint64		h = sift_fmix64(nullmask + 1);

	for (int d = 0; d < x->cd->ndims; d++)
		h = sift_fmix64(h ^ (uint64) key[d]) + d;
	return h;
}

static Grp *
group_of(GranCtx *x, uint8 nullmask, const int64 *key, bool *full)
{
	uint32		h = (uint32) key_hash(x, nullmask, key) & x->mask;
	const SiftPreaggCubeDesc *cd = x->cd;

	for (; x->slot[h] >= 0; h = (h + 1) & x->mask)
	{
		Grp		   *g = &x->grp[x->slot[h]];

		if (g->nullmask == nullmask && memcmp(g->key, key, sizeof(int64) * cd->ndims) == 0)
			return g;
	}
	if (x->ngrp == (int) cd->max_groups_per_gran)
	{
		*full = true;
		return NULL;
	}
	x->slot[h] = x->ngrp;
	x->grp[x->ngrp] = (Grp) {.nullmask = nullmask,.m = palloc0(Max(cd->nmeasures, 1) * sizeof(MeasAcc)),.d = palloc0(Max(cd->ndistinct, 1) * sizeof(DistAcc))};
	memcpy(x->grp[x->ngrp].key, key, sizeof(int64) * cd->ndims);
	return &x->grp[x->ngrp++];
}

static void
fold_row(GranCtx *x, int r, Grp *g)
{
	const SiftPreaggCubeDesc *cd = x->cd;

	g->rows++;
	for (int m = 0; m < cd->nmeasures; m++)
	{
		const SiftColIn *c = &x->cols[cd->meas_leaf[m]];

		if (c->isnull != NULL && c->isnull[r])
			continue;
		if (cd->meas_kind[m] >= SIFT_PMEAS_F4)
			meas_add_float(&g->m[m], c->f[r]);
		else
			meas_add_int(&g->m[m], c->i[r]);
	}
	for (int d = 0; d < cd->ndistinct; d++)
	{
		const SiftColIn *c = &x->cols[cd->dist_leaf[d]];
		int64		v;

		if (col_value(c, SIFT_PDIM_INT64, r, &v))
			dist_add(&g->d[d], v, c->kind == SIFT_CELL_STR ? x->dicts[cd->dist_leaf[d]] : NULL);
	}
}

static int
grp_cmp(const void *a, const void *b)
{
	const Grp  *x = a,
			   *y = b;

	if (x->nullmask != y->nullmask)
		return x->nullmask < y->nullmask ? -1 : 1;
	for (int d = 0; d < SIFT_PREAGG_MAX_DIMS; d++)
		if (x->key[d] != y->key[d])
			return x->key[d] < y->key[d] ? -1 : 1;
	return 0;
}

static int
u32_cmp(const void *a, const void *b)
{
	const int64 x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

/* The set of distinct values as the bytes it is stored in, appended to the stream of the granule */
static SiftPreaggDistRef
put_dist(StringInfo dist, DistAcc *a, const SiftDictEnc *dict)
{
	SiftPreaggDistRef ref = {.off = (uint32) dist->len};

	if (dict != NULL)
	{
		const uint32 bytes = (dict->n + 7) / 8;
		uint32		cnt = 0;
		uint32		codes[SIFT_PAG_DIST_LIST_MAX];

		for (uint32 i = 0; a->bm != NULL && i < bytes; i++)
			cnt += pg_popcount32(a->bm[i]);
		if (cnt == 0)
			ref.mode = SIFT_PDIST_EMPTY;
		else if (cnt <= SIFT_PAG_DIST_LIST_MAX && cnt * sizeof(uint32) < bytes)
		{
			uint32		k = 0;

			for (uint32 v = 0; v < dict->n && k < lengthof(codes); v++)
				if (a->bm[v >> 3] & (1 << (v & 7)))
					codes[k++] = v;
			appendBinaryStringInfo(dist, (char *) codes, cnt * sizeof(uint32));
			ref.mode = SIFT_PDIST_U32_LIST;
		}
		else
		{
			appendBinaryStringInfo(dist, (char *) a->bm, bytes);
			ref.mode = SIFT_PDIST_DICT_BM;
		}
	}
	else if (a->over)
		ref.mode = SIFT_PDIST_OVERFLOW;
	else if (a->n > 0)
	{
		qsort(a->vals, a->n, sizeof(int64), u32_cmp);
		appendBinaryStringInfo(dist, (char *) a->vals, a->n * sizeof(int64));
		ref.mode = SIFT_PDIST_I64_LIST;
	}
	ref.len = (uint16) (dist->len - ref.off);
	return ref;
}

static void
put_group(StringInfo cells, StringInfo dist, const GranCtx *x, Grp *g)
{
	const SiftPreaggCubeDesc *cd = x->cd;
	SiftPreaggGroupHdr gh = {.row_count = g->rows,.dim_null_mask = g->nullmask};

	memcpy(gh.keys, g->key, sizeof(gh.keys));
	for (int d = 0; d < cd->ndistinct; d++)
		gh.flags |= g->d[d].over ? SIFT_PGRP_DIST_OVERFLOW : 0;
	appendBinaryStringInfo(cells, (char *) &gh, sizeof(gh));
	for (int m = 0; m < cd->nmeasures; m++)
	{
		const MeasAcc *a = &g->m[m];
		SiftPreaggMeasure pm = {.count_nonnull = a->count,.flags = a->flags};

		if (cd->meas_kind[m] >= SIFT_PMEAS_F4)
		{
			pm.flags |= SIFT_PM_IS_FLOAT;
			pm.u.f.sum_hi = a->fsum;
			pm.u.f.sum_lo = a->fcomp;
			pm.u.f.min_f = a->fmin;
			pm.u.f.max_f = a->fmax;
		}
		else
		{
			pm.flags |= cd->meas_kind[m] == SIFT_PMEAS_NUM64 ? SIFT_PM_IS_NUM64 : 0;
			pm.scale_or_pad = cd->meas_kind[m] == SIFT_PMEAS_NUM64 ? x->cols[cd->meas_leaf[m]].scale : 0;
			pm.u.i.sum_lo = (int64) a->sum_lo;
			pm.u.i.sum_hi = a->sum_hi;
			pm.u.i.min_i = a->mn;
			pm.u.i.max_i = a->mx;
		}
		appendBinaryStringInfo(cells, (char *) &pm, sizeof(pm));
	}
	for (int d = 0; d < cd->ndistinct; d++)
	{
		const SiftPreaggDistRef ref = put_dist(dist, &g->d[d], x->cols[cd->dist_leaf[d]].kind == SIFT_CELL_STR ? x->dicts[cd->dist_leaf[d]] : NULL);

		appendBinaryStringInfo(cells, (char *) &ref, sizeof(ref));
	}
}

/* A column the cube reads that holds a value the column cannot keep leaves the granule out */
static bool
cols_exact(const SiftPreaggCubeDesc *cd, const SiftColIn *cols, int nrows)
{
	for (int i = 0; i < cd->ndims + cd->nmeasures + cd->ndistinct; i++)
	{
		const int	leaf = i < cd->ndims ? cd->dim_leaf[i] : i < cd->ndims + cd->nmeasures ? cd->meas_leaf[i - cd->ndims] : cd->dist_leaf[i - cd->ndims - cd->nmeasures];
		const SiftColIn *c = &cols[leaf];

		for (int r = 0; c->tag != NULL && r < nrows; r++)
			if (c->tag[r] != 0)
				return false;
	}
	return true;
}

/* The cells of the granule in this cube, appended to cells and dist; false when it has more groups than the cube holds or a column it reads is not exact */
static bool
cube_granule(const SiftPreaggCubeDesc *cd, const SiftColIn *cols, int nrows, const SiftDictEnc *const *dicts, StringInfo cells, StringInfo dist, uint32 *ncells, uint8 *cflags)
{
	GranCtx		x = {.cd = cd,.cols = cols,.dicts = dicts,.nrows = nrows};
	const uint32 size = pg_nextpower2_32(Max(cd->max_groups_per_gran, 4) * 2);
	bool		full = false;

	if (!cols_exact(cd, cols, nrows))
		return false;
	x.grp = palloc(Min(cd->max_groups_per_gran, (uint32) nrows) * sizeof(Grp) + sizeof(Grp));
	x.slot = palloc(size * sizeof(int));
	memset(x.slot, 0xFF, size * sizeof(int));
	x.mask = size - 1;
	for (int r = 0; r < nrows && !full; r++)
	{
		int64		key[SIFT_PREAGG_MAX_DIMS] = {0};
		uint8		nullmask = 0;
		Grp		   *g;

		for (int d = 0; d < cd->ndims; d++)
			if (!col_value(&cols[cd->dim_leaf[d]], (SiftPreaggDimKind) cd->dim_kind[d], r, &key[d]))
				nullmask |= (uint8) (1 << d);
		g = group_of(&x, nullmask, key, &full);
		if (g != NULL)
			fold_row(&x, r, g);
	}
	if (full)
		return false;
	qsort(x.grp, x.ngrp, sizeof(Grp), grp_cmp);
	for (int i = 0; i < x.ngrp; i++)
	{
		put_group(cells, dist, &x, &x.grp[i]);
		*cflags |= x.grp[i].nullmask != 0 ? SIFT_PCUBE_NULL_GROUPS : 0;
	}
	*ncells = x.ngrp;
	return true;
}

bool
sift_preagg_v2_add_granule(SiftPreaggBuild *b, uint32 g, const SiftColIn *cols, int nrows, const SiftDictEnc *const *dicts, uint64 data_bytes)
{
	const uint64 budget = data_bytes / 100 * SIFT_PAG_BUDGET_PERCENT + SIFT_PAG_BUDGET_SLACK;
	bool		all = true;
	MemoryContext old = MemoryContextSwitchTo(b->gcx);

	for (int k = 0; k < b->plan.ncubes; k++)
	{
		StringInfoData cells,
					dist;
		uint32		ncells = 0;
		uint8		cflags = 0;
		bool		ok;

		initStringInfo(&cells);
		initStringInfo(&dist);
		ok = cube_granule(&b->plan.cube[k], cols, nrows, dicts, &cells, &dist, &ncells, &cflags) && b->used + cells.len + dist.len <= budget;
		b->first[k][g] = b->ncells[k];
		if (ok)
		{
			SiftPreaggCubeDesc *cd = &b->plan.cube[k];
			const uint32 csize = sift_pag_cell_size(cd);

			for (uint32 i = 0; i < ncells; i++)
			{
				char	   *cell = cells.data + (uint64) i * csize;

				for (int d = 0; d < cd->ndistinct; d++)
					((SiftPreaggDistRef *) (cell + sizeof(SiftPreaggGroupHdr) + cd->nmeasures * sizeof(SiftPreaggMeasure)))[d].off += (uint32) b->dist.len;
			}
			appendBinaryStringInfo(&b->cells[k], cells.data, cells.len);
			appendBinaryStringInfo(&b->dist, dist.data, dist.len);
			b->ncells[k] += ncells;
			b->used += cells.len + dist.len;
			b->cube_flags[k] |= cflags;
		}
		all &= ok;
		MemoryContextReset(b->gcx);
	}
	MemoryContextSwitchTo(old);
	for (int k = 0; k < b->plan.ncubes; k++)
		b->first[k][g + 1] = b->ncells[k];
	return all;
}

static void
pad8(StringInfo s)
{
	while (s->len % 8 != 0)
		appendStringInfoChar(s, '\0');
}

void *
sift_preagg_v2_assemble(int ncubes, const SiftPreaggCubeDesc *cube, uint32 ng, uint32 flags, uint32 *const *first, const StringInfoData *cells, const StringInfoData *dist, uint64 *len)
{
	StringInfoData out;
	SiftPreaggSecHdr hdr = {.magic = SIFT_PREAGG_MAGIC,.version = SIFT_PREAGG_VERSION,.ncubes = (uint16) ncubes,.ngranules = ng,.flags = flags};
	SiftPreaggCubeDesc cd[SIFT_PREAGG_MAX_CUBES];
	uint64		at = sizeof(hdr) + ncubes * sizeof(SiftPreaggCubeDesc);

	hdr.cubes_off = sizeof(hdr);
	for (int k = 0; k < ncubes; k++)
	{
		cd[k] = cube[k];
		cd[k].gran_dir_off = (uint32) at;
		at += MAXALIGN((uint64) (ng + 1) * sizeof(uint32));
		cd[k].cells_off = at;
		at += cells[k].len;
	}
	hdr.dist_off = dist->len > 0 ? at : 0;
	initStringInfo(&out);
	appendBinaryStringInfo(&out, (char *) &hdr, sizeof(hdr));
	appendBinaryStringInfo(&out, (char *) cd, ncubes * sizeof(SiftPreaggCubeDesc));
	for (int k = 0; k < ncubes; k++)
	{
		appendBinaryStringInfo(&out, (char *) first[k], (ng + 1) * sizeof(uint32));
		pad8(&out);
		appendBinaryStringInfo(&out, cells[k].data, cells[k].len);
	}
	appendBinaryStringInfo(&out, dist->data, dist->len);
	*len = out.len;
	return out.data;
}

void *
sift_preagg_v2_finish(SiftPreaggBuild *b, uint64 *len)
{
	SiftPreaggCubeDesc cube[SIFT_PREAGG_MAX_CUBES];
	void	   *sec;

	for (int k = 0; k < b->plan.ncubes; k++)
	{
		cube[k] = b->plan.cube[k];
		cube[k].flags = b->cube_flags[k];
	}
	sec = sift_preagg_v2_assemble(b->plan.ncubes, cube, b->ng, b->flags, b->first, b->cells, &b->dist, len);
	MemoryContextDelete(b->gcx);
	return sec;
}
