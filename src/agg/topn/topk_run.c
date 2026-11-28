/*
 * agg/topn/topk_run.c - the groups of SiftAggV6 into the heap of the best K (agg/topn/topk_run.h).
 */
#include "postgres.h"

#include "catalog/pg_operator_d.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/group/int.h"
#include "agg/group/compact.h"
#include "agg/group/topk.h"
#include "agg/topn/topk_run.h"
#include "core/memtrack.h"

#define INT8_GT_OPERATOR 413
#define TOPK_RUN		32

struct SiftV6TopKRun
{
	const SiftV6Spec *spec;
	TupleDesc	desc;
	MemoryContext tmp;
	SiftGTopK   *heap;
	int			count_agg;		/* the first sort column is a COUNT: its aggregate's number, else -1 */
	bool		count_desc;
	Datum	   *values;
	bool	   *isnull;
	uint32		tmp_uses;
	uint64		refused;		/* the candidates the heap did not take */
	uint64		put;			/* the candidates it took, some of which a better one pushed out */
};

SiftV6TopKRun *
sift_v6_topk_run_new(MemoryContext cx, const SiftV6Spec *spec, TupleDesc desc)
{
	SiftV6TopKRun *r = MemoryContextAllocZero(cx, sizeof(SiftV6TopKRun));
	const int	n = desc->natts;

	r->spec = spec;
	r->desc = desc;
	/* MEM[MEM-AGG-25]: agg transient contexts of this file (sift_mem_allocset_create, 256KB max blocks; was the default allocator with up to 8MB blocks) */
	r->tmp = sift_mem_allocset_create(cx, "sift agg v6 top k");
	r->count_agg = -1;
	if (spec->ntopk > 0 && spec->topk_item[0].pos >= spec->nkeys && (spec->topk_item[0].sortop == Int8LessOperator || spec->topk_item[0].sortop == INT8_GT_OPERATOR) &&
		(spec->agg[spec->topk_item[0].pos - spec->nkeys].kind == SV6_COUNT_STAR || spec->agg[spec->topk_item[0].pos - spec->nkeys].kind == SV6_COUNT))
	{
		r->count_agg = spec->topk_item[0].pos - spec->nkeys;
		r->count_desc = spec->topk_item[0].sortop == INT8_GT_OPERATOR;
	}
	r->heap = sift_gtopk_new(cx, spec->topk, desc, spec->ntopk, spec->topk_item, spec->topk_ties);
	r->values = MemoryContextAlloc(cx, Max(n, 1) * sizeof(Datum));
	r->isnull = MemoryContextAlloc(cx, Max(n, 1) * sizeof(bool));
	return r;
}

bool
sift_v6_topk_run_worst(const SiftV6TopKRun *r, int item, Datum *value, bool *isnull)
{
	return sift_gtopk_worst(r->heap, item, value, isnull);
}

/* a candidate whose sort values are made on demand from a group's slot */
typedef struct Cand
{
	const SiftV6Group *g;
	const V6Set *set;
	const SiftAggSlot64 *slot;
	const uint64 *cslot;		/* the slot of the group in the compact table, when it is there (slot is then NULL) */
	const V6Acc *acc;
	SiftV6TopKRun *r;
	Datum	   *values;
	bool	   *isnull;
	uint32		keys_done;		/* the key columns already in values: the whole tuple of a candidate that stays makes only the others */
	uint64		aggs_done;		/* the same for the aggregates (the first 64) */
} Cand;

static void
cand_keys(const Cand *c, uint32 want)
{
	if (c->cslot != NULL)
		v6g_compact_key_datums(c->g, c->set, c->cslot, c->values, c->isnull, want);
	else
		v6g_key_datums(c->g, c->set, &c->set->ht, c->slot, c->values, c->isnull, want);
}

static Datum
cand_lazy(void *ctx, int item, bool *isnull)
{
	Cand	   *c = ctx;
	const SiftV6Spec *spec = c->g->spec;
	const int	pos = spec->topk_item[item].pos;
	const int	j = pos - spec->nkeys;

	if (pos < spec->nkeys)
	{
		if (!((c->keys_done >> pos) & 1))
			cand_keys(c, UINT32_C(1) << pos);
		c->keys_done |= UINT32_C(1) << pos;
		*isnull = c->isnull[pos];
		return c->values[pos];
	}
	if (j >= 64)
		return v6g_agg_datum(c->g, &spec->agg[j], &c->acc[j], c->set->mask, isnull);
	if (!((c->aggs_done >> j) & 1))
		c->values[pos] = v6g_agg_datum(c->g, &spec->agg[j], &c->acc[j], c->set->mask, &c->isnull[pos]);
	c->aggs_done |= UINT64CONST(1) << j;
	*isnull = c->isnull[pos];
	return c->values[pos];
}

/* The columns of the tuple the sort columns did not already make */
static void
cand_whole_tuple(const Cand *c)
{
	const SiftV6Spec *spec = c->g->spec;

	cand_keys(c, ~c->keys_done);
	for (int j = 0; j < spec->naggs; j++)
		if (j >= 64 || !((c->aggs_done >> j) & 1))
			c->values[spec->nkeys + j] = v6g_agg_datum(c->g, &spec->agg[j], &c->acc[j], c->set->mask, &c->isnull[spec->nkeys + j]);
}

/* A group is done with: its temporary Datums go with the context every 64 groups, or when it holds 64 kB */
static inline void
tmp_release(SiftV6TopKRun *r)
{
	if (++r->tmp_uses >= 64 || MemoryContextMemAllocated(r->tmp, false) > 65536)
	{
		MemoryContextReset(r->tmp);
		r->tmp_uses = 0;
	}
}

static Datum
cand_whole(void *ctx, int item, bool *isnull)
{
	const Cand *c = ctx;
	const int	pos = c->r->spec->topk_item[item].pos;

	*isnull = c->isnull[pos];
	return c->values[pos];
}

static bool
having_passes(SiftV6TopKRun *r, ExprState *qual, ExprContext *ec, TupleTableSlot *slot, Datum *values, bool *isnull)
{
	bool		ok;

	if (qual == NULL)
		return true;
	ExecClearTuple(slot);
	memcpy(slot->tts_values, values, r->desc->natts * sizeof(Datum));
	memcpy(slot->tts_isnull, isnull, r->desc->natts * sizeof(bool));
	ExecStoreVirtualTuple(slot);
	ec->ecxt_scantuple = slot;
	ok = ExecQual(qual, ec);
	ResetExprContext(ec);
	return ok;
}

/* a tuple whole: offered whether or not it stays */
static void
heap_put(SiftV6TopKRun *r, const Datum *values, const bool *isnull)
{
	sift_gtopk_put(r->heap, values, isnull);
	r->put++;
}

static void
offer_whole(SiftV6TopKRun *r, Datum *values, bool *isnull, ExprState *qual, ExprContext *ec, TupleTableSlot *slot)
{
	Cand		c = {.r = r,.values = values,.isnull = isnull};

	if (!having_passes(r, qual, ec, slot, values, isnull))
		return;
	if (sift_gtopk_wants(r->heap, cand_whole, &c))
		heap_put(r, values, isnull);
	else
		r->refused++;
}

void
sift_v6_topk_run_stream(SiftV6TopKRun *r, bool (*next) (void *arg, Datum *values, bool *isnull), void *arg, ExprState *qual, ExprContext *ec, TupleTableSlot *slot)
{
	MemoryContext old = MemoryContextSwitchTo(r->tmp);

	while (next(arg, r->values, r->isnull))
	{
		offer_whole(r, r->values, r->isnull, qual, ec, slot);
		tmp_release(r);
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(r->tmp);
	sift_gtopk_finish(r->heap);
}

/* The count a group must beat to enter a heap that is full and sorted by that count alone: a group that only ties it never enters */
typedef struct CountBar
{
	bool		set;
	int64		worst;
} CountBar;

static void
count_bar(const SiftV6TopKRun *r, CountBar *bar)
{
	Datum		worst;
	bool		worst_null;

	bar->set = r->count_agg >= 0 && sift_gtopk_worst(r->heap, 0, &worst, &worst_null) && !worst_null;
	if (bar->set)
		bar->worst = DatumGetInt64(worst);
}

static inline bool
count_loses_n(const SiftV6TopKRun *r, const CountBar *bar, int64 n)
{
	const bool	only = r->spec->ntopk == 1 && !r->spec->topk_ties;

	if (r->count_desc)
		return only ? n <= bar->worst : n < bar->worst;
	return only ? n >= bar->worst : n > bar->worst;
}

static inline bool
count_loses(const SiftV6TopKRun *r, const CountBar *bar, const V6Acc *acc)
{
	return bar->set && count_loses_n(r, bar, (int64) acc[r->count_agg].n);
}

static void
offer_group(SiftV6TopKRun *r, SiftV6Group *g, const V6Set *set, const SiftAggSlot64 *s, const uint64 *cslot, const V6Acc *acc, CountBar *bar, ExprState *qual, ExprContext *ec, TupleTableSlot *slot)
{
	Cand		c = {.g = g,.set = set,.slot = s,.cslot = cslot,.acc = acc,.r = r,.values = r->values,.isnull = r->isnull};

	if (count_loses(r, bar, acc))
	{
		r->refused++;
		return;
	}
	memset(r->isnull, true, g->spec->nkeys * sizeof(bool));
	if (qual != NULL)
	{
		cand_whole_tuple(&c);
		offer_whole(r, r->values, r->isnull, qual, ec, slot);
		count_bar(r, bar);
	}
	else if (sift_gtopk_wants(r->heap, cand_lazy, &c))
	{
		cand_whole_tuple(&c);
		heap_put(r, r->values, r->isnull);
		count_bar(r, bar);
	}
	else
		r->refused++;
	tmp_release(r);
}

/* The groups of the compact table of the set, each offered with the state it would have in the group table */
typedef struct CompactOffer
{
	SiftV6TopKRun *r;
	SiftV6Group *g;
	const V6Set *set;
	CountBar	bar;
	ExprState  *qual;
	ExprContext *ec;
	TupleTableSlot *slot;
	V6Acc	   *acc;
} CompactOffer;

static void
offer_compact(void *arg, const uint64 *s)
{
	CompactOffer *o = arg;

	CHECK_FOR_INTERRUPTS();
	if (o->bar.set && count_loses_n(o->r, &o->bar, (int64) v6g_compact_count(o->set, s)))
	{
		o->r->refused++;
		return;
	}
	v6g_compact_acc(o->g, o->set, s, o->acc);
	offer_group(o->r, o->g, o->set, NULL, s, o->acc, &o->bar, o->qual, o->ec, o->slot);
}

/* The state blocks of a run of groups are fetched together before the first is read: the slots stream in table order, their blocks are where the groups were made; the heap is not finished, so the groups of another state can follow */
void
sift_v6_topk_run_offer_groups(SiftV6TopKRun *r, SiftV6Group *g, ExprState *qual, ExprContext *ec, TupleTableSlot *slot)
{
	const V6Set *set = &g->set[0];
	const SiftSaltedHT *ht = &set->ht;
	MemoryContext old = MemoryContextSwitchTo(r->tmp);
	CountBar	bar;

	count_bar(r, &bar);
	if (g->spilled || g->frozen || set->nk == 0)
	{
		while (sift_v6_group_next(g, r->values, r->isnull))
		{
			CHECK_FOR_INTERRUPTS();
			offer_whole(r, r->values, r->isnull, qual, ec, slot);
			tmp_release(r);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(r->tmp);
		return;
	}
	if (set->cmp != NULL)
	{
		CompactOffer o = {.r = r,.g = g,.set = set,.qual = qual,.ec = ec,.slot = slot,.acc = MemoryContextAlloc(g->cx, Max(g->spec->naggs, 1) * sizeof(V6Acc)),.bar = bar};

		v6g_compact_each(set, offer_compact, &o);
		pfree(o.acc);
		MemoryContextReset(r->tmp);
		MemoryContextSwitchTo(old);
		return;
	}
	for (uint32 at = sift_ht_next_occupied(ht, 0); at < ht->capacity;)
	{
		const SiftAggSlot64 *run[TOPK_RUN];
		int			n = 0;

		CHECK_FOR_INTERRUPTS();
		for (; n < TOPK_RUN && at < ht->capacity; at = sift_ht_next_occupied(ht, at + 1))
		{
			run[n] = &ht->slots[at];
			__builtin_prefetch(sift_ht_ext(ht, run[n++]));
		}
		for (int i = 0; i < n; i++)
			offer_group(r, g, set, run[i], NULL, (const V6Acc *) sift_ht_ext(ht, run[i]), &bar, qual, ec, slot);
	}
	MemoryContextReset(r->tmp);
	MemoryContextSwitchTo(old);
}

void
sift_v6_topk_run_groups(SiftV6TopKRun *r, SiftV6Group *g, ExprState *qual, ExprContext *ec, TupleTableSlot *slot)
{
	sift_v6_topk_run_offer_groups(r, g, qual, ec, slot);
	sift_gtopk_finish(r->heap);
}

bool
sift_v6_topk_run_next(SiftV6TopKRun *r, Datum *values, bool *isnull)
{
	const Datum *v;
	const bool *n;

	if (!sift_gtopk_next(r->heap, &v, &n))
		return false;
	memcpy(values, v, r->desc->natts * sizeof(Datum));
	memcpy(isnull, n, r->desc->natts * sizeof(bool));
	return true;
}

bool
sift_v6_topk_run_wants(SiftV6TopKRun *r, SiftTopKValue value, void *ctx)
{
	const bool	wants = sift_gtopk_wants(r->heap, value, ctx);

	r->refused += !wants;
	return wants;
}

void
sift_v6_topk_run_put(SiftV6TopKRun *r, const Datum *values, const bool *isnull)
{
	heap_put(r, values, isnull);
}

uint64
sift_v6_topk_run_dropped(const SiftV6TopKRun *r)
{
	return r->refused + r->put - sift_gtopk_count(r->heap);
}

void
sift_v6_topk_run_finish(SiftV6TopKRun *r)
{
	sift_gtopk_finish(r->heap);
}

void
sift_v6_topk_run_end(SiftV6TopKRun *r)
{
	sift_gtopk_end(r->heap);
}
