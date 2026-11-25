/*
 * agg/v6/unnest/pairs.c - the pairs of agg/v6/unnest/pairs.h.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include <stdalign.h>

#include "agg/v6/unnest/pairs.h"
#include "agg/vexpr/vexpr.h"

#define SIFT_UN_SLOTS	(SIFT_VEXPR_MAX_REGS + SIFT_UN_ELEMS_MAX)

struct SiftUnPairs
{
	SiftV6Pipe *pipe;
	int			ncols;
	int			nel;
	uint32		eused;
	SiftBatchSel bs;
	uint32		pidx[SIFT_VEC_LEN];
	uint32		eidx[SIFT_VEC_LEN];
	SiftVec    *out[SIFT_UN_SLOTS];
	SiftVec		outv[SIFT_UN_SLOTS];
	uint64	   *out_valid[SIFT_UN_SLOTS];
	void	   *out_data[SIFT_UN_SLOTS];
};

SiftUnPairs *
sift_un_pairs_new(MemoryContext cx, SiftV6Pipe *pipe, int ncols, int nel, uint32 eused)
{
	SiftUnPairs *p = MemoryContextAllocAligned(cx, sizeof(SiftUnPairs), alignof(SiftUnPairs), MCXT_ALLOC_ZERO);

	Assert(ncols <= SIFT_VEXPR_MAX_REGS && nel <= SIFT_UN_ELEMS_MAX);
	p->pipe = pipe;
	p->ncols = ncols;
	p->nel = nel;
	p->eused = eused;
	for (int s = 0; s < ncols + nel; s++)
	{
		p->out_data[s] = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sift_gather_width(pipe->type[s]) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
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
	for (int c = 0; c < p->nel; c++)
		if ((p->eused >> c) & 1)
			sift_vec_gather(p->out[p->ncols + c], p->out_valid[p->ncols + c], p->out_data[p->ncols + c], &elems[c], p->eidx, m);
	memset(bs->mask, 0, sizeof(bs->mask));
	for (uint32 i = 0; i < m; i++)
		bs->mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	bs->n_raw = bs->n_active = (uint16) m;
	bs->all_active = true;
	bs->has_sel = false;
	sift_v6_pipe_run(p->pipe, p->out, bs);
}
