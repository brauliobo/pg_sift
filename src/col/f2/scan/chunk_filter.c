/*
 * col/f2/scan/chunk_filter.c - the predicates a scan runs on the chunks of a granule as they are stored, in the order the scan learned (col/f2/f2_lazy.h): which chunks are read, which
 * predicate runs on them compressed, and the rows that pass.
 */
#include "postgres.h"

#include "portability/instr_time.h"

#include "col/f2/scan/scan_int.h"

/* What the lazy filter driver (col/f2/f2_lazy.h) reads and tests in one granule of a scan */
typedef struct GranuleFilter
{
	Relation	rel;
	F2Scan	   *s;
	SiftLazyGranule *lg;
	const SiftGranuleView *v;
	const PartPreds *pp;
	const bool *exceptions;
	bool	   *decided;
	instr_time	t0;
} GranuleFilter;

static uint64
gf_load(GranuleFilter *gf, const int *chunks, int n)
{
	uint64		bytes = 0;

	for (int i = 0; i < n; i++)
		if (!gf->lg->loaded[chunks[i]])
			bytes += ((const SiftGranuleHeader *) gf->lg->rec)->chunks[chunks[i]].len;
	sift_lazy_granule_load(gf->rel, gf->lg, chunks, n);
	return bytes;
}

static uint64
gf_load_cols(void *ctx, uint32 g, const int *cols, int ncols)
{
	int			chunks[SIFT_AFILT_MAX];

	for (int i = 0; i < ncols; i++)
		chunks[i] = cols[i] + 1;
	return gf_load(ctx, chunks, ncols);
}

static uint64
gf_load_whole(void *ctx, uint32 g)
{
	GranuleFilter *gf = ctx;
	int			want[SIFT_AFILT_MAX + 1],
				nw = 0;

	want[nw++] = SIFT_COL_TID_IDX;
	for (int k = 0; k < gf->s->lf.af.n; k++)
		if (!gf->decided[k])
			want[nw++] = gf->s->preds[k].col + 1;
	return gf_load(gf, want, nw);
}

static bool
gf_runnable(void *ctx, uint32 g, int slot)
{
	GranuleFilter *gf = ctx;

	return !gf->decided[slot] && !gf->exceptions[slot] && gf->s->cf_ok[slot];
}

static bool
gf_eval(void *ctx, uint32 g, int slot, uint32 nrows, uint64 *active)
{
	GranuleFilter *gf = ctx;
	uint64		mask[SIFT_CFILT_WORDS + 2];
	SiftCfiltResult res = sift_granule_filter_compressed(gf->v, &gf->pp->cf[slot], mask);

	if (res == SIFT_CFILT_UNSUPPORTED)
		return false;
	if (res == SIFT_CFILT_ALL_FALSE)
		memset(active, 0, sizeof(uint64) * SIFT_CFILT_WORDS);
	else if (res == SIFT_CFILT_BITS)
		for (uint32 w = 0; w < (nrows + 63) / 64; w++)
			active[w] &= mask[w];
	gf->decided[slot] = true;
	F2_STAT(compressed);
	return true;
}

static double
gf_now_ns(void *ctx)
{
	GranuleFilter *gf = ctx;
	instr_time	t;

	INSTR_TIME_SET_CURRENT(t);
	INSTR_TIME_SUBTRACT(t, gf->t0);
	return (double) INSTR_TIME_GET_NANOSEC(t);
}

static uint32
gf_chunk_len(void *ctx, uint32 g, int col)
{
	return ((const SiftGranuleHeader *) ((GranuleFilter *) ctx)->lg->rec)->chunks[col + 1].len;
}

const SiftLfOps sift_f2_granule_filter_ops = {gf_load_cols, gf_load_whole, gf_eval, gf_now_ns, gf_chunk_len, gf_runnable};

/* The rows of the granule that pass every predicate the chunks could tell, and the predicates they decided */
uint32
sift_f2_filter_chunks(Relation rel, F2Scan *s, uint32 g, SiftLazyGranule *lg, const SiftGranuleView *v, const PartPreds *pp, const bool *exceptions, bool *decided, uint64 *active)
{
	GranuleFilter gf = {.rel = rel,.s = s,.lg = lg,.v = v,.pp = pp,.exceptions = exceptions,.decided = decided};

	INSTR_TIME_SET_CURRENT(gf.t0);
	s->lf.ctx = &gf;
	return sift_lf_granule(&s->lf, g, lg->gd.nrows, active).n_active;
}
