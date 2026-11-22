/*
 * col/f2/f2_lazy.c - the lazy filter driver of a scan (col/f2/f2_lazy.h).
 */
#include "core/host.h"

#include "core/popcount.h"

#include "col/f2/f2_lazy.h"

uint32
sift_lf_count(const uint64 *active, uint32 nrows)
{
	uint32		n = 0;

	for (uint32 w = 0; w < (nrows + 63) / 64; w++)
		n += sift_pc64(active[w]);
	return n;
}

uint32
sift_lf_sparse_rows(const uint64 *active, uint32 nrows, uint32 *idx)
{
	uint32		n = 0;

	for (uint32 w = 0; w < (nrows + 63) / 64; w++)
		for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
			idx[n++] = w * 64 + (uint32) __builtin_ctzll(bits);
	return n;
}

SiftConjunctInit
sift_lf_prior(int col, uint8 barrier_group, const SiftPartColStat *st, uint64 nrows, uint64 nnull, SiftPredShape shape, int nvals, SiftCostClass cost)
{
	return (SiftConjunctInit) {.col = col,.barrier_group = barrier_group,.pass_prior = sift_col_stats_pass_prior(st, nrows, nnull, shape, nvals),
	.cpu_ns_row = sift_cost_class_ns(cost),.io_bytes_row = sift_col_stats_bytes_per_row(st, nrows)};
}

void
sift_lf_init(SiftLazyFilter *lf, const SiftLfOps *ops, void *ctx, const SiftConjunctInit *init, int n, bool io_cold)
{
	*lf = (SiftLazyFilter) {.ops = ops,.ctx = ctx};
	sift_afilt_init(&lf->af, init, n, io_cold);
	for (int i = 0; i < lf->af.n; i++)
		lf->col[i] = init[i].col;
}

/* Whether the column was read already in this granule by a conjunct before it: the columns read so far are kept in a short list */
static bool
column_loaded(const int *loaded, int nloaded, int col)
{
	for (int i = 0; i < nloaded; i++)
		if (loaded[i] == col)
			return true;
	return false;
}

SiftLfResult
sift_lf_granule(SiftLazyFilter *lf, uint32 g, uint32 nrows, uint64 *active)
{
	SiftLfResult r = {0};
	const uint16 *order = sift_afilt_order(&lf->af);
	bool		dense = sift_afilt_dense(&lf->af) && lf->ops->load_whole != NULL,
				timed = sift_afilt_sample_cpu(&lf->af) && lf->ops->now_ns != NULL;
	uint32		live = sift_lf_count(active, nrows);
	int			loaded[SIFT_AFILT_MAX],
				nloaded = 0;

	if (dense && live > 0)
	{
		r.filter_bytes += lf->ops->load_whole(lf->ctx, g);
		r.dense_read = true;
		lf->stats.whole_reads++;
	}
	for (int i = 0; i < lf->af.n; i++)
	{
		int			slot = order[i];
		uint32		before = live,
					io = 0;
		double		t0 = 0.0,
					t1;

		if (lf->ops->runnable != NULL && !lf->ops->runnable(lf->ctx, g, slot))
			continue;
		if (live == 0)
		{
			sift_afilt_skipped(&lf->af, slot);
			lf->stats.conjunct_skips++;
			continue;
		}
		if (!dense && !column_loaded(loaded, nloaded, lf->col[slot]))
		{
			io = (uint32) lf->ops->load(lf->ctx, g, &lf->col[slot], 1);
			loaded[nloaded++] = lf->col[slot];
			r.filter_bytes += io;
		}
		if (timed)
			t0 = lf->ops->now_ns(lf->ctx);
		if (!lf->ops->eval(lf->ctx, g, slot, nrows, active))
		{
			r.undecided |= UINT64CONST(1) << slot;
			continue;
		}
		t1 = timed ? lf->ops->now_ns(lf->ctx) - t0 : 0.0;
		live = sift_lf_count(active, nrows);
		sift_afilt_record(&lf->af, slot, before, live, t1, io, nrows);
		lf->stats.conjunct_runs++;
	}
	r.n_active = live;
	r.mode = live == 0 ? SIFT_LF_SKIP : live * SIFT_LF_SPARSE_DIV < nrows ? SIFT_LF_SPARSE : SIFT_LF_DENSE;
	lf->stats.granules++;
	lf->stats.filter_bytes += r.filter_bytes;
	lf->stats.skipped += r.mode == SIFT_LF_SKIP;
	lf->stats.sparse += r.mode == SIFT_LF_SPARSE;
	lf->stats.dense += r.mode == SIFT_LF_DENSE;
	sift_afilt_granule_done(&lf->af, live > 0);
	return r;
}

uint64
sift_lf_load_payload(SiftLazyFilter *lf, uint32 g, const int *cols, int ncols, const SiftLfResult *r)
{
	uint64		bytes = 0;

	if (r->mode == SIFT_LF_SKIP)
	{
		if (lf->ops->chunk_len != NULL)
			for (int i = 0; i < ncols; i++)
				lf->stats.payload_bytes_saved += lf->ops->chunk_len(lf->ctx, g, cols[i]);
		return 0;
	}
	bytes = lf->ops->load(lf->ctx, g, cols, ncols);
	lf->stats.payload_bytes_read += bytes;
	return bytes;
}
