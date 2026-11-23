/*
 * col/f2/f2_v6_topn.c - the units of the Top-N tournament (agg/topn_v6.h) over an index of format major 2. The slab of a partition is queued first (its rows tighten theta before any part is opened); a
 * partition is queued with the bounds of its time column when the leading key is that column; a partition is opened into its parts, a part (opened once, with its deletion vector) into the one
 * frontier granule of a part that reads in the order of the query or the reverse of it, and into all its granules, each with the zones of its keys, otherwise. A granule is read by its leaves
 * of the keys first: the rows that cannot beat theta are dropped before the quals and the heap are looked at; the rows left are judged as the source of batches judges them (f2_v6_bridge.c).
 * The parts the device read (SiftV6TopNDevice) are not opened: its best rows are offered first, with the addresses read from their granules, and tighten theta before the slab is scanned.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/gpu_v6.h"
#include "col/am.h"
#include "core/memtrack.h"
#include "col/f2/f2_store_scan.h"
#include "col/f2/f2_v6_bridge.h"
#include "col/f2/part_format.h"

typedef struct TopPart
{
	F2V6Part   *part;
	SiftZone   *keyz[SIFT_TOURN_MAX_KEYS];
	SiftZone   *boundz[SIFT_V6_MAX_BOUNDS];
	SiftF2OrderMatch match;
} TopPart;

typedef struct TopPartition
{
	bool		init;
	const SiftPendDelIndex *px;
	TopPart    *parts;
} TopPartition;

typedef struct TopSrc
{
	MemoryContext cx;
	MemoryContext gcx;			/* one granule */
	F2V6Ctx    *c;
	F2Eval	   *ev;
	SiftV6TopNSpec spec;
	int			key_leaf[SIFT_TOURN_MAX_KEYS];
	int			time_slot[SIFT_TOURN_MAX_KEYS];	/* the time column (0, 1) a key is, -1: none */
	SiftV6TopNDevice dev;		/* what the device took; no rows and no parts when it took nothing */
	bool		lead_key;		/* the leading key of the query is the leading key column of the index, an integer: the keys of a partition or a part bound it */
	unsigned	lead_flags;
	TopPartition *partitions;
	int			nleaves;
	int			leaves[SIFT_V6_MAX_COLS + SIFT_V6_MAX_MASKS + 2 * SIFT_KEY_ORDER_MAX + SIFT_TOURN_MAX_KEYS];
	uint64		cand[SIFT_GRANULE_ROWS / 64 + 1];
	uint64		pass[SIFT_GRANULE_ROWS / 64 + 1];
} TopSrc;

static const SiftTournBound unknown_bound;

static SiftTournBound
zone_bound(const SiftZone *z, uint32 nrows)
{
	if (z == NULL || (z->flags & SIFT_ZONE_NONE))
		return unknown_bound;
	return sift_f2_range_bound(z->min, z->max, z->nnull > 0, z->nnull >= nrows);
}

static SiftTournBound
time_bound(const TopSrc *s, int key, const int64 *tmin, const int64 *tmax)
{
	return s->time_slot[key] < 0 ? unknown_bound : sift_f2_range_bound(tmin[s->time_slot[key]], tmax[s->time_slot[key]], true, false);
}

static bool
part_time_known(const SiftPartDesc *d, int t)
{
	return (d->flags & (t == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1)) && d->col_delta_blk == InvalidBlockNumber;
}

/* The leading key inside the keys [lo, hi] of a unit; a column delta may move a value, so a unit that has one is unknown */
static SiftTournBound
lead_bound(const TopSrc *s, int key, const uint8 *lo, const uint8 *hi)
{
	int64		min,
				max;

	if (key != 0 || !s->lead_key || !sift_f2_v6_lead_range(lo, hi, s->lead_flags, &min, &max))
		return unknown_bound;
	return sift_f2_range_bound(min, max, false, false);
}

static bool
list_stable(const SiftPlanPartList *list)
{
	for (int j = 0; j < list->nparts; j++)
		if (list->parts[j].col_delta_blk != InvalidBlockNumber)
			return false;
	return true;
}

static TopPartition *
partition_of(TopSrc *s, uint32 pi)
{
	TopPartition *tp = &s->partitions[pi];

	if (!tp->init)
	{
		MemoryContext old = MemoryContextSwitchTo(s->cx);

		tp->px = f2v6_pending(s->c, &s->c->plan->partitions[pi]);
		tp->parts = palloc0(Max(s->c->plan->partlists[pi].nparts, 1) * sizeof(TopPart));
		tp->init = true;
		MemoryContextSwitchTo(old);
	}
	return tp;
}

static TopPart *
part_of(TopSrc *s, uint32 pi, uint32 pj)
{
	TopPartition *tp = partition_of(s, pi);
	TopPart    *p = &tp->parts[pj];

	if (p->part == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->cx);

		p->part = f2v6_part_open(s->c, &s->c->plan->partitions[pi], &s->c->plan->partlists[pi].parts[pj], tp->px);
		for (int k = 0; k < s->spec.nkeys; k++)
			p->keyz[k] = f2v6_zones(s->c, p->part, s->key_leaf[k]);
		for (int b = 0; b < sift_f2_v6_eval_nbounds(s->ev); b++)
			p->boundz[b] = f2v6_zones(s->c, p->part, sift_f2_v6_eval_bound_leaf(s->ev, b));
		p->match = s->c->f->nkey > 0 && s->c->f->key_cols[0] == s->key_leaf[0] ?
			sift_f2_order_match(s->c->f->spec.sort_flags[0] & SIFT_KEY_DESC, s->c->f->spec.sort_flags[0] & SIFT_KEY_NULLS_FIRST,
								s->spec.order[0].desc, s->spec.order[0].nulls_first) : SF2_ORDER_NONE;
		MemoryContextSwitchTo(old);
	}
	return p;
}

static void
push_granule(TopSrc *s, SiftTournament *t, SiftTournKind kind, uint32 pi, uint32 pj, uint32 g)
{
	const TopPart *p = &s->partitions[pi].parts[pj];
	const uint32 nrows = p->part->dir[g].nrows;
	SiftTournBound k0 = zone_bound(p->keyz[0] != NULL ? &p->keyz[0][g] : NULL, nrows),
				k1 = s->spec.nkeys > 1 ? zone_bound(p->keyz[1] != NULL ? &p->keyz[1][g] : NULL, nrows) : unknown_bound;

	if (sift_f2_v6_ruled_out(s->ev, (SiftZone *const *) p->boundz, g, nrows))
	{
		sift_f2_v6_eval_skips(s->ev)->granules++;
		return;
	}
	sift_tourn_push(t, kind, pi, g, pj, &k0, s->spec.nkeys > 1 ? &k1 : NULL);
}

typedef struct DevPart
{
	uint64		gen;
	uint32		slot;			/* of the partition */
	uint32		idx;			/* of the part in it */
} DevPart;

static int
dev_part_cmp(const void *a, const void *b)
{
	const uint64 x = ((const DevPart *) a)->gen,
				y = ((const DevPart *) b)->gen;

	return x < y ? -1 : x > y;
}

/* The parts of the plan the device read, by generation */
static DevPart *
device_parts(const TopSrc *s, int *n)
{
	const SiftPlanSnapshot *plan = s->c->plan;
	DevPart    *parts = palloc(Max(s->dev.nskip, 1) * sizeof(DevPart));

	*n = 0;
	for (int i = 0; i < plan->npartitions; i++)
		for (int j = 0; j < plan->partlists[i].nparts; j++)
		{
			const uint64 gen = plan->partlists[i].parts[j].part_gen;

			if (sift_store_gen_skipped(s->dev.skip, s->dev.nskip, gen))
				parts[(*n)++] = (DevPart) {gen, (uint32) i, (uint32) j};
		}
	qsort(parts, *n, sizeof(DevPart), dev_part_cmp);
	return parts;
}

/* the rows of the device by the granule they sit in, so that each granule gives its addresses once */
static int
dev_row_cmp(const void *a, const void *b, void *arg)
{
	const SiftGpuCand *c = arg,
			   *x = &c[*(const uint32 *) a],
			   *y = &c[*(const uint32 *) b];

	if (x->part_gen != y->part_gen)
		return x->part_gen < y->part_gen ? -1 : 1;
	return x->granule != y->granule ? (x->granule < y->granule ? -1 : 1) : 0;
}

/* The rows the device chose are rows of the tournament: its key, and the address of the row read from the granule it names */
static void
offer_device(TopSrc *s, SiftTournament *t)
{
	const SiftGpuCand *cand = s->dev.cand;
	DevPart    *parts;
	uint32	   *order;
	int			nparts;
	const SiftGpuCand *at = NULL;
	F2V6Gran	g;
	const DevPart *part = NULL;

	if (s->dev.ncand == 0)
		return;
	parts = device_parts(s, &nparts);
	order = palloc(s->dev.ncand * sizeof(uint32));
	for (uint32 i = 0; i < s->dev.ncand; i++)
		order[i] = i;
	qsort_arg(order, s->dev.ncand, sizeof(uint32), dev_row_cmp, (void *) cand);
	for (uint32 i = 0; i < s->dev.ncand; i++)
	{
		const SiftGpuCand *c = &cand[order[i]];
		SiftTopNWinRow row = {.key[0] = c->isnull ? 0 : c->key,.nulls = c->isnull,.local_g = (uint16) c->granule,.row_ord = (uint16) c->row};

		if (at == NULL || at->part_gen != c->part_gen || at->granule != c->granule)
		{
			const DevPart key = {.gen = c->part_gen};
			MemoryContext old;

			part = bsearch(&key, parts, nparts, sizeof(DevPart), dev_part_cmp);
			SIFT_INVARIANT(part != NULL, "the device read part generation %llu of index %u, which the plan of the scan lacks", (unsigned long long) c->part_gen, s->spec.indexoid);
			MemoryContextReset(s->gcx);
			old = MemoryContextSwitchTo(s->gcx);
			f2v6_granule(s->c, part_of(s, part->slot, part->idx)->part, c->granule, NULL, 0, &g);
			MemoryContextSwitchTo(old);
			at = c;
		}
		SIFT_INVARIANT(c->row < g.n && ((g.live[c->row >> 6] >> (c->row & 63)) & 1), "row %u of granule %u of part generation %llu of index %u is not one the scan sees",
					   c->row, c->granule, (unsigned long long) c->part_gen, s->spec.indexoid);
		row.tid = g.tid[c->row];
		row.part_slot = part->slot;
		sift_tourn_offer(t, &row);
	}
	pfree(order);
	pfree(parts);
}

static void
top_device(void *src, const SiftV6TopNDevice *dev)
{
	((TopSrc *) src)->dev = *dev;
}

static void
top_seed(void *src, SiftTournament *t)
{
	TopSrc	   *s = src;
	const SiftPlanSnapshot *plan = s->c->plan;
	SiftV6SkipStats *skips = sift_f2_v6_eval_skips(s->ev);

	offer_device(s, t);
	for (int i = 0; i < plan->npartitions; i++)
		if (plan->partitions[i].l0_slab_blk != InvalidBlockNumber && sift_f2_v6_eval_entry_meets_keys(s->ev, &plan->partitions[i]))
			sift_tourn_push(t, STOK_L0_SLAB, (uint32) i, 0, 0, NULL, NULL);
	for (int i = 0; i < plan->npartitions; i++)
	{
		const SiftPartitionEntry *e = &plan->partitions[i];
		const bool	stable = list_stable(&plan->partlists[i]);
		const bool	dirty = f2v6_time_dirty(s->c, e);
		SiftTournBound k0 = dirty ? unknown_bound : time_bound(s, 0, e->time_min, e->time_max),
					k1 = s->spec.nkeys > 1 && !dirty ? time_bound(s, 1, e->time_min, e->time_max) : unknown_bound;

		if (!sift_f2_v6_eval_entry_meets_keys(s->ev, e) || !f2v6_entry_meets_time(s->c, e, sift_f2_v6_eval_window(s->ev)))
		{
			skips->partitions++;
			continue;
		}
		for (int j = 0; j < plan->partlists[i].nparts; j++)
			if (!part_time_known(&plan->partlists[i].parts[j], 0) || !part_time_known(&plan->partlists[i].parts[j], 1))
				k0 = k1 = unknown_bound;
		if (stable && !k0.known)
			k0 = lead_bound(s, 0, e->key_lo, e->key_hi);
		sift_tourn_push(t, STOK_PARTITION, (uint32) i, 0, 0, &k0, s->spec.nkeys > 1 ? &k1 : NULL);
	}
}

static void
top_expand(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	TopSrc	   *s = src;
	const SiftPlanPartList *list = &s->c->plan->partlists[tok->part_slot];
	const SiftPartitionEntry *e = &s->c->plan->partitions[tok->part_slot];

	if (tok->kind == STOK_PARTITION)
	{
		for (int j = 0; j < list->nparts; j++)
		{
			const SiftPartDesc *d = &list->parts[j];
			const bool	known = part_time_known(d, 0) && part_time_known(d, 1) && !f2v6_time_dirty(s->c, e);
			SiftTournBound k0 = known ? time_bound(s, 0, d->time_min, d->time_max) : unknown_bound,
						k1 = known && s->spec.nkeys > 1 ? time_bound(s, 1, d->time_min, d->time_max) : unknown_bound;

			if (sift_store_gen_skipped(s->dev.skip, s->dev.nskip, d->part_gen) || !f2v6_part_meets_time(s->c, e, d, sift_f2_v6_eval_window(s->ev)) ||
				!sift_f2_v6_eval_part_meets_keys(s->ev, d))
			{
				sift_f2_v6_eval_skips(s->ev)->parts++;
				continue;
			}
			if (!k0.known && d->col_delta_blk == InvalidBlockNumber)
				k0 = lead_bound(s, 0, d->key_lo, d->key_hi);
			sift_tourn_push(t, STOK_PART, tok->part_slot, 0, (uint32) j, &k0, s->spec.nkeys > 1 ? &k1 : NULL);
		}
		return;
	}
	{
		const TopPart *p = part_of(s, tok->part_slot, tok->part_idx);

		if (p->match != SF2_ORDER_NONE)
			push_granule(s, t, STOK_KEY_FRONTIER, tok->part_slot, tok->part_idx, sift_f2_frontier_first(p->match, p->part->ngranules));
		else
			for (uint32 g = 0; g < p->part->ngranules; g++)
				push_granule(s, t, STOK_GRANULE, tok->part_slot, tok->part_idx, g);
	}
}

static void
top_frontier_next(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	TopSrc	   *s = src;
	const TopPart *p = &s->partitions[tok->part_slot].parts[tok->part_idx];
	uint32		next;

	if (sift_f2_frontier_next(p->match, tok->local_g, p->part->ngranules, &next))
		push_granule(s, t, STOK_KEY_FRONTIER, tok->part_slot, tok->part_idx, next);
}

/* The key of row r as the tournament orders it */
static int64
key_of(const ColView *v, int r)
{
	return v->kind == SIFT_CELL_F8 ? sift_f64_order_i64(v->f[r]) : v->i[r];
}

/* A row whose leading key is worse than theta cannot be among the winners */
static void
drop_worse_than_theta(TopSrc *s, SiftTournament *t, const F2V6Gran *g)
{
	const ColView *v = &g->views[s->key_leaf[0]];
	const SiftTournKey *o = &s->spec.order[0];
	int64		th;
	bool		th_null;

	if (!sift_tourn_theta(t, &th, &th_null) || th_null)
		return;
	for (int r = 0; r < g->n; r++)
		if (!v->isnull[r] && (o->desc ? key_of(v, r) < th : key_of(v, r) > th))
			s->cand[r >> 6] &= ~(UINT64CONST(1) << (r & 63));
}

static void
offer(TopSrc *s, SiftTournament *t, const F2V6Gran *g, uint32 slot, uint32 granule)
{
	for (int w = 0; w < (g->n + 63) / 64; w++)
		for (uint64 bits = s->pass[w]; bits != 0; bits &= bits - 1)
		{
			const int	r = w * 64 + __builtin_ctzll(bits);
			SiftTopNWinRow row = {.tid = g->tid[r],.part_slot = slot,.local_g = (uint16) granule,.row_ord = (uint16) r};

			for (int k = 0; k < s->spec.nkeys; k++)
			{
				const ColView *v = &g->views[s->key_leaf[k]];

				row.key[k] = v->isnull[r] ? 0 : key_of(v, r);
				row.nulls |= (uint8) (v->isnull[r] << k);
			}
			sift_tourn_offer(t, &row);
		}
}

static void
top_scan(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	TopSrc	   *s = src;
	MemoryContext old;
	F2V6Gran	g;

	MemoryContextReset(s->gcx);
	old = MemoryContextSwitchTo(s->gcx);
	if (tok->kind == STOK_L0_SLAB)
		f2v6_slab(s->c, &s->c->plan->partitions[tok->part_slot], partition_of(s, tok->part_slot)->px, false, s->leaves, s->nleaves, &g);
	else
		f2v6_granule(s->c, part_of(s, tok->part_slot, tok->part_idx)->part, tok->local_g, s->leaves, s->nleaves, &g);
	memcpy(s->cand, g.live, (g.n + 63) / 64 * sizeof(uint64));
	drop_worse_than_theta(s, t, &g);
	if (sift_f2_v6_eval_rows(s->ev, &g, s->cand, s->pass) > 0)
		offer(s, t, &g, tok->part_slot, tok->kind == STOK_L0_SLAB ? 0 : tok->local_g);
	MemoryContextSwitchTo(old);
}

static bool
top_fetch(void *src, const SiftTopNWinRow *w, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	return f2v6_fetch(((TopSrc *) src)->c, w->tid, attnos, n, values, isnull);
}

static void
top_close(void *src)
{
	TopSrc	   *s = src;

	f2v6_end(s->c);
	MemoryContextDelete(s->cx);
}

bool
sift_f2_v6_topn_open(EState *estate, const SiftV6TopNSpec *spec, const SiftV6Input *in, const SiftV6Vexpr *quals, SiftV6TopNSource *dst)
{
	MemoryContext cx = sift_mem_allocset_create(estate->es_query_cxt, "sift f2 v6 topn");
	MemoryContext old = MemoryContextSwitchTo(cx);
	TopSrc	   *s = palloc0(sizeof(TopSrc));
	const int  *evl;

	s->cx = cx;
	s->gcx = sift_mem_allocset_create(cx, "sift f2 v6 topn granule");
	s->spec = *spec;
	s->c = f2v6_begin(cx, estate->es_snapshot, spec->indexoid);
	s->ev = sift_f2_v6_eval_new(cx, s->c, in, quals, false);
	s->partitions = palloc0(Max(s->c->plan->npartitions, 1) * sizeof(TopPartition));
	evl = sift_f2_v6_eval_leaves(s->ev, &s->nleaves);
	memcpy(s->leaves, evl, s->nleaves * sizeof(int));
	for (int k = 0; k < spec->nkeys; k++)
	{
		const SiftColDef *d = &s->c->schema->cols[s->c->rel->first[spec->key[k].attno]];

		if (d->lanes > 1 || (d->store != SIFT_COLS_INT && d->store != SIFT_COLS_F4 && d->store != SIFT_COLS_F8))
		{
			f2v6_end(s->c);
			MemoryContextSwitchTo(old);
			MemoryContextDelete(cx);
			return false;
		}
		s->key_leaf[k] = d->id;
		s->leaves[s->nleaves++] = s->key_leaf[k];
		s->time_slot[k] = s->c->f->spec.time_cols[0] == s->key_leaf[k] + 1 ? 0 : s->c->f->spec.time_cols[1] == s->key_leaf[k] + 1 ? 1 : -1;
	}
	s->lead_key = s->c->f->nkey > 0 && s->key_leaf[0] == s->c->f->key_cols[0] && s->c->schema->cols[s->key_leaf[0]].store == SIFT_COLS_INT;
	s->lead_flags = s->c->f->spec.sort_flags[0];
	MemoryContextSwitchTo(old);
	*dst = (SiftV6TopNSource) {.ops = {.expand = top_expand,.frontier_next = top_frontier_next,.scan = top_scan},.src = s,.device = top_device,.seed = top_seed,.fetch = top_fetch,.close = top_close,.skips = sift_f2_v6_eval_skips(s->ev)};
	return true;
}
