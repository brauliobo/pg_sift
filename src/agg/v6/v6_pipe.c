/*
 * agg/v6/v6_pipe.c - the batches of the fact side through the dimensions of a star join to the groups (agg/v6/v6_pipe.h).
 */
/* check-memory: allow-allocs every array here follows the slots of one statement, not the rows of the table */
#include "postgres.h"

#include <stdalign.h>

#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_pipe.h"
#include "core/popcount.h"

struct SiftV6PipeBatch
{
	SiftVec    *vec;			/* [nslots] */
	uint64	   *valid;			/* [nslots * SIFT_VEC_WORDS] */
	char	  **data;			/* [nslots] the values of the slots this batch owns */
	SiftBatchSel own;
	SiftBatchSel *bs;
};

SiftV6Pipe *
sift_v6_pipe_new(MemoryContext cx, const SiftV6Spec *spec, SiftV6Group *group, int nfact, const SiftVecType *ftype)
{
	SiftV6Pipe *p = MemoryContextAllocZero(cx, sizeof(SiftV6Pipe));
	const SiftV6Ext *x = spec->ext;
	int			at = nfact;

	p->cx = cx;
	p->spec = spec;
	p->group = group;
	p->nfact = nfact;
	p->ndims = x != NULL ? x->ndims : 0;
	for (int d = 0; d < p->ndims; d++)
	{
		p->pay_slot[d] = at;
		at += x->dim[d].npay;
		p->nparts[d] = 1;
	}
	p->nslots = at;
	p->type = MemoryContextAlloc(cx, p->nslots * sizeof(SiftVecType));
	for (int i = 0; i < nfact; i++)
		p->type[i] = ftype[i];
	for (int d = 0; d < p->ndims; d++)
		for (int c = 0; c < x->dim[d].npay; c++)
		{
			const SiftV6Class cls = sift_v6_class(x->dim[d].ptype[c]);

			p->type[p->pay_slot[d] + c] = cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
		}
	p->key_slot = MemoryContextAlloc(cx, Max(spec->nkeys, 1) * sizeof(int));
	p->arg_slot = MemoryContextAlloc(cx, Max(sift_v6_spec_nvec(spec), 1) * sizeof(int));
	p->keyvec = MemoryContextAlloc(cx, Max(spec->nkeys, 1) * sizeof(SiftVec *));
	p->argvec = MemoryContextAlloc(cx, Max(sift_v6_spec_nvec(spec), 1) * sizeof(SiftVec *));
	p->match = MemoryContextAlloc(cx, SIFT_VEC_LEN * sizeof(uint32));
	p->fact_idx = MemoryContextAlloc(cx, SIFT_VEC_LEN * sizeof(uint16));
	p->dim_row = MemoryContextAlloc(cx, SIFT_VEC_LEN * sizeof(uint32));
	return p;
}

/* the column the vector v of a batch is: argvec[j] is the argument of aggregate j, argvec[naggs + j] its second, then the columns of the COUNT(DISTINCT ..) rows and those of the FILTER clauses */
const SiftV6Col *
sift_v6_vec_col(const SiftV6Spec *spec, int v)
{
	if (v < spec->naggs)
		return sift_v6_kind_reads_column(spec->agg[v].kind) ? &spec->agg[v].arg : NULL;
	if (v < 2 * spec->naggs)
		return spec->agg[v - spec->naggs].kind == SV6_ARG_MAX || spec->agg[v - spec->naggs].kind == SV6_ARG_MIN ? &spec->agg[v - spec->naggs].arg2 : NULL;
	if (v >= sift_v6_spec_fvec(spec))
		return &spec->fcol[v - sift_v6_spec_fvec(spec)];
	for (int j = 0; j < spec->naggs; j++)
		if (v >= sift_v6_agg_xvec(spec, j) && v < sift_v6_agg_xvec(spec, j) + spec->agg[j].nxcols)
			return &spec->agg[j].xcol[v - sift_v6_agg_xvec(spec, j)];
	return NULL;
}

int
sift_v6_pipe_dim_slot(const SiftV6Pipe *p, int dim, int pay)
{
	return p->pay_slot[dim] + pay;
}

static void *
vec_buffer(MemoryContext cx, SiftVecType t)
{
	return MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sift_gather_width(t) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
}

/* the batch of level l: level 0 borrows the fact vectors, the others own every slot they can hold */
static SiftV6PipeBatch *
level_of(SiftV6Pipe *p, int l)
{
	SiftV6PipeBatch *b = p->lvl[l];

	if (b != NULL)
		return b;
	b = p->lvl[l] = MemoryContextAllocAligned(p->cx, sizeof(SiftV6PipeBatch), alignof(SiftV6PipeBatch), MCXT_ALLOC_ZERO);
	b->vec = MemoryContextAllocZero(p->cx, p->nslots * sizeof(SiftVec));
	b->valid = MemoryContextAllocZero(p->cx, (size_t) p->nslots * SIFT_VEC_WORDS * sizeof(uint64));
	b->data = MemoryContextAllocZero(p->cx, p->nslots * sizeof(char *));
	for (int s = l == 0 ? p->nfact : 0; s < p->nslots; s++)
		b->data[s] = vec_buffer(p->cx, p->type[s]);
	b->bs = &b->own;
	return b;
}

static void
count_active(SiftBatchSel *bs)
{
	uint32		n = 0;

	for (uint32 w = 0; w < (uint32) ((bs->n_raw + 63) / 64); w++)
		n += (uint32) sift_pc64(bs->mask[w]);
	bs->n_active = (uint16) n;
	bs->all_active = n == bs->n_raw;
	bs->has_sel = false;
}

static void
update_groups(SiftV6Pipe *p, SiftV6PipeBatch *b)
{
	const SiftV6Spec *spec = p->spec;

	for (int i = 0; i < spec->nkeys; i++)
		p->keyvec[i] = &b->vec[p->key_slot[i]];
	for (int j = 0; j < sift_v6_spec_nvec(spec); j++)
		p->argvec[j] = p->arg_slot[j] < 0 ? NULL : &b->vec[p->arg_slot[j]];
	sift_v6_group_update(p->group, b->bs, p->keyvec, p->argvec);
	p->rows_out += b->bs->n_active;
}

/* the payload columns of dimension d for the rows b holds, from the rows of the dimension dim_row names (0: none, a NULL) */
static void
gather_payload(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const uint32 *dim_row, uint32 n)
{
	const SiftStarDim *dim = p->dim[d];

	for (int c = 0; c < p->spec->ext->dim[d].npay; c++)
	{
		const int	s = p->pay_slot[d] + c;
		uint64	   *valid = b->valid + (size_t) s * SIFT_VEC_WORDS;

		if (p->type[s] == SVEC_STRVIEW)
			sift_star_gather_str(dim, c, dim_row, (uint16) n, (SiftStrView *) b->data[s], valid);
		else
			sift_star_gather_bits(dim, c, dim_row, (uint16) n, (uint64 *) b->data[s], valid);
		b->vec[s] = (SiftVec) {.data = b->data[s],.valid = valid,.len = (uint16) n,.all_valid = false,.dense = true,.type = p->type[s]};
	}
}

static void
restrict_part(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const SiftStarProbe *pr)
{
	SiftBatchSel *bs = b->bs;

	for (uint32 r = 0; r < bs->n_raw; r++)
		if (((bs->mask[r >> 6] >> (r & 63)) & 1) && sift_star_probe_part(&p->dim[d]->shape, pr, r, p->nparts[d]) != p->part[d])
			bs->mask[r >> 6] &= ~(UINT64CONST(1) << (r & 63));
	count_active(bs);
}

static void stage(SiftV6Pipe *p, int d, SiftV6PipeBatch *b);

/* a dimension with a key more than once: the pairs of (row, dimension row), a batch of them at a time, go on to the next dimension */
static void
expand(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const SiftStarProbe *pr)
{
	SiftStarCursor cur = {0};
	uint32	   *idx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	const int	nown = p->pay_slot[d];

	while (!sift_star_cursor_done(&cur, pr->n))
	{
		const uint32 m = sift_star_expand(p->dim[d], pr, NULL, &cur, p->fact_idx, p->dim_row, SIFT_VEC_LEN);
		SiftV6PipeBatch *nb;

		if (m == 0)
			break;
		nb = level_of(p, d + 1);
		for (uint32 i = 0; i < m; i++)
			idx[i] = p->fact_idx[i];
		for (int s = 0; s < nown; s++)
			sift_vec_gather(&nb->vec[s], nb->valid + (size_t) s * SIFT_VEC_WORDS, nb->data[s], &b->vec[s], idx, m);
		gather_payload(p, d, nb, p->dim_row, m);
		memset(nb->own.mask, 0, sizeof(nb->own.mask));
		for (uint32 i = 0; i < m; i++)
			nb->own.mask[i >> 6] |= UINT64CONST(1) << (i & 63);
		nb->own.n_raw = nb->own.n_active = (uint16) m;
		nb->own.all_active = true;
		nb->own.has_sel = false;
		stage(p, d + 1, nb);
	}
	pfree(idx);
}

static void
stage(SiftV6Pipe *p, int d, SiftV6PipeBatch *b)
{
	SiftBatchSel *bs = b->bs;
	const SiftStarDim *dim;
	SiftStarProbe pr;

	if (bs->n_active == 0)
		return;
	if (d == p->ndims)
	{
		update_groups(p, b);
		return;
	}
	dim = p->dim[d];
	pr = (SiftStarProbe) {.n = bs->n_raw,.active = bs->mask};
	for (int k = 0; k < dim->shape.nkeys; k++)
		pr.key[k] = &b->vec[p->fslot[d][k]];
	if (p->nparts[d] > 1)
	{
		restrict_part(p, d, b, &pr);
		if (bs->n_active == 0)
			return;
	}
	if (!dim->unique_keys)
	{
		expand(p, d, b, &pr);
		return;
	}
	sift_star_probe(dim, &pr, p->match);
	sift_star_apply(dim, p->match, NULL, bs->n_raw, bs->mask);
	count_active(bs);
	if (bs->n_active == 0)
		return;
	gather_payload(p, d, b, p->match, bs->n_raw);
	stage(p, d + 1, b);
}

void
sift_v6_pipe_run(SiftV6Pipe *p, SiftVec *const *fact, SiftBatchSel *bs)
{
	SiftV6PipeBatch *b = level_of(p, 0);

	for (int i = 0; i < p->nfact; i++)
		b->vec[i] = *fact[i];
	b->bs = bs;
	stage(p, 0, b);
}
