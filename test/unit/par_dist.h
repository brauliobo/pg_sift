/*
 * par_dist.h - what par_distinct.c and bench_par_radix.c share for COUNT(DISTINCT): the columns of a scan (keys that are words and a text, sums, and a counted word or text, with NULL values), the scan
 * that feeds agg/v6/par/distinct.h with them, and the answer it must give: the rows sorted by key and value, and counted.
 */
#ifndef SIFT_PAR_DIST_H
#define SIFT_PAR_DIST_H

#include "postgres.h"

#include "agg/v6/par/distinct.h"
#include "core/hash.h"

#define DIST_COPIES		4

typedef struct DData
{
	uint32		n;
	uint64	   *key[SIFT_PAR_GROUP_MAX_KEYS];
	SiftStrRef *kstr;
	uint64	   *sum[SIFT_PAR_GROUP_MAX_SUM];
	uint64	   *val;
	uint8	   *vnull;
	SiftStrRef *vstr;
	SiftStrRef *pool[DIST_COPIES];	/* the texts of the keys, and of the values, as many times as a part has its own dictionary */
	SiftStrRef *vpool[DIST_COPIES];
	uint32		npool;
	uint32		nvpool;
} DData;

static uint64 dgen_state;

static uint64
dgen_next(void)
{
	return sift_fmix64(++dgen_state);
}

/* The text of key or value j: its digits, then bytes of 'x' (the text 0 is the empty one, one in 251 is long, one in 5 has bytes above 127) */
static SiftStrRef
dtext(uint32 j, const char *prefix)
{
	char		buf[600];
	int			len = j == 0 ? 0 : snprintf(buf, sizeof(buf), "%s%u", j % 5 == 0 ? "\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87 " : prefix, j);
	const int	pad = j == 0 ? 0 : j % 17 + (j % 251 == 250 ? 250 : 0);
	char	   *p = malloc(len + pad + 1);

	memcpy(p, buf, len);
	memset(p + len, 'x', pad);
	return (SiftStrRef) {(const uint8 *) p, len + pad};
}

static SiftStrRef *
dpool(uint32 n, const char *prefix)
{
	SiftStrRef *p = malloc(sizeof(SiftStrRef) * Max(n, 1));

	for (uint32 j = 0; j < n; j++)
		p[j] = dtext(j, prefix);
	return p;
}

static void
dpool_free(SiftStrRef *p, uint32 n)
{
	for (uint32 j = 0; j < n; j++)
		free((void *) p[j].p);
	free(p);
}

/*
 * n rows of ngroups groups at most (with a text, the word 0 of a key is of group % 3 and the text of group / 3; the other words are of group % (3 + j)), ndist values (the value 0 is the zero word, or
 * the empty text), skew: nine rows of ten in group 0 (and value 0), nulls: a value in nine is NULL. A text is not at one place: the row i reads the copy of the pool that its part has.
 */
static DData *
dgen(const SiftParDistinctSpec *sp, uint32 n, uint32 ngroups, uint32 ndist, bool skew, bool nulls)
{
	DData	   *d = calloc(1, sizeof(DData));
	const bool	grouped = sp->kw > 0 || sp->kstr;

	d->n = n;
	d->npool = Max(ngroups / 3 + 1, 1);
	d->nvpool = ndist;
	for (int c = 0; c < DIST_COPIES; c++)
	{
		d->pool[c] = dpool(d->npool, "k");
		d->vpool[c] = sp->vstr ? dpool(ndist, "v") : NULL;
	}
	for (int j = 0; j < sp->kw; j++)
		d->key[j] = malloc(sizeof(uint64) * Max(n, 1));
	for (int m = 0; m < sp->nsum; m++)
		d->sum[m] = malloc(sizeof(uint64) * Max(n, 1));
	d->kstr = sp->kstr ? malloc(sizeof(SiftStrRef) * Max(n, 1)) : NULL;
	d->val = sp->vstr ? NULL : malloc(sizeof(uint64) * Max(n, 1));
	d->vnull = sp->vstr ? NULL : calloc(Max(n, 1), 1);
	d->vstr = sp->vstr ? malloc(sizeof(SiftStrRef) * Max(n, 1)) : NULL;
	for (uint32 i = 0; i < n; i++)
	{
		const bool	hot = skew && dgen_next() % 10 < 9;
		const uint32 g = !grouped ? 0 : hot ? 0 : dgen_next() % ngroups;
		const uint32 v = hot ? 0 : dgen_next() % ndist;
		const bool	null = nulls && dgen_next() % 9 == 0;
		const int	copy = (i >> 8) % DIST_COPIES;

		for (int j = 0; j < sp->kw; j++)
			d->key[j][i] = j == 0 ? (sp->kstr ? g % 3 : g) * UINT64CONST(0x9E3779B97F4A7C15) : sift_fmix64((g % (3 + j)) * 7 + j);
		if (sp->kstr)
			d->kstr[i] = d->pool[copy][g / 3];
		for (int m = 0; m < sp->nsum; m++)
			d->sum[m][i] = (uint64) ((int64) (dgen_next() % 2001) - 1000);
		if (sp->vstr)
			d->vstr[i] = null ? (SiftStrRef) {NULL, SIFT_STR_NULL} : d->vpool[copy][v];
		else
		{
			d->val[i] = v * UINT64CONST(0x9E3779B97F4A7C15);
			d->vnull[i] = null;
		}
	}
	return d;
}

static void
dfree(DData *d)
{
	for (int j = 0; j < SIFT_PAR_GROUP_MAX_KEYS; j++)
		free(d->key[j]);
	for (int m = 0; m < SIFT_PAR_GROUP_MAX_SUM; m++)
		free(d->sum[m]);
	for (int c = 0; c < DIST_COPIES; c++)
	{
		dpool_free(d->pool[c], d->npool);
		if (d->vpool[c] != NULL)
			dpool_free(d->vpool[c], d->nvpool);
	}
	free(d->kstr);
	free(d->val);
	free(d->vnull);
	free(d->vstr);
	free(d);
}

/* ---- the scan: the rows of the items [lo, hi), with no copy ---- */

typedef struct DSrc
{
	const DData *d;
	const SiftParDistinctSpec *sp;
	bool		overflowed;
} DSrc;

static bool
dsrc_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParDistinctBatch *b, SiftErr *err)
{
	const DSrc *s = arg;

	b->n = hi - lo;
	for (int j = 0; j < s->sp->kw; j++)
		b->key[j] = s->d->key[j] + lo;
	for (int m = 0; m < s->sp->nsum; m++)
		b->sum[m] = s->d->sum[m] + lo;
	b->kstr = s->sp->kstr ? s->d->kstr + lo : NULL;
	b->val = s->sp->vstr ? NULL : s->d->val + lo;
	b->vnull = s->sp->vstr ? NULL : s->d->vnull + lo;
	b->vstr = s->sp->vstr ? s->d->vstr + lo : NULL;
	return true;
}

static void
dsrc_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
	((DSrc *) arg)->overflowed = true;
}

#endif							/* SIFT_PAR_DIST_H */
