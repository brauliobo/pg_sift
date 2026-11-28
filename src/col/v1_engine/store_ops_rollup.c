/*
 * col/v1_engine/store_ops_rollup.c - the statements a Format 1 unit answers from the rollup of its rows (col/v1_engine/store_ops_rollup.h).
 *
 * The rollup is read with every leaf of the keys and the tests as a dim. A tuple is one group of the unit by all of its dims: a test of a dim is made on the value the tuple has there (the one
 * group of a histogram of that value, or its NULL group), and the keys of the tuple are the values of the key dims, so that two tuples that differ only in another dim fold into one group.
 */
/* check-memory: allow-allocs every array here follows the keys and tests of one statement, not the rows of the table */
#include "postgres.h"

#include "agg/vexpr/str.h"
#include "col/v1_engine/store_ops_rollup.h"

#define ROLLUP_KEYS		2

struct SiftV1Rollup
{
	SiftSrcRollupNeed need;		/* the dims: the keys, then the leaves of the tests */
	int			nkeys;
	int			key[ROLLUP_KEYS];
	bool		key_text[ROLLUP_KEYS];
	const SiftV1Pred *pred;
	int			npred;
	SiftSrcRollup r;			/* the last read, and the dim there of each key and test */
	int			kdim[ROLLUP_KEYS];
	int		   *pdim;
	void	   *kdata[ROLLUP_KEYS];
	uint64		kvalid[ROLLUP_KEYS][SIFT_VEC_WORDS];
	SiftVec		kvec[ROLLUP_KEYS];
	SiftVec    *kvp[ROLLUP_KEYS];
};

/* Leaf as a dim of need: false when the rollup would hold more dims than one can */
static bool
dim_add(SiftSrcRollupNeed *need, int leaf)
{
	for (int j = 0; j < need->ndims; j++)
		if (need->dim[j] == leaf)
			return true;
	if (need->ndims == SIFT_SRC_MAX_RDIMS)
		return false;
	need->dim[need->ndims++] = leaf;
	return true;
}

SiftV1Rollup *
sift_v1_rollup_open(MemoryContext cx, const int *key, const bool *key_text, int nkeys, const SiftV1Pred *pred, int npred, const SiftSrcRollupNeed *need)
{
	SiftSrcRollupNeed n = *need;
	SiftV1Rollup *ru;

	Assert(nkeys <= ROLLUP_KEYS);
	n.ndims = 0;
	for (int k = 0; k < nkeys; k++)
		if (!dim_add(&n, key[k]))
			return NULL;
	for (int i = 0; i < npred; i++)
		if (!dim_add(&n, pred[i].leaf))
			return NULL;
	if (n.ndims < 2)
		return NULL;
	ru = MemoryContextAllocZero(cx, sizeof(SiftV1Rollup));
	ru->need = n;
	ru->nkeys = nkeys;
	ru->pred = pred;
	ru->npred = npred;
	ru->pdim = MemoryContextAlloc(cx, Max(npred, 1) * sizeof(int));
	for (int k = 0; k < nkeys; k++)
	{
		ru->key[k] = key[k];
		ru->key_text[k] = key_text[k];
		ru->kdata[k] = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		ru->kvp[k] = &ru->kvec[k];
	}
	return ru;
}

/* The dim of r that is leaf, -1: none */
static int
dim_of(const SiftSrcRollup *r, int leaf)
{
	for (int j = 0; j < r->ndims; j++)
		if (r->dim[j] == leaf)
			return j;
	return -1;
}

bool
sift_v1_rollup_read(SiftV1Rollup *ru, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, SiftSrcHist *h)
{
	const SiftSrcRollup *r = &ru->r;

	if (!rd->unit_rollup(src, unit, -1, &ru->need, &ru->r))
		return false;
	for (int k = 0; k < ru->nkeys; k++)
		if ((ru->kdim[k] = dim_of(r, ru->key[k])) < 0 || (ru->key_text[k] && r->dict[ru->kdim[k]] == NULL))
			return false;
	for (int i = 0; i < ru->npred; i++)
		if ((ru->pdim[i] = dim_of(r, ru->pred[i].leaf)) < 0)
			return false;
	*h = (SiftSrcHist) {.ngroups = r->ntuples,.count = r->count,.nmeas = r->nmeas};
	for (int m = 0; m < r->nmeas; m++)
	{
		h->meas[m] = r->meas[m];
		h->nn[m] = r->nn[m];
		h->sum[m] = r->sum[m];
		h->sumf[m] = r->sumf[m];
		h->min[m] = r->min[m];
		h->max[m] = r->max[m];
	}
	return true;
}

static bool
null_at(const SiftSrcRollup *r, int j, uint32 t)
{
	return r->isnull[j] != NULL && r->isnull[j][t];
}

bool
sift_v1_rollup_kept(const SiftV1Rollup *ru, uint32 t)
{
	const SiftSrcRollup *r = &ru->r;
	const SiftSrcHist null_group = {.ngroups = 1,.has_null = true};

	for (int i = 0; i < ru->npred; i++)
	{
		const int	j = ru->pdim[i];
		const SiftSrcHist values = {.ngroups = r->ntuples,.key = r->key[j],.dict = r->dict[j]};

		if (!(null_at(r, j, t) ? sift_v1_pred_passes(&ru->pred[i], &null_group, 0) : sift_v1_pred_passes(&ru->pred[i], &values, t)))
			return false;
	}
	return true;
}

void
sift_v1_rollup_put(SiftV1Rollup *ru, uint32 t, int row)
{
	const SiftSrcRollup *r = &ru->r;

	if (row == 0)
		memset(ru->kvalid, 0, sizeof(ru->kvalid));
	for (int k = 0; k < ru->nkeys; k++)
	{
		const int	j = ru->kdim[k];
		const int64 v = r->key[j][t];
		const SiftSrcDict *d = r->dict[j];

		if (null_at(r, j, t))
			continue;
		ru->kvalid[k][row >> 6] |= UINT64CONST(1) << (row & 63);
		if (ru->key_text[k])
			((SiftStrView *) ru->kdata[k])[row] = sift_vstr_make(d->bytes + d->off[v], d->off[v + 1] - d->off[v]);
		else
			((int64 *) ru->kdata[k])[row] = v;
	}
}

SiftVec *const *
sift_v1_rollup_keys(SiftV1Rollup *ru, int n)
{
	for (int k = 0; k < ru->nkeys; k++)
		ru->kvec[k] = (SiftVec) {.data = ru->kdata[k],.valid = ru->kvalid[k],.len = (uint16) n,.dense = true,.type = ru->key_text[k] ? SVEC_STRVIEW : SVEC_I64};
	return ru->kvp;
}
