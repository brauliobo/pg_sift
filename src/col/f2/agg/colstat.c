/*
 * agg/v6_colstat.c - the visibility map of a relation and the totals of the columns of its parts (agg/v6_colstat.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "port/pg_bitutils.h"

#include "agg/group/int.h"
#include "col/am.h"
#include "col/f2/agg/colstat.h"
#include "col/f2/part/vis.h"
#include "core/cachebudget.h"

/* ---- the visibility map ---- */

void
sift_v6_vis_init(V6VisMap *m, MemoryContext cx)
{
	*m = (V6VisMap) {.cx = cx};
}

static void
vis_build(V6VisMap *m, Relation heap)
{
	MemoryContext old;

	if (m->built)
		return;
	old = MemoryContextSwitchTo(m->cx);
	sift_vm_dirty_build(&m->dirty, heap, false);
	MemoryContextSwitchTo(old);
	m->built = true;
}

void
sift_v6_vis_ask_index(SiftV6Vet *v, Relation index)
{
	const SiftColFormatOps *ops = sift_col_ops(index);

	if (ops->deletes_heard != NULL && ops->deletes_heard(index, v->heap, v->snap))
		v->heard_xmin = sift_vis_snap_xmin(v->snap);
}

bool
sift_v6_vis_part_settled(const SiftV6Vet *v, const SiftPartDesc *d)
{
	return d != NULL && sift_vis_part_inserts_settled(d, v->heard_xmin);
}

bool
sift_v6_vis_part_all_visible(V6VisMap *m, Relation heap, const SiftPartDesc *d)
{
	if (d->tid_max == 0)
		return false;
	vis_build(m, heap);
	return !sift_vm_dirty_overlaps(&m->dirty, (BlockNumber) (d->tid_min >> 16), (BlockNumber) (d->tid_max >> 16));
}

int
sift_v6_vis_dirty_blocks(V6VisMap *m, Relation heap, const SiftPartDesc *d, BlockNumber *out, int max)
{
	const BlockNumber lo = (BlockNumber) (d->tid_min >> 16),
				hi = (BlockNumber) (d->tid_max >> 16);
	const SiftBlkInterval *iv;
	int			n = 0,
				a = 0,
				b;

	if (d->tid_max == 0)
		return -1;
	vis_build(m, heap);
	iv = m->dirty.intervals;
	b = m->dirty.nintervals;
	if (m->dirty.all_dirty || hi >= m->dirty.nheap_blocks)
		return -1;
	while (a < b)
	{
		const int	mid = a + (b - a) / 2;

		if (iv[mid].hi < lo)
			a = mid + 1;
		else
			b = mid;
	}
	for (; a < m->dirty.nintervals && iv[a].lo <= hi; a++)
		for (BlockNumber blk = Max(lo, iv[a].lo); blk <= Min(hi, iv[a].hi); blk++)
		{
			if (n == max)
				return -1;
			out[n++] = blk;
		}
	return n;
}

void
sift_v6_vis_free(V6VisMap *m)
{
	if (m->built)
		sift_vm_dirty_free(&m->dirty);
	m->built = false;
}

/* ---- what a spec asks of whole parts ---- */

static bool
integer_arg(const SiftV6Col *c)
{
	return c->derive == 0 && c->jleaf == 0 && c->scale == 0 && (c->typid == INT2OID || c->typid == INT4OID || c->typid == INT8OID);
}

static bool
plain_spec(const SiftV6Spec *spec)
{
	return spec->naggs > 0 && spec->vqual == NIL && spec->nkeys == 0 && spec->nsets == 0 && spec->nbins == 0 && spec->nfilters == 0 && spec->nderive == 0 && !spec->affine && spec->ext == NULL && !spec->constant_groups;
}

bool
sift_v6_spec_minmax_only(const SiftV6Spec *spec)
{
	bool		only = plain_spec(spec);

	for (int j = 0; only && j < spec->naggs; j++)
	{
		const SiftV6Agg *a = &spec->agg[j];

		only = (a->kind == SV6_MIN || a->kind == SV6_MAX) && a->fclause == 0 && a->arg.jleaf == 0 && a->arg.scale == 0 && sift_v6_class(a->arg.typid) != SV6C_F64 &&
			sift_v6_class(a->arg.typid) != SV6C_TEXT && sift_v6_class(a->arg.typid) != SV6C_BOOL && a->arg.typid != NUMERICOID;
	}
	return only;
}

static bool
totals_of(const SiftV6Agg *a)
{
	if (a->fclause != 0 || a->nxcols != 0)
		return false;
	if (a->kind == SV6_COUNT_STAR)
		return true;
	return (a->kind == SV6_COUNT || a->kind == SV6_SUM || a->kind == SV6_AVG || a->kind == SV6_MIN || a->kind == SV6_MAX) && integer_arg(&a->arg);
}

static bool
totals_spec(const SiftV6Spec *spec)
{
	bool		ok = plain_spec(spec);

	for (int j = 0; ok && j < spec->naggs; j++)
		ok = totals_of(&spec->agg[j]);
	return ok;
}

bool
sift_v6_spec_whole_parts(const SiftV6Spec *spec)
{
	return sift_v6_spec_count_only(spec) || sift_v6_spec_minmax_only(spec) || totals_spec(spec);
}

/* ---- the totals of a column of a part, kept by the backend ---- */

typedef struct ColTotals
{
	uint64		nn;				/* the rows with a value */
	SiftI128	sum;
	int64		min;
	int64		max;
} ColTotals;

typedef struct StatEnt
{
	RelFileLocator rel;
	uint64		part_gen;
	uint64		nrows;
	uint64		used;			/* sift_cache_tick(); 0: the slot is empty */
	BlockNumber header_blk;
	int			leaf;
	ColTotals	tot;
} StatEnt;

#define STAT_SLOTS	1024		/* direct mapped: a part and a column have one slot, the one they replace is a part read again */

static StatEnt *stat_slots;

static uint64
stat_oldest(void)
{
	uint64		oldest = UINT64_MAX;

	for (int i = 0; stat_slots != NULL && i < STAT_SLOTS; i++)
		if (stat_slots[i].used != 0 && stat_slots[i].used < oldest)
			oldest = stat_slots[i].used;
	return oldest;
}

static bool
stat_evict_one(void)
{
	int			at = -1;

	for (int i = 0; stat_slots != NULL && i < STAT_SLOTS; i++)
		if (stat_slots[i].used != 0 && (at < 0 || stat_slots[i].used < stat_slots[at].used))
			at = i;
	if (at >= 0)
		stat_slots[at].used = 0;
	return at >= 0;
}

static void
stat_evict_all(void)
{
	for (int i = 0; stat_slots != NULL && i < STAT_SLOTS; i++)
		stat_slots[i].used = 0;
}

static StatEnt *
stat_slot(const RelFileLocator *rel, uint64 part_gen, int leaf)
{
	static const SiftCacheKind kind = {"part column totals", stat_oldest, stat_evict_one, stat_evict_all};
	const uint64 h = (part_gen * UINT64CONST(0x9E3779B97F4A7C15) ^ (uint64) leaf * UINT64CONST(0xC2B2AE3D27D4EB4F) ^ rel->relNumber) * UINT64CONST(0x9E3779B97F4A7C15);

	if (stat_slots == NULL)
	{
		if (!sift_cache_room(STAT_SLOTS * sizeof(StatEnt)))
			return NULL;
		stat_slots = MemoryContextAllocAligned(sift_cache_create_context("sift cache part column totals"), STAT_SLOTS * sizeof(StatEnt), 32, MCXT_ALLOC_ZERO);
		sift_cache_register(&kind);
	}
	return &stat_slots[h >> 54];
}

static bool
stat_is(const StatEnt *e, const RelFileLocator *rel, const SiftPartDesc *d, int leaf)
{
	return e->used != 0 && e->part_gen == d->part_gen && e->leaf == leaf && e->header_blk == d->header_blk && e->nrows == d->nrows && RelFileLocatorEquals(e->rel, *rel);
}

void
sift_v6_colstat_forget(const RelFileLocator *rel)
{
	for (int i = 0; stat_slots != NULL && i < STAT_SLOTS; i++)
		if (RelFileLocatorEquals(stat_slots[i].rel, *rel))
			stat_slots[i].used = 0;
}

/* ---- the scan's view of them ---- */

struct V6ColStat
{
	const SiftV6Spec *spec;
	SiftV6Group *g;
	RelFileLocator rel;
	int			nleaf;
	int		   *leaf;
	int		   *at;				/* of aggregate j: its place among leaf, -1 for COUNT(*) */
	SiftPartDesc part;			/* the part being read, when whole */
	bool		whole;
	uint64		rows;
	ColTotals  *tot;			/* of the part being read */
	ColTotals  *known;			/* of a part the cache answers */
};

V6ColStat *
sift_v6_colstat_begin(const SiftV6Spec *spec, SiftV6Group *g, Relation index, const int *first, MemoryContext cx)
{
	V6ColStat  *cs;

	if (!totals_spec(spec) || sift_v6_spec_count_only(spec))
		return NULL;
	cs = MemoryContextAllocZero(cx, sizeof(V6ColStat));
	cs->spec = spec;
	cs->g = g;
	cs->rel = index->rd_locator;
	cs->leaf = MemoryContextAlloc(cx, spec->naggs * sizeof(int));
	cs->at = MemoryContextAlloc(cx, spec->naggs * sizeof(int));
	cs->tot = MemoryContextAllocAligned(cx, spec->naggs * sizeof(ColTotals), 32, 0);
	cs->known = MemoryContextAllocAligned(cx, spec->naggs * sizeof(ColTotals), 32, 0);
	for (int j = 0; j < spec->naggs; j++)
	{
		const int	leaf = spec->agg[j].kind == SV6_COUNT_STAR ? -1 : first[spec->agg[j].arg.attno];
		int			k = 0;

		while (k < cs->nleaf && cs->leaf[k] != leaf)
			k++;
		if (leaf >= 0 && k == cs->nleaf)
			cs->leaf[cs->nleaf++] = leaf;
		cs->at[j] = leaf >= 0 ? k : -1;
	}
	return cs;
}

static void
fold_totals(const V6ColStat *cs, const SiftPartDesc *d, const ColTotals *tot)
{
	for (int j = 0; j < cs->spec->naggs; j++)
	{
		const SiftV6Agg *a = &cs->spec->agg[j];
		const ColTotals *t = cs->at[j] >= 0 ? &tot[cs->at[j]] : NULL;
		V6Acc		acc = {.n = t != NULL ? (int64) t->nn : (int64) d->nrows};

		if ((a->kind == SV6_SUM || a->kind == SV6_AVG) && t != NULL)
			v6acc_set_i128(&acc, t->sum);
		else if ((a->kind == SV6_MIN || a->kind == SV6_MAX) && t != NULL)
			acc.v0 = (uint64) (a->kind == SV6_MIN ? t->min : t->max);
		sift_v6_acc_combine(v6g_acc_op(a), &cs->g->set[0].plain[j], &acc);
	}
}

static void
keep_part(V6ColStat *cs)
{
	if (!cs->whole || cs->rows != cs->part.nrows)
		return;
	for (int k = 0; k < cs->nleaf; k++)
	{
		StatEnt    *e = stat_slot(&cs->rel, cs->part.part_gen, cs->leaf[k]);

		if (e == NULL)
			return;
		*e = (StatEnt) {.rel = cs->rel,.part_gen = cs->part.part_gen,.nrows = cs->part.nrows,.header_blk = cs->part.header_blk,.leaf = cs->leaf[k],.tot = cs->tot[k],.used = sift_cache_tick()};
	}
	sift_cache_commit();
}

bool
sift_v6_colstat_offer(V6ColStat *cs, const SiftPartDesc *d, bool whole)
{
	bool		known = whole;

	keep_part(cs);
	cs->whole = false;
	for (int k = 0; known && k < cs->nleaf; k++)
	{
		StatEnt    *e = stat_slot(&cs->rel, d->part_gen, cs->leaf[k]);

		known = e != NULL && stat_is(e, &cs->rel, d, cs->leaf[k]);
		if (known)
		{
			e->used = sift_cache_tick();
			cs->known[k] = e->tot;
		}
	}
	if (known)
	{
		fold_totals(cs, d, cs->known);
		return true;
	}
	cs->part = *d;
	cs->whole = whole;
	cs->rows = 0;
	for (int k = 0; k < cs->nleaf; k++)
		cs->tot[k] = (ColTotals) {.min = PG_INT64_MAX,.max = PG_INT64_MIN};
	return false;
}

/* The rows of the slice that are live and have a value, folded into t */
static void
slice_totals(ColTotals *t, const ColView *cv, const uint64 *live, int off, int n)
{
	const int64 *x = cv->i + off;
	const bool *isnull = cv->isnull + off;
	int64		hi = 0,
				lo = 0,
				mn = t->min,
				mx = t->max;
	uint64		nn = 0;

	for (int i = 0; i < n; i++)
	{
		const int64 take = (int64) ((live[(off + i) >> 6] >> ((off + i) & 63)) & 1) & !isnull[i];
		const int64 v = x[i];

		nn += take;
		hi += take * (v >> 32);
		lo += take * (v & 0xFFFFFFFF);
		mn = take && v < mn ? v : mn;
		mx = take && v > mx ? v : mx;
	}
	t->nn += nn;
	sift_i128_add(&t->sum, (SiftI128) {.lo = (uint64) hi << 32,.hi = hi >> 32});
	sift_i128_add(&t->sum, (SiftI128) {.lo = (uint64) lo,.hi = 0});
	t->min = mn;
	t->max = mx;
}

void
sift_v6_colstat_note(V6ColStat *cs, const SiftF2Batch *b)
{
	if (!cs->whole || b->part == NULL || b->part->part_gen != cs->part.part_gen)
		return;
	for (int w = 0; w < (b->n + 63) / 64; w++)
		cs->rows += pg_popcount64(b->live[w]);
	for (int k = 0; k < cs->nleaf; k++)
		for (int off = 0; off < b->n; off += SIFT_VEC_LEN)
			slice_totals(&cs->tot[k], &b->views[cs->leaf[k]], b->live, off, Min(SIFT_VEC_LEN, b->n - off));
}

void
sift_v6_colstat_end(V6ColStat *cs)
{
	keep_part(cs);
	cs->whole = false;
}
