/*
 * col/f2/key_plan.c - the key ranges of a set of predicates and the granules of a part they reach (col/f2/key_plan.h).
 */
#include "core/host.h"

#include "col/f2/key_plan.h"

#define SET_CAP			SIFT_KEY_PLAN_MAX_RANGES

/* What the predicates say of one key column */
typedef struct ColCon
{
	const SiftKeyVal *set[SET_CAP];
	int			nset;
	bool		has_set;
	const SiftKeyVal *lo;
	const SiftKeyVal *hi;
	bool		lo_incl;
	bool		hi_incl;
	bool		is_null;
	bool		not_null;
	const SiftKeyVal *pfx;
	bool		empty;
} ColCon;

/* A string of key bytes in the making: cut at key_bytes, with whether more was dropped */
typedef struct Bound
{
	uint8		b[SIFT_KEY_BYTES_MAX];
	uint32		len;
	bool		trunc;
} Bound;

static void
bound_put(const SiftKeySpec *s, Bound *x, uint8 byte)
{
	if (x->len < s->key_bytes)
		x->b[x->len++] = byte;
	else
		x->trunc = true;
}

static void
bound_enc(const SiftKeySpec *s, Bound *x, int col, const SiftKeyVal *v)
{
	if (x->len < s->key_bytes)
		x->len += sift_key_enc_val(&s->cols[col], v, x->b + x->len, s->key_bytes - x->len, &x->trunc);
	else
		x->trunc = true;
}

/* The least string above every string that has x as a prefix; false when there is none. A cut string keeps its prefix: a bound below the true one is never wrong */
static bool
bound_succ(Bound *x)
{
	if (x->trunc)
		return true;
	while (x->len > 0 && x->b[x->len - 1] == 0xFF)
		x->len--;
	if (x->len == 0)
		return false;
	x->b[x->len - 1]++;
	return true;
}

static void
bound_image(const SiftKeySpec *s, const Bound *x, uint8 *out)
{
	memset(out, 0, s->key_bytes);
	memcpy(out, x->b, x->len);
}

/* ---- what each column is confined to ---- */

static void
tighten_lo(const SiftKeyCol *c, ColCon *k, const SiftKeyVal *v, bool incl)
{
	int			r = k->lo == NULL ? 1 : sift_key_val_cmp(c, v, k->lo);

	if (r > 0 || (r == 0 && !incl && k->lo_incl))
	{
		k->lo = v;
		k->lo_incl = incl;
	}
}

static void
tighten_hi(const SiftKeyCol *c, ColCon *k, const SiftKeyVal *v, bool incl)
{
	int			r = k->hi == NULL ? -1 : sift_key_val_cmp(c, v, k->hi);

	if (r < 0 || (r == 0 && !incl && k->hi_incl))
	{
		k->hi = v;
		k->hi_incl = incl;
	}
}

static int
set_find(const SiftKeyCol *c, const ColCon *k, const SiftKeyVal *v)
{
	for (int i = 0; i < k->nset; i++)
		if (sift_key_val_cmp(c, k->set[i], v) == 0)
			return i;
	return -1;
}

/* An equality or a short IN list: the values are the set, or those of the set that are in it too; a longer list is the range between its least and greatest value */
static void
add_set(const SiftKeyCol *c, ColCon *k, const SiftKeyVal *vals, int n)
{
	const SiftKeyVal *vs[SET_CAP];
	int			m = 0;
	const SiftKeyVal *least = NULL,
			   *most = NULL;

	for (int i = 0; i < n; i++)
	{
		if (vals[i].isnull)
			continue;
		if (least == NULL || sift_key_val_cmp(c, &vals[i], least) < 0)
			least = &vals[i];
		if (most == NULL || sift_key_val_cmp(c, &vals[i], most) > 0)
			most = &vals[i];
		if (m < SET_CAP)
			vs[m++] = &vals[i];
	}
	if (least == NULL)
	{
		k->empty = true;
		return;
	}
	if (n > SET_CAP)
	{
		tighten_lo(c, k, least, true);
		tighten_hi(c, k, most, true);
		return;
	}
	if (!k->has_set)
	{
		for (int i = 0; i < m; i++)
			if (set_find(c, k, vs[i]) < 0)
				k->set[k->nset++] = vs[i];
		k->has_set = true;
	}
	else
	{
		const SiftKeyVal *kept[SET_CAP];
		int			nk = 0;

		for (int i = 0; i < k->nset; i++)
			for (int j = 0; j < m; j++)
				if (sift_key_val_cmp(c, k->set[i], vs[j]) == 0)
				{
					kept[nk++] = k->set[i];
					break;
				}
		memcpy(k->set, kept, nk * sizeof(kept[0]));
		k->nset = nk;
	}
}

static void
add_pred(const SiftKeySpec *s, ColCon *k, const SiftKeyPred *p)
{
	const SiftKeyCol *c = &s->cols[p->col];

	switch (p->op)
	{
		case SIFT_KOP_EQ:
			add_set(c, k, p->vals, 1);
			break;
		case SIFT_KOP_IN:
			add_set(c, k, p->vals, p->nvals);
			break;
		case SIFT_KOP_LT:
		case SIFT_KOP_LE:
			if (!p->vals[0].isnull)
				tighten_hi(c, k, &p->vals[0], p->op == SIFT_KOP_LE);
			else
				k->empty = true;
			break;
		case SIFT_KOP_GT:
		case SIFT_KOP_GE:
			if (!p->vals[0].isnull)
				tighten_lo(c, k, &p->vals[0], p->op == SIFT_KOP_GE);
			else
				k->empty = true;
			break;
		case SIFT_KOP_BETWEEN:
			if (p->vals[0].isnull || p->vals[1].isnull)
				k->empty = true;
			else
			{
				tighten_lo(c, k, &p->vals[0], true);
				tighten_hi(c, k, &p->vals[1], true);
			}
			break;
		case SIFT_KOP_ISNULL:
			k->is_null = true;
			break;
		case SIFT_KOP_NOTNULL:
			k->not_null = true;
			break;
		case SIFT_KOP_PREFIX:
			if (c->kind == SIFT_KK_TEXT && !p->vals[0].isnull)
				k->pfx = &p->vals[0];
			break;
		default:
			break;
	}
}

static bool
starts_with(const SiftKeyVal *v, const SiftKeyVal *pfx)
{
	return v->len >= pfx->len && memcmp(v->p, pfx->p, pfx->len) == 0;
}

/* The constraints of the column made consistent with each other: the set cut to the bounds and the prefix, a text prefix beside a range its lower bound; empty when nothing can satisfy them */
static void
settle(const SiftKeyCol *c, ColCon *k)
{
	if (k->is_null && (k->has_set || k->lo != NULL || k->hi != NULL || k->not_null || k->pfx != NULL))
		k->empty = true;
	if (k->lo != NULL && k->hi != NULL)
	{
		int			r = sift_key_val_cmp(c, k->lo, k->hi);

		if (r > 0 || (r == 0 && !(k->lo_incl && k->hi_incl)))
			k->empty = true;
	}
	if (k->has_set)
	{
		int			m = 0;

		for (int i = 0; i < k->nset; i++)
		{
			const SiftKeyVal *v = k->set[i];
			int			rl = k->lo ? sift_key_val_cmp(c, v, k->lo) : 1,
						rh = k->hi ? sift_key_val_cmp(c, v, k->hi) : -1;

			if ((rl > 0 || (rl == 0 && k->lo_incl)) && (rh < 0 || (rh == 0 && k->hi_incl)) && (k->pfx == NULL || starts_with(v, k->pfx)))
				k->set[m++] = v;
		}
		k->nset = m;
		if (m == 0)
			k->empty = true;
		for (int i = 1; i < m; i++)
			for (int j = i; j > 0 && sift_key_val_cmp(c, k->set[j], k->set[j - 1]) < 0; j--)
			{
				const SiftKeyVal *t = k->set[j];

				k->set[j] = k->set[j - 1];
				k->set[j - 1] = t;
			}
	}
	else if (k->pfx != NULL && (k->lo != NULL || k->hi != NULL))
	{
		tighten_lo(c, k, k->pfx, true);
		k->pfx = NULL;
	}
}

/* ---- the ranges ---- */

typedef struct Point
{
	Bound		p;
} Point;

static void
emit(const SiftKeySpec *s, SiftKeyPlan *plan, const Bound *lo, const Bound *hi)
{
	SiftMarkRange *r;

	if (plan->n == SIFT_KEY_PLAN_MAX_RANGES)
		return;
	r = &plan->range[plan->n];
	memset(r, 0, sizeof(*r));
	if (lo != NULL)
	{
		r->has_lo = true;
		bound_image(s, lo, r->lo);
	}
	if (hi != NULL)
	{
		r->has_hi = true;
		r->hi_exact = !hi->trunc;
		bound_image(s, hi, r->hi);
	}
	if (r->has_lo && r->has_hi && r->hi_exact && memcmp(r->lo, r->hi, s->key_bytes) >= 0)
		return;
	plan->n++;
}

/* The keys that have the prefix p: [p, successor of p) */
static void
emit_prefix(const SiftKeySpec *s, SiftKeyPlan *plan, const Bound *p)
{
	Bound		hi = *p;

	if (p->trunc)
		emit(s, plan, p, p);
	else
		emit(s, plan, p, bound_succ(&hi) ? &hi : NULL);
}

/* The keys of the points followed by a value of column col in [lo, hi] (as the column orders its values: a descending column holds them the other way round in bytes) */
static void
emit_range(const SiftKeySpec *s, SiftKeyPlan *plan, const Point *pt, int col, const ColCon *k)
{
	const SiftKeyCol *c = &s->cols[col];
	bool		desc = (c->flags & SIFT_KEYCOL_DESC) != 0;
	Bound		lo = pt->p,
				hi = pt->p;
	bool		have_hi = true;
	const SiftKeyVal *vlow = desc ? k->hi : k->lo,
			   *vhigh = desc ? k->lo : k->hi;
	bool		low_incl = desc ? k->hi_incl : k->lo_incl,
				high_incl = desc ? k->lo_incl : k->hi_incl;

	if (pt->p.trunc)
	{
		emit(s, plan, &pt->p, &pt->p);
		return;
	}
	if (k->pfx != NULL)
	{
		uint8	   *tmp = palloc((Size) k->pfx->len * 2 + 8);
		uint32		n = sift_key_enc_val(c, k->pfx, tmp, k->pfx->len * 2 + 8, NULL);

		for (uint32 i = 0; i + 2 < n; i++)
			bound_put(s, &lo, tmp[i]);
		pfree(tmp);
		emit_prefix(s, plan, &lo);
		return;
	}
	if (vlow != NULL)
	{
		bound_enc(s, &lo, col, vlow);
		if (!low_incl && !bound_succ(&lo))
			return;
	}
	else
		bound_put(s, &lo, SIFT_KEY_VALUE_TAG);
	if (vhigh != NULL)
	{
		bound_enc(s, &hi, col, vhigh);
		if (high_incl)
			have_hi = bound_succ(&hi);
	}
	else
		bound_put(s, &hi, 0x02);
	emit(s, plan, &lo, have_hi ? &hi : NULL);
}

static int
mark_range_cmp(const void *a, const void *b)
{
	const SiftMarkRange *x = a,
			   *y = b;
	int			r = (int) x->has_lo - (int) y->has_lo;

	if (r == 0 && x->has_lo)
		r = memcmp(x->lo, y->lo, SIFT_KEY_BYTES_MAX);
	return r;
}

static bool
has_range(const ColCon *k)
{
	return k->lo != NULL || k->hi != NULL || k->not_null || k->pfx != NULL;
}

void
sift_key_plan_build(const SiftKeySpec *spec, const SiftKeyPred *preds, int npreds, SiftKeyPlan *plan)
{
	ColCon		con[SIFT_KEY_COLS_MAX];
	Point		pt[SIFT_KEY_PLAN_MAX_RANGES];
	int			np = 1,
				col;
	bool		stopped = false;

	*plan = (SiftKeyPlan) {.state = SIFT_KEY_PLAN_RANGES};
	memset(con, 0, sizeof(con));
	for (int i = 0; i < npreds; i++)
		if (preds[i].col < spec->ncols)
			add_pred(spec, &con[preds[i].col], &preds[i]);
	for (int i = 0; i < spec->ncols; i++)
		settle(&spec->cols[i], &con[i]);
	for (int i = 0; i < spec->ncols; i++)
		if (con[i].empty)
		{
			plan->state = SIFT_KEY_PLAN_EMPTY;
			return;
		}
	memset(pt, 0, sizeof(pt));
	for (col = 0; col < spec->ncols && !stopped; col++)
	{
		const ColCon *k = &con[col];

		if (k->has_set && np * k->nset <= SET_CAP)
		{
			Point		next[SIFT_KEY_PLAN_MAX_RANGES];
			int			nn = 0;

			for (int i = 0; i < np; i++)
				for (int j = 0; j < k->nset; j++)
				{
					next[nn] = pt[i];
					bound_enc(spec, &next[nn].p, col, k->set[j]);
					nn++;
				}
			memcpy(pt, next, nn * sizeof(Point));
			np = nn;
		}
		else if (k->is_null)
		{
			SiftKeyVal	nul = {.isnull = true};

			for (int i = 0; i < np; i++)
				bound_enc(spec, &pt[i].p, col, &nul);
		}
		else if (k->has_set || has_range(k))
		{
			ColCon		hull = *k;

			if (k->has_set)
			{
				hull.lo = k->set[0];
				hull.hi = k->set[k->nset - 1];
				hull.lo_incl = hull.hi_incl = true;
			}
			for (int i = 0; i < np; i++)
				emit_range(spec, plan, &pt[i], col, &hull);
			plan->range_col = true;
			stopped = true;
			break;
		}
		else
		{
			stopped = true;
			break;
		}
		plan->prefix_cols = col + 1;
	}
	if (!plan->range_col)
	{
		if (plan->prefix_cols == 0)
		{
			plan->state = SIFT_KEY_PLAN_FULL;
			return;
		}
		for (int i = 0; i < np; i++)
			emit_prefix(spec, plan, &pt[i].p);
	}
	if (plan->n == 0)
		plan->state = SIFT_KEY_PLAN_EMPTY;
	qsort(plan->range, plan->n, sizeof(SiftMarkRange), mark_range_cmp);
}

bool
sift_mark_range_has(const SiftMarkRange *r, const uint8 *key, uint8 key_bytes)
{
	int			c;

	if (r->has_lo && memcmp(key, r->lo, key_bytes) < 0)
		return false;
	if (!r->has_hi)
		return true;
	c = memcmp(key, r->hi, key_bytes);
	return c < 0 || (c == 0 && !r->hi_exact);
}

/* The mark compared is the last key of the granule, or where there are first keys only the next granule's first: no row of the granule is above it */
static bool
below_range(const SiftKeyMarks *km, uint32 g, const SiftMarkRange *r)
{
	const uint8 *top = sift_key_marks_hi(km, g);

	if (!r->has_lo)
		return false;
	if (top == NULL)
	{
		if (g + 1 >= km->ngranules)
			return false;
		top = sift_key_marks_lo(km, g + 1);
	}
	return memcmp(top, r->lo, km->key_bytes) < 0;
}

/* Every row of the granule is at or above the end of the range; a granule that starts at an end that was not cut is outside it, one that starts at a cut end may hold a row inside */
static bool
above_range(const SiftKeyMarks *km, uint32 g, const SiftMarkRange *r)
{
	int			c;

	if (!r->has_hi)
		return false;
	c = memcmp(sift_key_marks_lo(km, g), r->hi, km->key_bytes);
	return c > 0 || (c == 0 && r->hi_exact && km->exact);
}

static uint32
first_not_below(const SiftKeyMarks *km, const SiftMarkRange *r)
{
	uint32		lo = 0,
				hi = km->ngranules;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (below_range(km, mid, r))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static uint32
first_above(const SiftKeyMarks *km, const SiftMarkRange *r)
{
	uint32		lo = 0,
				hi = km->ngranules;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (above_range(km, mid, r))
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

int
sift_key_marks_spans(const SiftKeyMarks *km, const SiftKeyPlan *plan, uint32 spans[][2])
{
	int			n = 0;

	if (plan->state == SIFT_KEY_PLAN_EMPTY || km->ngranules == 0)
		return 0;
	if (plan->state == SIFT_KEY_PLAN_FULL)
	{
		spans[0][0] = 0;
		spans[0][1] = km->ngranules;
		return 1;
	}
	for (int i = 0; i < plan->n; i++)
	{
		uint32		g0 = first_not_below(km, &plan->range[i]),
					g1 = first_above(km, &plan->range[i]);

		if (g0 >= g1)
			continue;
		if (n > 0 && g0 <= spans[n - 1][1])
			spans[n - 1][1] = Max(spans[n - 1][1], g1);
		else
		{
			spans[n][0] = g0;
			spans[n][1] = g1;
			n++;
		}
	}
	return n;
}

/* ---- a row comparison ---- */

/* The least image of a bound above the keys of the plan's range r that it is cut to (lo), or the greatest (hi) */
static void
range_tighten(const SiftKeySpec *s, SiftMarkRange *r, const Bound *lo, const Bound *hi)
{
	uint8		img[SIFT_KEY_BYTES_MAX];

	if (lo != NULL)
	{
		bound_image(s, lo, img);
		if (!r->has_lo || memcmp(img, r->lo, SIFT_KEY_BYTES_MAX) > 0)
		{
			memcpy(r->lo, img, SIFT_KEY_BYTES_MAX);
			r->has_lo = true;
		}
	}
	if (hi != NULL)
	{
		bound_image(s, hi, img);
		if (!r->has_hi || memcmp(img, r->hi, SIFT_KEY_BYTES_MAX) < 0)
		{
			memcpy(r->hi, img, SIFT_KEY_BYTES_MAX);
			r->has_hi = true;
			r->hi_exact = !hi->trunc;
		}
		else if (memcmp(img, r->hi, SIFT_KEY_BYTES_MAX) == 0)
			r->hi_exact &= !hi->trunc;
	}
}

void
sift_key_plan_row_bound(const SiftKeySpec *spec, SiftKeyPlan *plan, int op, const SiftKeyVal *vals, int n)
{
	const bool	desc = (spec->cols[0].flags & SIFT_KEYCOL_DESC) != 0,
				incl = op == SIFT_KOP_LE || op == SIFT_KOP_GE,
				upper = (op == SIFT_KOP_LT || op == SIFT_KOP_LE) != desc;
	Bound		p = {0};
	SiftMarkRange base = {0},
			   *r;
	int			kept = 0;

	if (plan->state == SIFT_KEY_PLAN_EMPTY || n < 1 || n > spec->ncols)
		return;
	for (int k = 0; k < n; k++)
		if (vals[k].isnull || ((spec->cols[k].flags & SIFT_KEYCOL_DESC) != 0) != desc)
			return;
	for (int k = 0; k < n; k++)
		bound_enc(spec, &p, k, &vals[k]);
	if (upper == incl && !bound_succ(&p))
	{
		if (!upper)
			plan->state = SIFT_KEY_PLAN_EMPTY;
		return;
	}
	if (plan->state == SIFT_KEY_PLAN_FULL)
	{
		plan->state = SIFT_KEY_PLAN_RANGES;
		plan->n = 1;
		plan->range[0] = base;
	}
	for (int i = 0; i < plan->n; i++)
	{
		r = &plan->range[i];
		range_tighten(spec, r, upper ? NULL : &p, upper ? &p : NULL);
		if (!(r->has_lo && r->has_hi && r->hi_exact && memcmp(r->lo, r->hi, SIFT_KEY_BYTES_MAX) >= 0))
			plan->range[kept++] = *r;
	}
	plan->n = kept;
	plan->state = kept == 0 ? SIFT_KEY_PLAN_EMPTY : SIFT_KEY_PLAN_RANGES;
}
