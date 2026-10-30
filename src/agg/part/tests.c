/*
 * agg/part/tests.c - what each partition applies to the units it stores
 * The filters, the pre-aggregate, the Vulkan and the recheck callbacks of the provider; every
 * partition gets them and the call goes to the partition in turn.
 */
#include "postgres.h"

#include "agg/part/int.h"

void
part_counters(const void *impl, SiftSrcCounters *c)
{
	const PartSource *ps = impl;

	memset(c, 0, sizeof(*c));
	for (int i = 0; i < ps->n; i++)
	{
		SiftSrcCounters pc;

		if (ps->src[i] == NULL)
			continue;
		sift_colsrc_counters(ps->src[i], &pc);
		c->units += pc.units;
		c->units_pruned += pc.units_pruned;
		c->units_set_pruned += pc.units_set_pruned;
		c->units_search_pruned += pc.units_search_pruned;
		c->rows += pc.rows;
		c->bytes += pc.bytes;
		c->rows_vetted += pc.rows_vetted;
		c->heap_pages += pc.heap_pages;
		c->rechecked += pc.rechecked;
		c->recheck_units += pc.recheck_units;
		c->point_units += pc.point_units;
		c->point_rows += pc.point_rows;
		c->run_windows += pc.run_windows;
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

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			sift_colsrc_prune_str(ps->src[i], t, nt);
}

bool
part_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	return sift_colsrc_filter_str(cur_of(impl), unit_of(impl), t, rows);
}

/* the partitions share one layout, so they take the same tests: the first's answer is everyone's */
int
part_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	PartSource *ps = impl;
	int			n = 0;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			n = sift_colsrc_set_wide(ps->src[i], t, nt);
	return n;
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
	uint64		applied = 0;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			applied = sift_colsrc_set_filters(ps->src[i], f, nf);
	return applied;
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

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			sift_colsrc_tolerate_ovf(ps->src[i], leaf);
}

void
part_set_raw(void *impl, int leaf)
{
	PartSource *ps = impl;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			sift_colsrc_set_raw(ps->src[i], leaf);
}

void
part_set_numsum(void *impl, int leaf)
{
	PartSource *ps = impl;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			sift_colsrc_set_numsum(ps->src[i], leaf);
}

void
part_set_recheck(void *impl, int leaf, int mode, const char *key, Oid cast)
{
	PartSource *ps = impl;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			sift_colsrc_set_recheck(ps->src[i], leaf, mode, key, cast);
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
part_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	if (!sift_colsrc_unit_hist(cur_of(impl), unit_of(impl), dim, h))
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

bool
part_gpu_unit(void *impl, const SiftSrcUnit *unit, SiftSrcGpuUnit *g)
{
	return sift_colsrc_gpu_unit(cur_of(impl), unit_of(impl), g);
}

bool
part_gpu_dense(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_gpu_dense(cur_of(impl), unit_of(impl), leaf);
}

bool
part_gpu_chunk(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c)
{
	return sift_colsrc_gpu_chunk(cur_of(impl), unit_of(impl), leaf, c);
}

void
part_gpu_dict(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict)
{
	sift_colsrc_gpu_dict(cur_of(impl), unit_of(impl), leaf, dict);
	*dict = *presented(impl, leaf, dict);
}

uint64 *
part_recheck_rows(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_recheck_rows(cur_of(impl), unit_of(impl));
}
