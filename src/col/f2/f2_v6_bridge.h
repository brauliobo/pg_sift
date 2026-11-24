/*
 * col/f2/f2_v6_bridge.h - what the index of format major 2 gives the operators of SiftAggV6 (agg/v6/v6_vexpr.h): the format that owns an index, says what it holds and how it is sorted,
 * opens its batches (f2_v6_bridge.c), the Top-N tournament source (f2_v6_topn.c) and the statistics for the planner (f2_v6_card.c). All of it is bound once at load by sift_f2_v6_format_init() (f2_v6_format.c).
 *
 * The functions below that take no server are the decisions the sources make about the order of a part: whether the leading key of a part reads in the order the query asks, or in the
 * reverse of it, and which granule of such a part is read first and which next. They are shared by the sources and by the standalone tests.
 */
#ifndef SIFT_COL_F2_V6_BRIDGE_H
#define SIFT_COL_F2_V6_BRIDGE_H

#include "postgres.h"

#include "agg/v6/v6_card_est.h"
#include "agg/v6/v6_topn.h"
#include "agg/v6/v6_vexpr.h"
#include "col/f2/f2_v6_read.h"

typedef enum SiftF2OrderMatch
{
	SF2_ORDER_NONE,				/* the part is not in an order the query reads in one direction */
	SF2_ORDER_EXACT,			/* the part reads in the order of the query: its first granule has the best rows */
	SF2_ORDER_REVERSE			/* the part reads in the reverse of it: its last granule has the best rows, and the granules are read backwards */
} SiftF2OrderMatch;

/* The leading key of a part is sorted (desc, NULLs first) and the query asks (want_desc, want_nulls_first): NULLs are at one end of a part, so the direction and the place of the NULLs both match or both reverse */
static inline SiftF2OrderMatch
sift_f2_order_match(bool part_desc, bool part_nulls_first, bool want_desc, bool want_nulls_first)
{
	if (part_desc == want_desc && part_nulls_first == want_nulls_first)
		return SF2_ORDER_EXACT;
	if (part_desc != want_desc && part_nulls_first != want_nulls_first)
		return SF2_ORDER_REVERSE;
	return SF2_ORDER_NONE;
}

/* The granule a frontier starts at, of ngranules >= 1 */
static inline uint32
sift_f2_frontier_first(SiftF2OrderMatch m, uint32 ngranules)
{
	return m == SF2_ORDER_REVERSE ? ngranules - 1 : 0;
}

/* The granule after g in the order the part is read; false at the end of the part */
static inline bool
sift_f2_frontier_next(SiftF2OrderMatch m, uint32 g, uint32 ngranules, uint32 *next)
{
	if (m == SF2_ORDER_REVERSE)
	{
		if (g == 0)
			return false;
		*next = g - 1;
		return true;
	}
	if (g + 1 >= ngranules)
		return false;
	*next = g + 1;
	return true;
}

/*
 * The integers the leading key column holds in the keys [lo, hi] of a partition or a part (their packed bounds in the manifest), whatever the direction of the column (flags: SIFT_KEY_*).
 * Both bounds must hold an integer there: a bound that is unbounded or NULL says nothing of the range, and then no row of the unit is a NULL, which is why the range is the whole of it.
 */
extern bool sift_f2_v6_lead_range(const uint8 *lo, const uint8 *hi, unsigned flags, int64 *min, int64 *max);

/* The bound of a unit from the integers its bounds give the leading key (the range of the values that are not NULL); a unit whose NULLs are not told may hold them */
static inline SiftTournBound
sift_f2_range_bound(int64 min, int64 max, bool has_null, bool all_null)
{
	return (SiftTournBound) {.known = true,.has_null = has_null || all_null,.all_null = all_null,.min = min,.max = max};
}

/* ---- the statistics the planner gets (f2_v6_card.c folds them from the sections of the parts; the folds are plain arithmetic) ---- */

typedef struct SiftF2ZoneFold
{
	int64		min;
	int64		max;
	bool		any;			/* a zone with a value that is not NULL was added */
	uint64		rows;
	uint64		null_rows;
} SiftF2ZoneFold;

/* The zone of a granule of nrows rows joins the fold; a zone of NULLs only adds to the NULL count */
static inline void
sift_f2_zone_fold_add(SiftF2ZoneFold *f, const SiftCardZone *z, uint32 nrows)
{
	f->rows += nrows;
	f->null_rows += Min(z->nnull, nrows);
	if (z->nnull >= nrows)
		return;
	f->min = f->any ? Min(f->min, z->min) : z->min;
	f->max = f->any ? Max(f->max, z->max) : z->max;
	f->any = true;
}

/* Every stride-th granule of ngranules keeps the sample under max_sample granules: the granules 0, stride, 2 * stride ... */
static inline uint32
sift_f2_sample_stride(uint64 ngranules, uint32 max_sample)
{
	return (uint32) Max((ngranules + max_sample - 1) / max_sample, 1);
}

/*
 * The distinct values of a column over all the parts from those of the parts that were read: never below the part with the most, and each of the others adds half of what it has beyond that
 * (the parts overlap in their values), scaled by the rows the parts not read have over those read; never above the sum scaled so, nor the rows.
 */
static inline double
sift_f2_ndv_union(double max_part, double sum_parts, double read_rows, double total_rows)
{
	const double scale = read_rows > 0 ? Max(total_rows / read_rows, 1.0) : 1.0;
	double		est = max_part + 0.5 * (sum_parts - max_part) * scale;

	est = Min(est, sum_parts * scale);
	est = Min(est, total_rows);
	return Max(est, Min(max_part, total_rows));
}

static inline int64
sift_f2_mid(int64 lo, int64 hi)
{
	return (lo >> 1) + (hi >> 1) + (lo & hi & 1);
}

/*
 * How the zones of the sample (n zones in the order of the parts, nrows[i] rows each) lie: overlap, the mean share of the whole range [gmin, gmax] a zone spans (0: the zones follow one
 * another, 1: every zone spans all of it), and monotonicity, the share of the pairs that follow one another by their lower bound that rise less those that fall (-1 .. 1).
 */
static inline void
sift_f2_zone_shape(const SiftCardZone *z, const uint32 *nrows, uint32 n, int64 gmin, int64 gmax, double *overlap, double *mono)
{
	const double range = (double) gmax - (double) gmin;
	double		span = 0;
	int64		pairs = 0,
				net = 0;
	const SiftCardZone *prev = NULL;
	uint32		used = 0;

	for (uint32 i = 0; i < n; i++)
	{
		if (z[i].nnull >= nrows[i])
			continue;
		span += range > 0 ? ((double) z[i].max - (double) z[i].min) / range : 1.0;
		used++;
		if (prev != NULL)
		{
			pairs++;
			net += z[i].min > prev->min ? 1 : z[i].min < prev->min ? -1 : 0;
		}
		prev = &z[i];
	}
	*overlap = used > 0 ? Min(span / used, 1.0) : 0.0;
	*mono = pairs > 0 ? (double) net / pairs : 0.0;
}

typedef struct SiftF2MidW
{
	int64		mid;
	uint32		w;
} SiftF2MidW;

static inline int
sift_f2_midw_cmp(const void *x, const void *y)
{
	const SiftF2MidW *a = x,
			   *b = y;

	return a->mid < b->mid ? -1 : a->mid > b->mid;
}

/*
 * The fences of SIFT_CARD_HIST_BUCKETS equal-depth buckets from the middles of the zones, each weighted by its rows that are not NULL: bounds[0] and bounds[16] are the range, the rest the
 * middle that holds the quantile; 0 when fewer than two zones have values (no histogram). scratch holds n entries.
 */
static inline int
sift_f2_zone_hist(const SiftCardZone *z, const uint32 *nrows, uint32 n, int64 gmin, int64 gmax, int64 *bounds, SiftF2MidW *scratch)
{
	uint64		total = 0,
				acc = 0;
	uint32		m = 0,
				at = 0;

	for (uint32 i = 0; i < n; i++)
		if (z[i].nnull < nrows[i])
		{
			scratch[m++] = (SiftF2MidW) {.mid = sift_f2_mid(z[i].min, z[i].max),.w = nrows[i] - z[i].nnull};
			total += nrows[i] - z[i].nnull;
		}
	if (m < 2)
		return 0;
	qsort(scratch, m, sizeof(SiftF2MidW), sift_f2_midw_cmp);
	bounds[0] = gmin;
	for (int j = 1; j < SIFT_CARD_HIST_BUCKETS; j++)
	{
		const uint64 want = total * j / SIFT_CARD_HIST_BUCKETS;

		while (at + 1 < m && acc + scratch[at].w <= want)
			acc += scratch[at++].w;
		bounds[j] = Max(Min(scratch[at].mid, gmax), bounds[j - 1]);
	}
	bounds[SIFT_CARD_HIST_BUCKETS] = gmax;
	return SIFT_CARD_HIST_BUCKETS;
}

/* The batches of the input over a sift_col index (SiftStoreOps.open of an index): the partitions and parts the bounds of the quals leave, in the order of the keys when sorted */
extern SiftV6Source *sift_f2_v6_index_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted);

/*
 * The rows of granules judged as a source judges them (the Top-N source reads granules by its own order): the quals of the input and the heap. The leaves are those the input, its masks and
 * the sort key of the index need; the window, the bounds and the zones of the quals' bounds tell what a unit can hold.
 */
typedef struct F2Src F2Eval;

extern F2Eval *sift_f2_v6_eval_new(MemoryContext cx, F2V6Ctx *c, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted);
extern const int *sift_f2_v6_eval_leaves(const F2Eval *e, int *n);
extern const SiftF2TimeWin *sift_f2_v6_eval_window(const F2Eval *e);
extern int	sift_f2_v6_eval_nbounds(const F2Eval *e);
extern int	sift_f2_v6_eval_bound_leaf(const F2Eval *e, int b);
extern bool sift_f2_v6_ruled_out(const F2Eval *e, SiftZone *const *zones, uint32 g, uint32 nrows);

/* What the bounds of the quals on the leading key leave of the partitions and parts: the units no row of which can pass are not opened, and counted */
extern bool sift_f2_v6_eval_entry_meets_keys(const F2Eval *e, const SiftPartitionEntry *ent);
extern bool sift_f2_v6_eval_part_meets_keys(const F2Eval *e, const SiftPartDesc *d);
extern SiftV6SkipStats *sift_f2_v6_eval_skips(F2Eval *e);
extern uint32 sift_f2_v6_eval_rows(F2Eval *e, const F2V6Gran *g, const uint64 *cand, uint64 *pass);

/* The Top-N tournament source over the index (SiftV6TopNOpen of agg/v6/v6_topn.h) */
extern bool sift_f2_v6_topn_open(EState *estate, const SiftV6TopNSpec *spec, const SiftV6Input *in, const SiftV6Vexpr *quals, SiftV6TopNSource *dst);

/* The statistics of the index folded when its parts were written, cached with the plan snapshot of the index; NULL when it has none (f2_v6_card.c) */
extern const SiftCardStatsView *sift_f2_v6_card_view(Oid indexoid);

/*
 * The leaf of the index that holds the key path of a jsonb attribute (doc->>'key' read as a scalar of class cls); false when the index shredded no such leaf. For the operators that read
 * a key of a document apart from the document.
 */
extern bool sift_f2_v6_resolve_path(Oid indexoid, int att, const char *path, SiftV6Class cls, int *leaf_out);

/*
 * The attributes atts[0 .. n-1] of the index against the order its parts keep (ascending, NULLs last: the sort the operators of SiftAggV6 ask for), as the number of leading attributes
 * that match, and for the leading key the direction and the place of its NULLs (want_desc, want_nulls_first) against the parts': a Top-N or a window may read a part backwards.
 */
extern SiftF2OrderMatch sift_f2_v6_order_match(Oid indexoid, int att, bool want_desc, bool want_nulls_first);

#endif							/* SIFT_COL_F2_V6_BRIDGE_H */
