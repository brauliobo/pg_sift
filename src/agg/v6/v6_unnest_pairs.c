/*
 * agg/v6/v6_unnest_pairs.c - the pairs of agg/v6/v6_unnest_pairs.h.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include <stdalign.h>

#include "agg/v6/v6_unnest_pairs.h"
#include "agg/vexpr.h"

struct SiftUnPairs
{
	SiftV6Pipe *pipe;
	int			ncols;
	SiftBatchSel bs;
	uint32		pidx[SIFT_VEC_LEN];
	uint32		eidx[SIFT_VEC_LEN];
	SiftVec    *out[SIFT_VEXPR_MAX_REGS + 1];
	SiftVec		outv[SIFT_VEXPR_MAX_REGS + 1];
	uint64	   *out_valid[SIFT_VEXPR_MAX_REGS + 1];
	void	   *out_data[SIFT_VEXPR_MAX_REGS + 1];
};

SiftUnPairs *
sift_un_pairs_new(MemoryContext cx, SiftV6Pipe *pipe, int ncols, const SiftVecType *type)
{
	SiftUnPairs *p = MemoryContextAllocAligned(cx, sizeof(SiftUnPairs), alignof(SiftUnPairs), MCXT_ALLOC_ZERO);

	Assert(ncols <= SIFT_VEXPR_MAX_REGS);
	p->pipe = pipe;
	p->ncols = ncols;
	for (int s = 0; s <= ncols; s++)
	{
		p->out_data[s] = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sift_gather_width(type[s]) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		p->out_valid[s] = MemoryContextAllocZero(cx, SIFT_VEC_WORDS * sizeof(uint64));
		p->out[s] = &p->outv[s];
	}
	return p;
}

uint32 *
sift_un_pairs_pidx(SiftUnPairs *p)
{
	return p->pidx;
}

uint32 *
sift_un_pairs_eidx(SiftUnPairs *p)
{
	return p->eidx;
}

void
sift_un_pairs_emit(SiftUnPairs *p, const SiftVec *fact, const SiftVec *elems, uint32 m)
{
	SiftBatchSel *bs = &p->bs;

	for (int i = 0; i < p->ncols; i++)
		sift_vec_gather(p->out[i], p->out_valid[i], p->out_data[i], &fact[i], p->pidx, m);
	sift_vec_gather(p->out[p->ncols], p->out_valid[p->ncols], p->out_data[p->ncols], elems, p->eidx, m);
	memset(bs->mask, 0, sizeof(bs->mask));
	for (uint32 i = 0; i < m; i++)
		bs->mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	bs->n_raw = bs->n_active = (uint16) m;
	bs->all_active = true;
	bs->has_sel = false;
	sift_v6_pipe_run(p->pipe, p->out, bs);
}
