/*
 * col/f2/prune.c - what a scan of an index of format major 2 leaves unread: the key range its predicates confine the lead key column to, the partitions and parts whose bounds
 * lie outside it, and the granules of a part whose zones do. A part is sorted by (key, _tid), so the zones of the lead key column are in order over its granules (ascending or descending, the NULLs first or last, as key_order says): the granules
 * that can hold the range are found by two binary searches, each probe a read of one 24-byte zone, whatever the number of granules.
 */
#include "postgres.h"

#include "utils/timestamp.h"

#include "col/f2/compact/compact.h"
#include "col/f2/f2.h"
#include "col/f2/key_sig.h"
#include "col/f2/marks.h"

/* The integers the predicates on the lead key column allow, and whether they want the NULLs (which sort after every integer, or before when key_order says NULLS FIRST) or nothing else */
typedef struct LeadRange
{
	int64		lo;
	int64		hi;
	bool		ints_only;
	bool		null_only;
} LeadRange;

static void
lead_empty(LeadRange *r)
{
	r->lo = PG_INT64_MAX;
	r->hi = PG_INT64_MIN;
	r->ints_only = true;
}

static void
lead_narrow(LeadRange *r, const SiftColPred *p)
{
	int64		v = p->nvals > 0 ? p->vals[0].v.i : 0;

	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			r->lo = Max(r->lo, v);
			r->hi = Min(r->hi, v);
			break;
		case SIFT_COLOP_LT:
			if (v == PG_INT64_MIN)
				lead_empty(r);
			else
				r->hi = Min(r->hi, v - 1);
			break;
		case SIFT_COLOP_LE:
			r->hi = Min(r->hi, v);
			break;
		case SIFT_COLOP_GT:
			if (v == PG_INT64_MAX)
				lead_empty(r);
			else
				r->lo = Max(r->lo, v + 1);
			break;
		case SIFT_COLOP_GE:
			r->lo = Max(r->lo, v);
			break;
		case SIFT_COLOP_IN:
			{
				int64		lo = PG_INT64_MAX,
							hi = PG_INT64_MIN;

				for (int j = 0; j < p->nvals; j++)
				{
					lo = Min(lo, p->vals[j].v.i);
					hi = Max(hi, p->vals[j].v.i);
				}
				r->lo = Max(r->lo, lo);
				r->hi = Min(r->hi, hi);
				break;
			}
		case SIFT_COLOP_ISNULL:
			r->null_only = true;
			return;
		case SIFT_COLOP_NOTNULL:
			break;
		default:
			return;
	}
	r->ints_only = true;
}

/* The bound that has the lead component alone: the integer or the NULL as the lead column is ordered (flags), or the least bound above all such */
static bool
pack_prefix(int64 v, bool null, bool successor, unsigned flags, uint8 *out)
{
	SiftBound	b,
				next;

	sift_bound_clear(&b);
	if (null)
		(void) sift_bound_push_key_null(&b, flags);
	else
		(void) sift_bound_push_key_i64(&b, v, flags);
	if (successor && !sift_bound_prefix_next(&b, &next))
		return false;
	sift_bound_pack(successor ? &next : &b, out);
	return true;
}

/* The keys of a range of the lead column's integers: a descending column holds them from the greatest down, so the first key of the range is that of its greatest integer; has_lo is false where nothing sorts below it */
static void
range_of(const LeadRange *r, unsigned flags, SiftF2KeyRange *k)
{
	bool		desc = flags & SIFT_KEY_DESC,
				open_below = desc ? r->hi == PG_INT64_MAX : r->lo == PG_INT64_MIN;

	*k = (SiftF2KeyRange) {.ilo = r->lo,.ihi = r->hi,.null_only = r->null_only};
	if (r->null_only)
	{
		k->has_lo = pack_prefix(0, true, false, flags, k->lo);
		k->has_hi = pack_prefix(0, true, true, flags, k->hi);
	}
	else if (r->ints_only)
	{
		k->has_lo = ((flags & SIFT_KEY_NULLS_FIRST) || !open_below) && pack_prefix(desc ? r->hi : r->lo, false, false, flags, k->lo);
		k->has_hi = pack_prefix(desc ? r->lo : r->hi, false, true, flags, k->hi);
	}
}

static int
i64_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

static int
i64_cmp_desc(const void *a, const void *b)
{
	return i64_cmp(b, a);
}

/* The values of the one IN list on the lead key column that is short enough to be probed one at a time, in the order of the column (so that their granules ascend), inside [lo, hi]; -1 when there is none */
static int
lead_in_values(const SiftF2Rel *f, const SiftColPred *preds, int npreds, const LeadRange *r, int64 *vals)
{
	int			n = -1;

	for (int i = 0; i < npreds; i++)
		if (preds[i].col == f->key_cols[0] && preds[i].op == SIFT_COLOP_IN && preds[i].nvals <= SIFT_F2_MAX_RANGES && (n < 0 || preds[i].nvals < n))
		{
			n = 0;
			for (int j = 0; j < preds[i].nvals; j++)
				if (preds[i].vals[j].v.i >= r->lo && preds[i].vals[j].v.i <= r->hi)
					vals[n++] = preds[i].vals[j].v.i;
		}
	if (n > 1)
	{
		int			m = 1;

		qsort(vals, n, sizeof(int64), (f->spec.sort_flags[0] & SIFT_KEY_DESC) ? i64_cmp_desc : i64_cmp);
		for (int i = 1; i < n; i++)
			if (vals[i] != vals[m - 1])
			{
				SIFT_INVARIANT(m <= i, "the distinct values are compacted in place behind the one read");
				vals[m++] = vals[i];
			}
		n = m;
	}
	return n;
}

/* The ranges of the lead key column; the hull of the partitions only when every part is in the key, as the partitions are cut by the key of the rows routed to them */
static bool
key_ranges(const SiftF2Rel *f, const SiftColPred *preds, int npreds, SiftF2KeySet *ks)
{
	LeadRange	r = {.lo = PG_INT64_MIN,.hi = PG_INT64_MAX};
	int64		vals[SIFT_F2_MAX_RANGES];
	int			nv;
	unsigned	flags = f->nkey > 0 ? f->spec.sort_flags[0] : 0;

	if (sift_spec_int_keys(&f->spec) == 0)
	{
		ks->n = 1;
		return true;
	}
	for (int i = 0; i < npreds; i++)
		if (preds[i].col == f->key_cols[0])
			lead_narrow(&r, &preds[i]);
	if ((r.null_only && r.ints_only) || r.lo > r.hi)
		return false;
	range_of(&r, flags, &ks->hull);
	nv = lead_in_values(f, preds, npreds, &r, vals);
	if (nv == 0)
		return false;
	if (nv < 0)
	{
		ks->n = 1;
		ks->range[0] = ks->hull;
		return true;
	}
	for (int i = 0; i < nv; i++)
		range_of(&(LeadRange) {.lo = vals[i],.hi = vals[i],.ints_only = true}, flags, &ks->range[ks->n++]);
	return true;
}

bool
sift_f2_key_set(Relation rel, const SiftF2Rel *f, const SiftColPred *preds, int npreds, SiftF2KeySet *ks)
{
	*ks = (SiftF2KeySet) {.trusted_gen = sift_f2_key_trusted_gen(rel, sift_f2_spec_key_sig(&f->spec))};
	if (!key_ranges(f, preds, npreds, ks))
		return false;
	if (ks->trusted_gen != 0)
		ks->hull = (SiftF2KeyRange) {.ilo = PG_INT64_MIN,.ihi = PG_INT64_MAX};
	return true;
}

static bool
key_range_meets(const SiftPartDesc *d, const SiftF2KeyRange *k)
{
	return !(k->has_lo && memcmp(d->key_hi, k->lo, SIFT_BOUND_BYTES) < 0) && !(k->has_hi && memcmp(d->key_lo, k->hi, SIFT_BOUND_BYTES) >= 0);
}

bool
sift_f2_part_overlaps(const SiftPartDesc *d, const SiftF2KeySet *ks)
{
	if (d->part_gen < ks->trusted_gen)
		return true;
	for (int i = 0; i < ks->n; i++)
		if (key_range_meets(d, &ks->range[i]))
			return true;
	return false;
}

/*
 * The granules of a part are in the order of the lead key column: ascending or descending, the NULLs before or after the integers. A probe is false up to the first granule that
 * is, and true from there on. The zone of a granule of NULLs alone has no integers (SIFT_ZONE_NONE): it is before the range of integers when the NULLs come first, after it otherwise.
 */
typedef bool (*ZoneProbe) (const SiftZone *z, const SiftF2KeyRange *k, unsigned flags);

static bool
holds_nulls(const SiftZone *z, const SiftF2KeyRange *k, unsigned flags)
{
	return z->nnull > 0;
}

static bool
holds_no_nulls(const SiftZone *z, const SiftF2KeyRange *k, unsigned flags)
{
	return z->nnull == 0;
}

/* May the granule hold an integer of the range, or one that comes after it in the order of the column */
static bool
reaches_range(const SiftZone *z, const SiftF2KeyRange *k, unsigned flags)
{
	if (z->flags & SIFT_ZONE_NONE)
		return !(flags & SIFT_KEY_NULLS_FIRST);
	return (flags & SIFT_KEY_DESC) ? z->min <= k->ihi : z->max >= k->ilo;
}

/* Is every integer of the granule after the range in the order of the column */
static bool
passes_range(const SiftZone *z, const SiftF2KeyRange *k, unsigned flags)
{
	if (z->flags & SIFT_ZONE_NONE)
		return !(flags & SIFT_KEY_NULLS_FIRST);
	return (flags & SIFT_KEY_DESC) ? z->max < k->ilo : z->min > k->ihi;
}

/* The first granule whose zone of the column satisfies probe, given that every later granule does too (ng when none does) */
static uint32
first_true(Relation rel, const SiftPart *part, const SiftSecDirEntry *zs, int col, const SiftF2KeyRange *k, unsigned flags, ZoneProbe probe)
{
	uint32		lo = 0,
				hi = part->hdr.ngranules;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		SiftZone	z;

		sift_part_read_section_bytes(rel, part, zs, ((uint64) col * part->hdr.ngranules + mid) * sizeof(z), sizeof(z), &z);
		if (probe(&z, k, flags))
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

static bool
range_span(Relation rel, const SiftPart *part, const SiftSecDirEntry *zs, int col, const SiftF2KeyRange *k, unsigned flags, uint32 *g0, uint32 *g1)
{
	uint32		ng = part->hdr.ngranules;

	*g0 = 0;
	*g1 = ng;
	if (k->null_only && (flags & SIFT_KEY_NULLS_FIRST))
		*g1 = first_true(rel, part, zs, col, k, flags, holds_no_nulls);
	else if (k->null_only)
		*g0 = first_true(rel, part, zs, col, k, flags, holds_nulls);
	else if (k->has_lo || k->has_hi)
	{
		*g0 = k->has_lo ? first_true(rel, part, zs, col, k, flags, reaches_range) : 0;
		*g1 = k->has_hi ? first_true(rel, part, zs, col, k, flags, passes_range) : ng;
	}
	return *g0 < *g1;
}

int
sift_f2_granule_spans(Relation rel, const SiftPart *part, const SiftF2Rel *f, const SiftF2KeySet *ks, const SiftKeyPlan *plan, uint32 spans[][2])
{
	const SiftSecDirEntry *zs = sift_part_section(part, SEC_ZONES);
	const bool	in_key = part->hdr.part_gen >= ks->trusted_gen;
	const SiftKeyMarks *km = in_key && plan->state == SIFT_KEY_PLAN_RANGES ? sift_part_key_marks(rel, part, f) : NULL;
	uint32		ng = part->hdr.ngranules;
	int			n = 0;

	if (plan->state == SIFT_KEY_PLAN_EMPTY)
		return 0;
	if (km != NULL)
		return sift_key_marks_spans(km, plan, spans);
	if (sift_spec_int_keys(&f->spec) == 0 || !in_key || zs == NULL || zs->len != (uint64) part->hdr.ncols * ng * sizeof(SiftZone))
	{
		spans[0][0] = 0;
		spans[0][1] = ng;
		return ng > 0;
	}
	for (int i = 0; i < ks->n; i++)
	{
		uint32		g0,
					g1;

		if (!range_span(rel, part, zs, f->key_cols[0], &ks->range[i], f->spec.sort_flags[0], &g0, &g1))
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

bool
sift_f2_time_window(const SiftF2Rel *f, const SiftColPred *preds, int npreds, SiftF2TimeWin *w)
{
	*w = (SiftF2TimeWin) {0};
	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
	{
		LeadRange	r = {.lo = PG_INT64_MIN,.hi = PG_INT64_MAX};
		int			col = f->spec.time_cols[s] - 1;

		if (col < 0)
			continue;
		for (int i = 0; i < npreds; i++)
			if (preds[i].col == col)
				lead_narrow(&r, &preds[i]);
		if ((r.null_only && r.ints_only) || r.lo > r.hi)
			return false;
		w->use[s] = r.ints_only && !r.null_only;
		w->lo[s] = r.lo;
		w->hi[s] = r.hi;
	}
	return true;
}

bool
sift_f2_entry_meets_time(const SiftPartitionEntry *e, const SiftF2TimeWin *w)
{
	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
		if (w->use[s] && (e->flags & (s == 0 ? SIFT_PARTENTRY_HAS_TIME0 : SIFT_PARTENTRY_HAS_TIME1)) && !sift_time_meets(e->time_min[s], e->time_max[s], w->lo[s], w->hi[s]))
			return false;
	return true;
}

bool
sift_f2_part_meets_time(const SiftPartDesc *d, const SiftF2TimeWin *w)
{
	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
		if (w->use[s] && (d->flags & (s == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1)) && !sift_time_meets(d->time_min[s], d->time_max[s], w->lo[s], w->hi[s]))
			return false;
	return true;
}

int64
sift_f2_hot_cut(const SiftF2Rel *f)
{
	TimestampTz now = GetCurrentTimestamp();

	if (f->time_unit[0] == 0)
		return PG_INT64_MIN;
	return (now / USECS_PER_DAY - SIFT_HOT_WINDOW_DAYS) * USECS_PER_DAY / f->time_unit[0];
}
