/*
 * par_data.h - what par_radix.c and bench_par_radix.c share: the generators of the columns of a scan, and the scan that feeds the operators of agg/v6/par/ with them.
 */
#ifndef SIFT_PAR_DATA_H
#define SIFT_PAR_DATA_H

#include "postgres.h"

#include "agg/v6/par/distinct.h"
#include "agg/v6/par/group.h"
#include "core/hash.h"

typedef struct Data
{
	uint32		n;
	uint64	   *key[SIFT_PAR_GROUP_MAX_KEYS];	/* the words of the key columns that are words */
	SiftStrRef *str[SIFT_PAR_GROUP_MAX_KEYS];	/* the texts of the ones that are texts */
	uint64	   *arg[SIFT_PAR_GROUP_MAX_SUM];
	uint64	   *val;
	uint32	   *gid;
	SiftStrRef *pool[SIFT_PAR_GROUP_MAX_KEYS];	/* the distinct texts of each text column */
	uint32		npool[SIFT_PAR_GROUP_MAX_KEYS];
} Data;

static uint64 gen_state;

static uint64
gen_next(void)
{
	return sift_fmix64(++gen_state);
}

static uint32
pick_group(uint32 ngroups, bool skew)
{
	return skew && gen_next() % 10 < 9 ? 0 : gen_next() % ngroups;
}

static void
gen_args(Data *d, int nsum)
{
	for (int m = 0; m < nsum; m++)
	{
		d->arg[m] = malloc(sizeof(uint64) * d->n);
		for (uint32 i = 0; i < d->n; i++)
			d->arg[m][i] = (uint64) ((int64) (gen_next() % 2001) - 1000);
	}
}

/* Rows of the groups 0 .. ngroups-1 (a skewed column: nine rows of ten in group 0). With kw >= 2 a group in 13 is the NULL one: the first word 0, and the last one the flag 1 */
static Data *
gen_int(int kw, uint32 n, uint32 ngroups, bool skew, int nsum, bool key1_u32)
{
	Data	   *d = calloc(1, sizeof(Data));

	d->n = n;
	for (int j = 0; j < kw; j++)
		d->key[j] = malloc(sizeof(uint64) * n);
	for (uint32 i = 0; i < n; i++)
	{
		const uint64 g = pick_group(ngroups, skew);
		const bool	null = kw >= 2 && g % 13 == 0;

		for (int j = 0; j < kw; j++)
		{
			uint64		w = j == 0 ? g * UINT64CONST(0x9E3779B97F4A7C15) : sift_fmix64(g * 7 + j);

			if (j == kw - 1 && (key1_u32 || null) && j > 0)
				w &= 0x7fffffff;
			if (null)
				w = j == kw - 1 && j > 0 ? 1 : 0;
			d->key[j][i] = w;
		}
	}
	gen_args(d, nsum);
	return d;
}

/* n texts: the text u is its digits and then u % 17 'x' (the text 0 is the empty string, one in 251 is long) */
static SiftStrRef *
gen_pool(uint32 n)
{
	SiftStrRef *pool = calloc(n, sizeof(SiftStrRef));

	for (uint32 u = 0; u < n; u++)
	{
		char		buf[400];
		int			len = u == 0 ? 0 : snprintf(buf, sizeof(buf), "%u", u);
		const int	pad = u % 17 + (u % 251 == 250 ? 250 : 0);

		memset(buf + len, 'x', pad);
		len += u == 0 ? 0 : pad;
		pool[u].p = memcpy(malloc(len + 1), buf, len);
		pool[u].len = len;
	}
	return pool;
}

/*
 * Rows of the groups 0 .. ngroups-1 (a skewed column: nine rows of ten in group 0) by nk key columns, the ones of the bits of strs texts and the others words. The last column tells the groups apart; each
 * of the others takes a few values (13, 20, 27 ..), so that many groups share a word or a text and differ in another column. With nulls, one text in 11 is NULL.
 */
static Data *
gen_mixed(int nk, int strs, uint32 n, uint32 ngroups, bool skew, int nsum, bool nulls)
{
	Data	   *d = calloc(1, sizeof(Data));
	uint32		values[SIFT_PAR_GROUP_MAX_KEYS];

	d->n = n;
	for (int j = 0; j < nk; j++)
	{
		values[j] = j == nk - 1 ? ngroups : Min(ngroups, 13 + 7 * j);
		if ((strs >> j) & 1)
		{
			d->pool[j] = gen_pool(values[j]);
			d->npool[j] = values[j];
			d->str[j] = malloc(sizeof(SiftStrRef) * n);
		}
		else
			d->key[j] = malloc(sizeof(uint64) * n);
	}
	for (uint32 i = 0; i < n; i++)
	{
		const uint32 g = pick_group(ngroups, skew);

		for (int j = 0; j < nk; j++)
		{
			const uint32 u = g % values[j];

			if (!((strs >> j) & 1))
				d->key[j][i] = sift_fmix64((uint64) u * 7 + j);
			else
				d->str[j][i] = nulls && u % 11 == 3 ? (SiftStrRef) {NULL, SIFT_STR_NULL} : d->pool[j][u];
		}
	}
	gen_args(d, nsum);
	return d;
}

static Data *
gen_str(uint32 n, uint32 ngroups, bool skew, int nsum, bool nulls)
{
	return gen_mixed(1, 1, n, ngroups, skew, nsum, nulls);
}

/* The values of a COUNT(DISTINCT): ndist of them (one is 0 when zero), the group of each below ngroups (0: none) */
static Data *
gen_dist(uint32 n, uint32 ndist, uint32 ngroups, bool zero)
{
	Data	   *d = calloc(1, sizeof(Data));

	d->n = n;
	d->val = malloc(sizeof(uint64) * n);
	d->gid = ngroups > 0 ? malloc(sizeof(uint32) * n) : NULL;
	for (uint32 i = 0; i < n; i++)
	{
		const uint64 v = gen_next() % ndist;

		d->val[i] = v == 0 && !zero ? ndist : v * UINT64CONST(0x9E3779B97F4A7C15);
		if (ngroups > 0)
			d->gid[i] = gen_next() % ngroups;
	}
	return d;
}

static void
data_free(Data *d)
{
	for (int j = 0; j < SIFT_PAR_GROUP_MAX_KEYS; j++)
	{
		for (uint32 u = 0; u < d->npool[j]; u++)
			free((void *) d->pool[j][u].p);
		free(d->key[j]);
		free(d->pool[j]);
		free(d->str[j]);
	}
	for (int m = 0; m < SIFT_PAR_GROUP_MAX_SUM; m++)
		free(d->arg[m]);
	free(d->val);
	free(d->gid);
	free(d);
}

/* ---- the scan: the rows of the items [lo, hi) of the data, with no copy ---- */

typedef struct Src
{
	const Data *d;
	const SiftParGroupSpec *sp;
} Src;

static bool
src_group_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *b, SiftErr *err)
{
	const Src  *s = arg;

	b->n = hi - lo;
	for (int j = 0; j < s->sp->nk; j++)
		if (sift_par_group_is_str(s->sp, j))
			b->str[j] = s->d->str[j] + lo;
		else
			b->key[j] = s->d->key[j] + lo;
	for (int m = 0; m < s->sp->nsum; m++)
		b->arg[m] = s->d->arg[m] + lo;
	return true;
}

static bool
src_distinct_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParDistinctBatch *b, SiftErr *err)
{
	const Src  *s = arg;

	b->n = hi - lo;
	b->val = s->d->val + lo;
	b->gid = s->d->gid != NULL ? s->d->gid + lo : NULL;
	return true;
}

#endif							/* SIFT_PAR_DATA_H */
