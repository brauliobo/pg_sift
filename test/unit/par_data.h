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
	uint64	   *key[SIFT_PAR_GROUP_MAX_KW];
	uint64	   *arg[SIFT_PAR_GROUP_MAX_SUM];
	SiftStrRef *str;
	uint64	   *val;
	uint32	   *gid;
	SiftStrRef *pool;			/* the text of each group */
	uint32		npool;
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

/* Text keys: the group g is its digits and then g % 17 'x' (the group 0 is the empty string, one in 251 is long); one group in 11 is NULL */
static Data *
gen_str(uint32 n, uint32 ngroups, bool skew, int nsum, bool nulls)
{
	Data	   *d = calloc(1, sizeof(Data));

	d->n = n;
	d->pool = calloc(ngroups, sizeof(SiftStrRef));
	d->npool = ngroups;
	for (uint32 g = 0; g < ngroups; g++)
	{
		char		buf[400];
		int			len = g == 0 ? 0 : snprintf(buf, sizeof(buf), "%u", g);
		const int	pad = g % 17 + (g % 251 == 250 ? 250 : 0);

		memset(buf + len, 'x', pad);
		len += g == 0 ? 0 : pad;
		d->pool[g].p = memcpy(malloc(len + 1), buf, len);
		d->pool[g].len = len;
	}
	d->str = malloc(sizeof(SiftStrRef) * n);
	for (uint32 i = 0; i < n; i++)
	{
		const uint32 g = pick_group(ngroups, skew);

		d->str[i] = nulls && g % 11 == 3 ? (SiftStrRef) {NULL, SIFT_STR_NULL} : d->pool[g];
	}
	gen_args(d, nsum);
	return d;
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
	for (int j = 0; j < SIFT_PAR_GROUP_MAX_KW; j++)
		free(d->key[j]);
	for (int m = 0; m < SIFT_PAR_GROUP_MAX_SUM; m++)
		free(d->arg[m]);
	for (uint32 g = 0; g < d->npool; g++)
		free((void *) d->pool[g].p);
	free(d->pool);
	free(d->str);
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
	for (int j = 0; j < s->sp->kw; j++)
		b->key[j] = s->d->key[j] + lo;
	for (int m = 0; m < s->sp->nsum; m++)
		b->arg[m] = s->d->arg[m] + lo;
	b->str = s->sp->str ? s->d->str + lo : NULL;
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
