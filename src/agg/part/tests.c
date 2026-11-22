/*
 * agg/part/tests.c - what each partition applies to the units it stores
 * The filters, the pre-aggregate, the Vulkan and the recheck callbacks of the provider; every
 * partition gets them and the call goes to the partition in turn.
 */
#include "postgres.h"

#include "agg/part/int.h"

/* what the partitions are told is kept and told again to each source opened later: a partition's source is not open for the whole scan (agg/part/open.c) */
uint64
part_apply(const PartOp *op, SiftSrcSource *s)
{
	switch (op->kind)
	{
		case PO_FILTERS:
			return sift_colsrc_set_filters(s, op->arg, op->n);
		case PO_PRUNE_STR:
			sift_colsrc_prune_str(s, op->arg, op->n);
			return 0;
		case PO_WIDE:
			return sift_colsrc_set_wide(s, op->arg, op->n);
		case PO_TOLERATE_OVF:
			sift_colsrc_tolerate_ovf(s, op->leaf);
			return 0;
		case PO_RAW:
			sift_colsrc_set_raw(s, op->leaf);
			return 0;
		case PO_NUMSUM:
			sift_colsrc_set_numsum(s, op->leaf);
			return 0;
		case PO_RECHECK:
			sift_colsrc_set_recheck(s, op->leaf, op->mode, op->key, op->cast);
			return 0;
	}
	return 0;
}

/* the partitions share one layout, so they take the same tests: the first's answer is everyone's */
uint64
part_tell(PartSource *ps, const PartOp *op)
{
	uint64		answer = part_apply(op, ps->src[0]);

	ps->op = ps->nop == 0 ? MemoryContextAlloc(ps->mcxt, sizeof(PartOp)) : repalloc(ps->op, (ps->nop + 1) * sizeof(PartOp));
	ps->op[ps->nop++] = *op;
	ps->opened = true;
	for (int i = 1; i < ps->n; i++)
		if (ps->src[i])
			(void) part_apply(op, ps->src[i]);
	return answer;
}

static const void *
copy_in(PartSource *ps, const void *p, Size len)
{
	return memcpy(MemoryContextAlloc(ps->mcxt, Max(len, 1)), p, len);
}

void
counters_add(SiftSrcCounters *c, const SiftSrcCounters *pc)
{
	c->units += pc->units;
	c->units_pruned += pc->units_pruned;
	c->units_set_pruned += pc->units_set_pruned;
	c->units_search_pruned += pc->units_search_pruned;
	c->rows += pc->rows;
	c->bytes += pc->bytes;
	c->rows_vetted += pc->rows_vetted;
	c->heap_pages += pc->heap_pages;
	c->rechecked += pc->rechecked;
	c->recheck_units += pc->recheck_units;
	c->point_units += pc->point_units;
	c->point_rows += pc->point_rows;
	c->run_windows += pc->run_windows;
	c->dirty_rows += pc->dirty_rows;
	c->rows_wbuf += pc->rows_wbuf;
}

void
part_counters(const void *impl, SiftSrcCounters *c)
{
	const PartSource *ps = impl;

	*c = ps->done;
	for (int i = 0; i < ps->n; i++)
	{
		SiftSrcCounters pc;

		if (ps->src[i] == NULL)
			continue;
		sift_colsrc_counters(ps->src[i], &pc);
		counters_add(c, &pc);
	}
}

bool
part_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_overflow(cur_of(impl), unit_of(impl), leaf);
}

void
part_prune_str(void *impl, const SiftSrcStrTest *t, int nt)
{
	PartSource *ps = impl;

	(void) part_tell(ps, &(PartOp) {PO_PRUNE_STR, .n = nt, .arg = copy_in(ps, t, nt * sizeof(SiftSrcStrTest))});
}

bool
part_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	return sift_colsrc_filter_str(cur_of(impl), unit_of(impl), t, rows);
}

int
part_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	PartSource *ps = impl;

	return (int) part_tell(ps, &(PartOp) {PO_WIDE, .n = nt, .arg = copy_in(ps, t, nt * sizeof(SiftSrcWideTest))});
}

bool
part_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows)
{
	return sift_colsrc_filter_wide(cur_of(impl), unit_of(impl), i, rows);
}

uint64
part_set_filters(void *impl, const SiftSrcFilter *f, int nf)
{
	PartSource *ps = impl;

	return part_tell(ps, &(PartOp) {PO_FILTERS, .n = nf, .arg = copy_in(ps, f, nf * sizeof(SiftSrcFilter))});
}

bool
part_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	return sift_colsrc_filter(cur_of(impl), unit_of(impl), bits);
}

/* the rows a unit keeps after the filters, as the child reads them: the child narrows its own unit, the wrapper's follows */
bool
part_narrow(void *impl, SiftSrcUnit *unit, const uint64 *bits)
{
	PartSource *ps = impl;

	if (!sift_colsrc_can_narrow(cur_of(impl)) || !sift_colsrc_narrow(cur_of(impl), &ps->cunit, bits))
		return false;
	unit->nrow = ps->cunit.nrow;
	return true;
}

bool
part_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	return sift_colsrc_filter_types(cur_of(impl), unit_of(impl), leaf, types, bits);
}

void
part_tolerate_ovf(void *impl, int leaf)
{
	PartSource *ps = impl;

	(void) part_tell(ps, &(PartOp) {PO_TOLERATE_OVF, .leaf = leaf});
}

void
part_set_raw(void *impl, int leaf)
{
	PartSource *ps = impl;

	(void) part_tell(ps, &(PartOp) {PO_RAW, .leaf = leaf});
}

void
part_set_numsum(void *impl, int leaf)
{
	PartSource *ps = impl;

	(void) part_tell(ps, &(PartOp) {PO_NUMSUM, .leaf = leaf});
}

void
part_set_recheck(void *impl, int leaf, int mode, const char *key, Oid cast)
{
	PartSource *ps = impl;

	(void) part_tell(ps, &(PartOp) {PO_RECHECK, .leaf = leaf, .mode = mode, .cast = cast, .key = key ? MemoryContextStrdup(ps->mcxt, key) : NULL});
}

void
part_overflow_raise(void *impl, const SiftSrcUnit *unit, const uint64 *rows)
{
	sift_colsrc_overflow_raise(cur_of(impl), unit_of(impl), rows);
}

void
part_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	sift_colsrc_read_types(cur_of(impl), unit_of(impl), leaf, tags);
}

void
part_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits)
{
	sift_colsrc_read_jnull(cur_of(impl), unit_of(impl), leaf, bits);
}

bool
part_unit_exact(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_unit_exact(cur_of(impl), unit_of(impl));
}

int
part_unit_dead(void *impl, const SiftSrcUnit *unit, int mode)
{
	return sift_colsrc_unit_dead(cur_of(impl), unit_of(impl), mode);
}

bool
part_unit_except(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_unit_except(cur_of(impl), unit_of(impl), leaf);
}

bool
part_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	return sift_colsrc_unit_agg(cur_of(impl), unit_of(impl), leaf, agg);
}

bool
part_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, const SiftSrcRollupNeed *need, SiftSrcHist *h)
{
	if (!sift_colsrc_unit_hist(cur_of(impl), unit_of(impl), dim, need, h))
		return false;
	h->dict = presented(impl, SLOT_HIST(ps_of(impl), dim), h->dict);
	return true;
}

uint32
part_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list)
{
	return sift_colsrc_unit_rollup_tuples(cur_of(impl), unit_of(impl), list);
}

bool
part_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *r)
{
	if (!sift_colsrc_unit_rollup(cur_of(impl), unit_of(impl), list, need, r))
		return false;
	for (int j = 0; j < r->ndims; j++)
		r->dict[j] = presented(impl, SLOT_ROLLUP(ps_of(impl), j), r->dict[j]);
	return true;
}

bool
part_unit_rollup_miss(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_unit_rollup_miss(cur_of(impl), unit_of(impl));
}

uint64 *
part_recheck_rows(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_recheck_rows(cur_of(impl), unit_of(impl));
}
