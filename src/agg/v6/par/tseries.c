/*
 * agg/v6/par/tseries.c - the batch of agg/v6/par/tseries.h.
 */
#include "postgres.h"

#include "agg/v6/par/tseries.h"
#include "core/popcount.h"

static bool
arena_full(SiftArena *a, SiftErr *err)
{
	sift_err_fill(err, SIFT_ERR_ARENA_FULL, 0, a->cap - a->used, "the arena of the thread has no room for a granule");
	return false;
}

static bool
serial(SiftErr *err, const char *why)
{
	sift_err_fill(err, SIFT_ERR_SERIAL, 0, 0, why);
	return false;
}

/* The bin of each active row of column c, by the kernel of the scan over slices of SIFT_VEC_LEN rows: bins[r] for the rows whose source is not NULL; NULL with err filled */
static int64 *
bin_column(const SiftTsKey *k, const SiftParCol *c, const uint64 *active, int n, SiftArena *a, SiftErr *err)
{
	SiftBatchSel *bs = sift_arena_alloc(a, sizeof(SiftBatchSel), SIFT_ARENA_ALIGN);
	uint64	   *valid = sift_arena_alloc(a, SIFT_VEC_WORDS * sizeof(uint64), SIFT_ARENA_ALIGN);
	int64	   *bins = sift_arena_alloc(a, Max(n, 1) * sizeof(int64) + SIFT_VEC_PAD, SIFT_ARENA_ALIGN);

	if (bs == NULL || valid == NULL || bins == NULL)
	{
		arena_full(a, err);
		return NULL;
	}
	for (int at = 0; at < n; at += SIFT_VEC_LEN)
	{
		const int	cnt = Min(SIFT_VEC_LEN, n - at),
					words = (cnt + 63) / 64;
		SiftVec		in = {.data = (void *) (c->v + at),.valid = valid,.len = (uint16) cnt,.type = SVEC_I64};
		uint32		err_row;

		memset(bs->mask, 0, sizeof(bs->mask));
		memset(valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
		memcpy(bs->mask, active + at / 64, words * sizeof(uint64));
		if (cnt & 63)
			bs->mask[words - 1] &= (UINT64CONST(1) << (cnt & 63)) - 1;
		bs->n_raw = (uint16) cnt;
		bs->all_active = false;
		bs->has_sel = false;
		bs->n_active = 0;
		for (int w = 0; w < words; w++)
			bs->n_active += (uint16) sift_pc64(bs->mask[w]);
		if (bs->n_active == 0)
			continue;
		for (int i = 0; i < cnt; i++)
			valid[i >> 6] |= (uint64) !c->isnull[at + i] << (i & 63);
		if (k->month)
			sift_datemonth_batch(bs, &in, bins + at);
		else if (sift_datebin_batch(&k->bin, bs, &in, bins + at, &err_row) != SDB_OK)
		{
			serial(err, "a bin that raises is the scan's");
			return NULL;
		}
	}
	return bins;
}

/* Whether a column of a granule has a NULL in any of its n rows */
static bool
has_null(const SiftParCol *c, int n)
{
	return memchr(c->isnull, 1, n) != NULL;
}

static bool
columns_have_null(const SiftTsShape *sh, const SiftParCol *col, int n)
{
	for (int j = 0; j < sh->nk; j++)
		if (has_null(&col[sh->key[j].at], n))
			return true;
	for (int m = 0; m < sh->nsum; m++)
		if (has_null(&col[sh->sum_at[m]], n))
			return true;
	for (int c = 0; c < sh->ncount; c++)
		if (has_null(&col[sh->count_at[c]], n))
			return true;
	return false;
}

/* The rows of the bitmap in order, nact of them */
static uint32 *
rows_of(const uint64 *active, uint32 nact, int n, SiftArena *a)
{
	uint32	   *sel = sift_arena_alloc(a, Max(nact, 1) * sizeof(uint32), 8);
	uint32		i = 0;

	for (int w = 0; sel != NULL && w < (n + 63) / 64; w++)
		for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
			sel[i++] = w * 64 + __builtin_ctzll(bits);
	return sel;
}

/* The words of columns with no NULL: a pass for each column, over the rows sel (all rows when it is NULL) */
static bool
dense_words(const SiftTsShape *sh, const SiftParCol *col, const int64 *const *bins, const uint32 *sel, uint32 nact, uint64 *const *key, uint64 *const *sum, SiftErr *err)
{
	for (int m = 0; m < sh->nsum; m++)
	{
		const int64 *src = col[sh->sum_at[m]].v;

		for (uint32 i = 0; i < nact; i++)
			sum[m][i] = (uint64) src[sel != NULL ? sel[i] : i];
	}
	for (int j = 0; j < sh->nk; j++)
	{
		const int64 *src = bins[j] != NULL ? bins[j] : col[sh->key[j].at].v;
		uint64		bad = 0;

		for (uint32 i = 0; i < nact; i++)
		{
			const uint64 word = (uint64) src[sel != NULL ? sel[i] : i] ^ SIFT_TS_SIGN;

			key[j][i] = word;
			bad |= word == SIFT_TS_NULL_WORD;
		}
		if (bad)
			return serial(err, "a key that is the word of the NULL key");
	}
	return true;
}

static bool
sparse_words(const SiftTsShape *sh, const SiftParCol *col, const uint64 *active, int n, const int64 *const *bins, uint64 *const *key, uint64 *const *sum, SiftErr *err)
{
	uint32		i = 0;

	for (int w = 0; w < (n + 63) / 64; w++)
		for (uint64 bits = active[w]; bits != 0; bits &= bits - 1, i++)
		{
			const uint32 r = w * 64 + __builtin_ctzll(bits);

			for (int c = 0; c < sh->ncount; c++)
				if (col[sh->count_at[c]].isnull[r])
					return serial(err, "a NULL in a counted column");
			for (int m = 0; m < sh->nsum; m++)
			{
				if (col[sh->sum_at[m]].isnull[r])
					return serial(err, "a NULL in a summed column");
				sum[m][i] = (uint64) col[sh->sum_at[m]].v[r];
			}
			for (int j = 0; j < sh->nk; j++)
			{
				const SiftParCol *c = &col[sh->key[j].at];

				if (c->isnull[r])
					key[j][i] = SIFT_TS_NULL_WORD;
				else if ((key[j][i] = (uint64) (bins[j] != NULL ? bins[j][r] : c->v[r]) ^ SIFT_TS_SIGN) == SIFT_TS_NULL_WORD)
					return serial(err, "a key that is the word of the NULL key");
			}
		}
	return true;
}

bool
sift_tseries_batch(const SiftTsShape *sh, const SiftParCol *col, const uint64 *active, uint32 nact, int n, SiftArena *a, SiftErr *err, SiftParGroupBatch *out)
{
	const int64 *bins[SIFT_PAR_GROUP_MAX_KEYS] = {0};
	uint64	   *key[SIFT_PAR_GROUP_MAX_KEYS] = {0},
			   *sum[SIFT_PAR_GROUP_MAX_SUM] = {0};
	const uint32 *sel = NULL;
	bool		ok;

	for (int j = 0; j < sh->nk; j++)
	{
		key[j] = sift_arena_alloc(a, nact * sizeof(uint64), 8);
		if (key[j] == NULL)
			return arena_full(a, err);
		if (sh->key[j].binned && (bins[j] = bin_column(&sh->key[j], &col[sh->key[j].at], active, n, a, err)) == NULL)
			return false;
	}
	for (int m = 0; m < sh->nsum; m++)
	{
		sum[m] = sift_arena_alloc(a, nact * sizeof(uint64), 8);
		if (sum[m] == NULL)
			return arena_full(a, err);
	}
	if (columns_have_null(sh, col, n))
		ok = sparse_words(sh, col, active, n, bins, key, sum, err);
	else
	{
		if (nact < (uint32) n && (sel = rows_of(active, nact, n, a)) == NULL)
			return arena_full(a, err);
		ok = dense_words(sh, col, bins, sel, nact, key, sum, err);
	}
	if (!ok)
		return false;
	out->n = nact;
	for (int j = 0; j < sh->nk; j++)
		out->key[j] = key[j];
	for (int m = 0; m < sh->nsum; m++)
		out->arg[m] = sum[m];
	return true;
}
