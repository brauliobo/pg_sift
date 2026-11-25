/*
 * col/f2/proj/agg.c - the cube of a SIFT_PROJ_AGG projection: the groups of the rows of one base part, each with the rows it has and the state of each measure (count, a sum of 128 bits and
 * the values it is of, a minimum, a maximum). Rows are added and taken away exactly (counts and sums are integers), the cells of a stored part are absorbed into the cube of a query that groups by
 * a subset of the columns or by a coarser date_trunc, and the groups are written back as the rows of a part. A sum of floating point values is never kept (the spec refuses it), so every
 * answer a cube gives is the one stock PostgreSQL gives.
 */
#include "core/host.h"

#include <math.h>

#include "col/f2/agg/proj/pred.h"
#include "col/f2/proj/proj.h"

/* Where the stored columns of a measure are in the layout of the part a row is absorbed from: found once for the layout, not for each row */
typedef struct StoredCols
{
	const SiftProjSpec *src;
	const SiftProjLayoutCol *layout;
	int			nlayout;
	int			rows;
	int			at[SIFT_PROJ_MAX_COLS][3];	/* per measure of src: the count, the sum's high, low and non-null count, or the extreme */
} StoredCols;

struct SiftProjCube
{
	SiftProjSpec spec;
	const SiftProjColumns *cols;
	int			ngroups;		/* in g, dead ones (rows 0) included until settle */
	int			cap;
	SiftProjGroup *g;
	uint64	   *hash;
	int32	   *tab;			/* index into g + 1, 0: empty */
	int			tabcap;
	SiftStrArena strs;			/* the bytes of the strings of the keys */
	bool		settled;		/* no group has lost its last row since settle ran */
	StoredCols	stored_cols;
	SiftCell   *rows_out;
	SiftCell   *scratch;		/* a row of the relation (one cell per column) the filters of stored rows are tested on */
};

static uint64
mix(uint64 h, const void *p, size_t n)
{
	const unsigned char *b = p;

	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * UINT64CONST(1099511628211);
	return h;
}

static uint64
hash_key(const SiftProjSpec *s, const SiftCell *key)
{
	uint64		h = UINT64CONST(14695981039346656037);

	for (int i = 0; i < s->nkeys; i++)
	{
		uint8		isnull = key[i].isnull;

		h = mix(h, &isnull, 1);
		if (isnull)
			continue;
		if (key[i].s != NULL)
		{
			h = mix(h, &key[i].slen, sizeof(uint32));
			h = mix(h, key[i].s, key[i].slen);
		}
		else
			h = mix(h, &key[i].i, sizeof(int64));
	}
	return h;
}

static int
cell_cmp(const SiftCell *a, const SiftCell *b)
{
	int			c;

	if (a->isnull || b->isnull)
		return a->isnull == b->isnull ? 0 : (a->isnull ? 1 : -1);
	if (a->s != NULL || b->s != NULL)
	{
		c = memcmp(a->s, b->s, Min(a->slen, b->slen));
		return c != 0 ? c : (a->slen < b->slen ? -1 : a->slen > b->slen);
	}
	return a->i < b->i ? -1 : a->i > b->i;
}

static bool
key_equal(const SiftProjSpec *s, const SiftCell *a, const SiftCell *b)
{
	for (int i = 0; i < s->nkeys; i++)
		if (cell_cmp(&a[i], &b[i]) != 0)
			return false;
	return true;
}

SiftProjCube *
sift_proj_cube_new(const SiftProjSpec *s, const SiftProjColumns *cols)
{
	SiftProjCube *c = palloc0(sizeof(SiftProjCube));

	c->spec = *s;
	c->cols = cols;
	c->settled = true;
	sift_str_arena_init(&c->strs, CurrentMemoryContext);
	return c;
}

void
sift_proj_cube_free(SiftProjCube *c)
{
	for (int i = 0; i < c->ngroups; i++)
	{
		pfree(c->g[i].key);
		pfree(c->g[i].st);
	}
	sift_str_arena_free(&c->strs);
	if (c->g != NULL)
		pfree(c->g);
	if (c->hash != NULL)
		pfree(c->hash);
	if (c->tab != NULL)
		pfree(c->tab);
	if (c->rows_out != NULL)
		pfree(c->rows_out);
	if (c->scratch != NULL)
		pfree(c->scratch);
	pfree(c);
}

static void
rehash(SiftProjCube *c, int tabcap)
{
	if (c->tab != NULL)
		pfree(c->tab);
	c->tabcap = tabcap;
	c->tab = palloc0(tabcap * sizeof(int32));
	for (int i = 0; i < c->ngroups; i++)
	{
		uint32		at = (uint32) c->hash[i] & (tabcap - 1);

		while (c->tab[at] != 0)
			at = (at + 1) & (tabcap - 1);
		c->tab[at] = i + 1;
	}
}

static SiftProjGroup *
group_for(SiftProjCube *c, const SiftCell *key)
{
	uint64		h = hash_key(&c->spec, key);
	uint32		at;
	SiftProjGroup *g;

	if (c->tab == NULL)
		rehash(c, 64);
	at = (uint32) h & (c->tabcap - 1);
	for (; c->tab[at] != 0; at = (at + 1) & (c->tabcap - 1))
		if (c->hash[c->tab[at] - 1] == h && key_equal(&c->spec, c->g[c->tab[at] - 1].key, key))
			return &c->g[c->tab[at] - 1];
	if (c->ngroups == c->cap)
	{
		c->cap = c->cap == 0 ? 64 : c->cap * 2;
		c->g = c->g == NULL ? palloc(c->cap * sizeof(SiftProjGroup)) : repalloc(c->g, c->cap * sizeof(SiftProjGroup));
		c->hash = c->hash == NULL ? palloc(c->cap * sizeof(uint64)) : repalloc(c->hash, c->cap * sizeof(uint64));
	}
	g = &c->g[c->ngroups];
	/* check-memory: allow the keys of a group are those of one projection spec */
	g->key = palloc(Max(c->spec.nkeys, 1) * sizeof(SiftCell));
	memcpy(g->key, key, c->spec.nkeys * sizeof(SiftCell));
	for (int i = 0; i < c->spec.nkeys; i++)
		if (!key[i].isnull && key[i].s != NULL)
			g->key[i].s = sift_str_arena_copy(&c->strs, key[i].s, key[i].slen);
	g->rows = 0;
	g->st = palloc0(Max(c->spec.nmeas, 1) * sizeof(SiftProjMState));
	c->hash[c->ngroups++] = h;
	c->tab[at] = c->ngroups;
	if (c->ngroups * 2 > c->tabcap)
		rehash(c, c->tabcap * 2);
	return &c->g[c->ngroups - 1];
}

/* PostgreSQL's order of float8: NaN is greater than every number and equal to itself */
static bool
f8_less(double a, double b)
{
	return isnan(b) ? !isnan(a) : (!isnan(a) && a < b);
}

static void
take_extreme(SiftProjMState *st, SiftCellKind kind, bool is_min, bool has, int64 i, double f)
{
	bool		better;

	if (!has)
		return;
	if (!st->has)
		better = true;
	else if (kind == SIFT_CELL_F8)
		better = is_min ? f8_less(f, st->mf) : f8_less(st->mf, f);
	else
		better = is_min ? i < st->mi : i > st->mi;
	if (better)
	{
		st->has = true;
		st->mi = i;
		st->mf = f;
	}
}

/* The predicates of a spec (WHERE, FILTER) hold on the row */
bool
sift_proj_spec_preds_pass(const SiftProjSpecPred *p, int n, const SiftCell *row)
{
	for (int i = 0; i < n; i++)
	{
		SiftProjConst vals[SIFT_PROJ_PRED_VALS];
		SiftProjPred pred;

		sift_proj_pred_of_spec(&p[i], vals, &pred);
		if (!sift_proj_pred_pass(&pred, row))
			return false;
	}
	return true;
}

/* Every predicate of a spec can be judged on the row's cells: no value in a column it tests is an exception tag */
bool
sift_proj_spec_preds_decidable(const SiftProjSpecPred *p, int n, const SiftCell *row)
{
	for (int i = 0; i < n; i++)
	{
		SiftProjConst vals[SIFT_PROJ_PRED_VALS];
		SiftProjPred pred;

		sift_proj_pred_of_spec(&p[i], vals, &pred);
		if (!sift_proj_preds_decidable(&pred, 1, row))
			return false;
	}
	return true;
}

static void
measure_add(SiftProjMState *st, const SiftProjMeasure *m, const SiftProjColumns *cols, const SiftCell *row, int sign)
{
	const SiftCell *x = m->leaf >= 0 ? &row[m->leaf] : NULL;

	if (m->nfilter > 0 && !sift_proj_spec_preds_pass(m->filter, m->nfilter, row))
		return;
	switch (m->fn)
	{
		case SPM_COUNT_DISTINCT:
			break;
		case SPM_COUNT_STAR:
			st->cnt += sign;
			break;
		case SPM_COUNT:
			st->cnt += x->isnull ? 0 : sign;
			break;
		case SPM_SUM:
		case SPM_AVG:
			if (!x->isnull)
			{
				st->sum += sign * (int128) x->i;
				st->cnt += sign;
			}
			break;
		default:
			if (!x->isnull)
				take_extreme(st, cols->col[m->leaf].kind, m->fn == SPM_MIN, true, x->i, x->f);
			break;
	}
}

static void
key_of_row(const SiftProjSpec *s, const SiftProjColumns *cols, const SiftCell *row, SiftCell *key)
{
	for (int i = 0; i < s->nkeys; i++)
	{
		key[i] = row[s->key[i].leaf];
		if (s->key[i].fn == SPF_TRUNC && !key[i].isnull)
			key[i].i = sift_proj_trunc_cell((SiftProjTime) cols->col[s->key[i].leaf].time, key[i].i, (SiftProjUnit) s->key[i].unit);
	}
}

bool
sift_proj_cube_add(SiftProjCube *c, const SiftCell *row, int sign)
{
	SiftCell	key[SIFT_PROJ_MAX_COLS];
	SiftProjGroup *g;

	if (sign < 0 && !sift_proj_spec_invertible(&c->spec))
		return false;
	key_of_row(&c->spec, c->cols, row, key);
	g = group_for(c, key);
	c->settled = false;
	g->rows += sign;
	for (int m = 0; m < c->spec.nmeas; m++)
		measure_add(&g->st[m], &c->spec.meas[m], c->cols, row, sign);
	return true;
}

static int
find_col(const SiftProjLayoutCol *layout, int n, int measure, SiftProjRole role)
{
	for (int i = 0; i < n; i++)
		if (layout[i].measure == measure && layout[i].role == role)
			return i;
	return -1;
}

static const StoredCols *
stored_cols_of(SiftProjCube *c, const SiftProjSpec *src, const SiftProjLayoutCol *layout, int n)
{
	StoredCols *sc = &c->stored_cols;

	if (sc->src == src && sc->layout == layout && sc->nlayout == n)
		return sc;
	*sc = (StoredCols) {src, layout, n, find_col(layout, n, -1, SPR_ROWS)};
	for (int m = 0; m < src->nmeas; m++)
		switch (src->meas[m].fn)
		{
			case SPM_COUNT_DISTINCT:
				break;
			case SPM_COUNT_STAR:
			case SPM_COUNT:
				sc->at[m][0] = find_col(layout, n, m, SPR_COUNT);
				break;
			case SPM_SUM:
			case SPM_AVG:
				sc->at[m][0] = find_col(layout, n, m, SPR_SUM_HI);
				sc->at[m][1] = find_col(layout, n, m, SPR_SUM_LO);
				sc->at[m][2] = find_col(layout, n, m, SPR_SUM_NN);
				break;
			default:
				sc->at[m][0] = find_col(layout, n, m, src->meas[m].fn == SPM_MIN ? SPR_MIN : SPR_MAX);
				break;
		}
	return sc;
}

static SiftProjMState
state_of_stored(const SiftProjSpec *src, const int *at, int m, const SiftCell *stored)
{
	SiftProjMState st = {0};

	switch (src->meas[m].fn)
	{
		case SPM_COUNT_STAR:
		case SPM_COUNT:
			st.cnt = stored[at[0]].i;
			break;
		case SPM_SUM:
		case SPM_AVG:
			st.sum = (int128) (((uint128) (uint64) stored[at[0]].i << 64) | (uint64) stored[at[1]].i);
			st.cnt = stored[at[2]].i;
			break;
		default:
			st.has = !stored[at[0]].isnull;
			st.mi = stored[at[0]].i;
			st.mf = stored[at[0]].f;
			break;
	}
	return st;
}

static void
measure_merge(SiftProjMState *dst, const SiftProjMeasure *m, const SiftProjColumns *cols, const SiftProjMState *in)
{
	switch (m->fn)
	{
		case SPM_COUNT_DISTINCT:
			break;
		case SPM_COUNT_STAR:
		case SPM_COUNT:
			dst->cnt += in->cnt;
			break;
		case SPM_SUM:
		case SPM_AVG:
			dst->sum += in->sum;
			dst->cnt += in->cnt;
			break;
		default:
			take_extreme(dst, cols->col[m->leaf].kind, m->fn == SPM_MIN, in->has, in->mi, in->mf);
			break;
	}
}

/* A FILTER of the query that names group columns of the stored row only: tested on the stored key cells, laid out as the relation's columns */
static bool
stored_filter_passes(SiftProjCube *c, const SiftProjLayoutCol *layout, int n, const SiftProjMeasure *m, const SiftCell *stored)
{
	if (c->scratch == NULL)
		c->scratch = palloc0(Max(c->cols->n, 1) * sizeof(SiftCell));
	for (int i = 0; i < n; i++)
		if (layout[i].role == SPR_KEY)
			c->scratch[layout[i].leaf] = stored[i];
	return sift_proj_spec_preds_pass(m->filter, m->nfilter, c->scratch);
}

void
sift_proj_cube_absorb(SiftProjCube *c, const SiftProjSpec *src, const SiftProjLayoutCol *layout, int nlayout, const SiftProjRollup *map, const SiftCell *stored)
{
	SiftCell	key[SIFT_PROJ_MAX_COLS];
	SiftProjGroup *g;
	const StoredCols *sc = stored_cols_of(c, src, layout, nlayout);
	int64		rows = stored[sc->rows].i;

	for (int i = 0; i < c->spec.nkeys; i++)
	{
		key[i] = stored[map->src_key[i]];
		if (map->unit[i] != SPU_NONE && !key[i].isnull)
			key[i].i = sift_proj_trunc_cell((SiftProjTime) c->cols->col[c->spec.key[i].leaf].time, key[i].i, (SiftProjUnit) map->unit[i]);
	}
	g = group_for(c, key);
	c->settled = false;
	g->rows += rows;
	for (int j = 0; j < c->spec.nmeas; j++)
	{
		SiftProjMState in;

		if (map->filter_mode[j] == SPFM_ON_KEY && !stored_filter_passes(c, layout, nlayout, &c->spec.meas[j], stored))
			continue;
		in = map->src_meas[j] < 0 ? (SiftProjMState) {.cnt = rows} : state_of_stored(src, sc->at[map->src_meas[j]], map->src_meas[j], stored);
		measure_merge(&g->st[j], &c->spec.meas[j], c->cols, &in);
	}
}

int
sift_proj_cube_size(const SiftProjCube *c)
{
	return c->ngroups;
}

SiftProjGroup *
sift_proj_cube_touch(SiftProjCube *c, const SiftCell *key)
{
	c->settled = false;
	return group_for(c, key);
}

/* Drops the groups whose rows were all taken away */
static void
settle(SiftProjCube *c)
{
	int			n = 0;

	if (c->settled)
		return;
	c->settled = true;
	for (int i = 0; i < c->ngroups; i++)
	{
		if (c->g[i].rows == 0)
		{
			pfree(c->g[i].key);
			pfree(c->g[i].st);
			continue;
		}
		c->g[n] = c->g[i];
		c->hash[n++] = c->hash[i];
	}
	if (n != c->ngroups)
	{
		c->ngroups = n;
		if (c->tab != NULL)
			rehash(c, c->tabcap);
	}
}

int
sift_proj_cube_ngroups(SiftProjCube *c)
{
	settle(c);
	return c->ngroups;
}

const SiftProjGroup *
sift_proj_cube_group(SiftProjCube *c, int i)
{
	settle(c);
	return &c->g[i];
}

static int
group_cmp(const void *a, const void *b, void *arg)
{
	const SiftProjGroup *x = a,
			   *y = b;
	const SiftProjSpec *spec = arg;

	for (int i = 0; i < spec->nkeys; i++)
	{
		int			r = cell_cmp(&x->key[i], &y->key[i]);

		if (r != 0)
			return r;
	}
	return 0;
}

static SiftCell
int_cell(int64 v)
{
	return (SiftCell) {.i = v};
}

static void
stored_measure(const SiftProjMeasure *m, const SiftProjColumns *cols, const SiftProjMState *st, SiftCell **at)
{
	SiftCell   *o = *at;

	switch (m->fn)
	{
		case SPM_COUNT_DISTINCT:
			break;
		case SPM_COUNT_STAR:
		case SPM_COUNT:
			*o++ = int_cell(st->cnt);
			break;
		case SPM_SUM:
		case SPM_AVG:
			*o++ = int_cell((int64) (st->sum >> 64));
			*o++ = int_cell((int64) (uint64) st->sum);
			*o++ = int_cell(st->cnt);
			break;
		default:
			*o = (SiftCell) {.isnull = !st->has,.i = st->mi,.f = st->mf};
			o++;
			(void) cols;
			break;
	}
	*at = o;
}

int
sift_proj_cube_rows(SiftProjCube *c, const SiftCell **out, int *ncols)
{
	SiftProjLayoutCol layout[SIFT_PROJ_LAYOUT_MAX];
	int			nl = sift_proj_layout(&c->spec, c->cols, layout);
	SiftProjGroup *sorted;

	settle(c);
	sorted = palloc(Max(c->ngroups, 1) * sizeof(SiftProjGroup));
	if (c->ngroups > 0)
		memcpy(sorted, c->g, c->ngroups * sizeof(SiftProjGroup));
	qsort_arg(sorted, c->ngroups, sizeof(SiftProjGroup), group_cmp, &c->spec);
	if (c->rows_out != NULL)
		pfree(c->rows_out);
	c->rows_out = palloc0(Max(c->ngroups, 1) * nl * sizeof(SiftCell));
	for (int r = 0; r < c->ngroups; r++)
	{
		SiftCell   *at = c->rows_out + (size_t) r * nl;

		memcpy(at, sorted[r].key, c->spec.nkeys * sizeof(SiftCell));
		at += c->spec.nkeys;
		*at++ = int_cell(sorted[r].rows);
		for (int m = 0; m < c->spec.nmeas; m++)
			stored_measure(&c->spec.meas[m], c->cols, &sorted[r].st[m], &at);
	}
	pfree(sorted);
	*out = c->rows_out;
	*ncols = nl;
	return c->ngroups;
}
