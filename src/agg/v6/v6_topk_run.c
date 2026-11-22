/*
 * agg/v6/v6_topk_run.c - the groups of SiftAggV6 into the heap of the best K (agg/v6/v6_topk_run.h).
 */
#include "postgres.h"

#include "catalog/pg_operator_d.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/sortsupport.h"

#include "agg/v6/group_int.h"
#include "agg/v6/v6_group_topk.h"
#include "agg/v6/v6_topk_run.h"

#define INT8_GT_OPERATOR 413

struct SiftV6TopKRun
{
	const SiftV6Spec *spec;
	TupleDesc	desc;
	MemoryContext tmp;
	SortSupportData ssup[SIFT_V6_MAX_TOPK_ITEMS];
	SiftGTopK   *heap;
	uint32		at;
	int			count_agg;		/* the first sort column is a COUNT: its aggregate's number, else -1 */
	bool		count_desc;
	Datum	   *values;
	bool	   *isnull;
	uint32		tmp_uses;
};

static int
item_cmp(int item, Datum a, bool a_null, Datum b, bool b_null, void *arg)
{
	SiftV6TopKRun *r = arg;

	return ApplySortComparator(a, a_null, b, b_null, &r->ssup[item]);
}

SiftV6TopKRun *
sift_v6_topk_run_new(MemoryContext cx, const SiftV6Spec *spec, TupleDesc desc)
{
	SiftV6TopKRun *r = MemoryContextAllocZero(cx, sizeof(SiftV6TopKRun));
	const int	n = desc->natts;
	int			pos[SIFT_V6_MAX_TOPK_ITEMS];
	int16	   *len = palloc(Max(n, 1) * sizeof(int16));
	bool	   *byval = palloc(Max(n, 1) * sizeof(bool));

	r->spec = spec;
	r->desc = desc;
	r->tmp = sift_mem_allocset_create(cx, "sift agg v6 top k");
	for (int i = 0; i < spec->ntopk; i++)
	{
		const SiftV6TopKItem *it = &spec->topk_item[i];

		r->ssup[i] = (SortSupportData) {.ssup_cxt = cx,.ssup_collation = it->collid,.ssup_nulls_first = it->nulls_first,.ssup_attno = it->pos + 1};
		PrepareSortSupportFromOrderingOp(it->sortop, &r->ssup[i]);
		pos[i] = it->pos;
	}
	for (int c = 0; c < n; c++)
	{
		len[c] = TupleDescAttr(desc, c)->attlen;
		byval[c] = TupleDescAttr(desc, c)->attbyval;
	}
	r->count_agg = -1;
	if (spec->ntopk > 0 && spec->topk_item[0].pos >= spec->nkeys && (spec->topk_item[0].sortop == Int8LessOperator || spec->topk_item[0].sortop == INT8_GT_OPERATOR) &&
		(spec->agg[spec->topk_item[0].pos - spec->nkeys].kind == SV6_COUNT_STAR || spec->agg[spec->topk_item[0].pos - spec->nkeys].kind == SV6_COUNT))
	{
		r->count_agg = spec->topk_item[0].pos - spec->nkeys;
		r->count_desc = spec->topk_item[0].sortop == INT8_GT_OPERATOR;
	}
	r->heap = sift_gtopk_new(cx, spec->topk, spec->ntopk, pos, n, len, byval, item_cmp, r);
	r->values = MemoryContextAlloc(cx, Max(n, 1) * sizeof(Datum));
	r->isnull = MemoryContextAlloc(cx, Max(n, 1) * sizeof(bool));
	pfree(len);
	pfree(byval);
	return r;
}

/* a candidate whose sort values are made on demand from a group's slot */
typedef struct Cand
{
	const SiftV6Group *g;
	const V6Set *set;
	const SiftAggSlot64 *slot;
	const V6Acc *acc;
	SiftV6TopKRun *r;
	Datum	   *values;
	bool	   *isnull;
	uint32		keys_done;		/* the key columns already in values: the whole tuple of a candidate that stays makes only the others */
	uint64		aggs_done;		/* the same for the aggregates (the first 64) */
} Cand;

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
			v6g_key_datums(c->g, c->set, &c->set->ht, c->slot, c->values, c->isnull, UINT32_C(1) << pos);
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

	v6g_key_datums(c->g, c->set, &c->set->ht, c->slot, c->values, c->isnull, ~c->keys_done);
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
offer_whole(SiftV6TopKRun *r, Datum *values, bool *isnull, ExprState *qual, ExprContext *ec, TupleTableSlot *slot)
{
	Cand		c = {.r = r,.values = values,.isnull = isnull};

	if (having_passes(r, qual, ec, slot, values, isnull) && sift_gtopk_wants(r->heap, cand_whole, &c))
		sift_gtopk_put(r->heap, values, isnull);
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

void
sift_v6_topk_run_groups(SiftV6TopKRun *r, SiftV6Group *g, ExprState *qual, ExprContext *ec, TupleTableSlot *slot)
{
	const SiftV6Spec *spec = g->spec;
	const V6Set *set = &g->set[0];
	MemoryContext old = MemoryContextSwitchTo(r->tmp);
	const SiftAggSlot64 *s;

	if (g->store != NULL || set->nk == 0)
	{
		while (sift_v6_group_next(g, r->values, r->isnull))
		{
			offer_whole(r, r->values, r->isnull, qual, ec, slot);
			tmp_release(r);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(r->tmp);
		sift_gtopk_finish(r->heap);
		return;
	}
	SIFT_HT_FOREACH(&set->ht, s)
	{
		const V6Acc *acc = (const V6Acc *) sift_ht_ext(&set->ht, s);
		Cand		c = {.g = g,.set = set,.slot = s,.acc = acc,.r = r,.values = r->values,.isnull = r->isnull};
		Datum		worst;
		bool		worst_null;

		if (r->count_agg >= 0 && sift_gtopk_worst(r->heap, 0, &worst, &worst_null) && !worst_null &&
			(r->count_desc ? (int64) acc[r->count_agg].n < DatumGetInt64(worst) : (int64) acc[r->count_agg].n > DatumGetInt64(worst)))
			continue;

		memset(r->isnull, true, spec->nkeys * sizeof(bool));
		if (qual != NULL)
		{
			cand_whole_tuple(&c);
			offer_whole(r, r->values, r->isnull, qual, ec, slot);
		}
		else if (sift_gtopk_wants(r->heap, cand_lazy, &c))
		{
			cand_whole_tuple(&c);
			sift_gtopk_put(r->heap, r->values, r->isnull);
		}
		tmp_release(r);
	}
	MemoryContextReset(r->tmp);
	MemoryContextSwitchTo(old);
	sift_gtopk_finish(r->heap);
}

bool
sift_v6_topk_run_next(SiftV6TopKRun *r, Datum *values, bool *isnull)
{
	const Datum *v;
	const bool *n;

	if (r->at >= sift_gtopk_count(r->heap))
		return false;
	sift_gtopk_row(r->heap, r->at++, &v, &n);
	memcpy(values, v, r->desc->natts * sizeof(Datum));
	memcpy(isnull, n, r->desc->natts * sizeof(bool));
	return true;
}

bool
sift_v6_topk_run_wants(SiftV6TopKRun *r, SiftTopKValue value, void *ctx)
{
	return sift_gtopk_wants(r->heap, value, ctx);
}

void
sift_v6_topk_run_put(SiftV6TopKRun *r, const Datum *values, const bool *isnull)
{
	sift_gtopk_put(r->heap, values, isnull);
}

void
sift_v6_topk_run_finish(SiftV6TopKRun *r)
{
	sift_gtopk_finish(r->heap);
}
