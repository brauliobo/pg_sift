/*
 * agg/par/ref.h - the answer that the operators of agg/par/ must give: the rows sorted by key, with the equal ones merged (and the K best of them), and the records that a run's groups are collected in.
 */
#ifndef SIFT_PAR_REF_H
#define SIFT_PAR_REF_H

#include "data.h"

typedef struct Rec
{
	uint64		key[SIFT_PAR_GROUP_MAX_KEYS];
	uint8	   *str[SIFT_PAR_GROUP_MAX_KEYS];
	uint32		len[SIFT_PAR_GROUP_MAX_KEYS];
	uint64		count;
	uint64		sum[SIFT_PAR_GROUP_MAX_SUM];
} Rec;

typedef struct Vec
{
	Rec		   *r;
	uint32		n;
	uint32		cap;
} Vec;

/* ---- records ---- */

static const SiftParGroupSpec *cur_spec;

static void
rec_free(Rec *r)
{
	for (int j = 0; j < SIFT_PAR_GROUP_MAX_KEYS; j++)
		free(r->str[j]);
}

static void
vec_free(Vec *v)
{
	for (uint32 i = 0; i < v->n; i++)
		rec_free(&v->r[i]);
	free(v->r);
	*v = (Vec) {0};
}

static void
vec_push(Vec *v, const SiftParGroupSpec *sp, const SiftParGroupRow *row)
{
	Rec		   *r;

	if (v->n == v->cap)
		v->r = realloc(v->r, sizeof(Rec) * (v->cap = v->cap ? v->cap * 2 : 1024));
	r = &v->r[v->n++];
	memset(r, 0, sizeof(*r));
	for (int j = 0; j < sp->nk; j++)
		if (!sift_par_group_is_str(sp, j))
			r->key[j] = row->key[j];
		else
		{
			r->len[j] = row->str[j].len;
			if (row->str[j].len != SIFT_STR_NULL)
				r->str[j] = memcpy(malloc(row->str[j].len + 1), row->str[j].p, row->str[j].len);
		}
	r->count = row->count;
	memcpy(r->sum, row->sum, sizeof(uint64) * sp->nsum);
}

/* Moves the records of src to the end of dst */
static void
vec_append(Vec *dst, Vec *src)
{
	if (src->n == 0)
		return;
	dst->r = realloc(dst->r, sizeof(Rec) * (dst->n + src->n));
	memcpy(dst->r + dst->n, src->r, sizeof(Rec) * src->n);
	dst->n += src->n;
	dst->cap = dst->n;
	free(src->r);
	*src = (Vec) {0};
}

/* Row i of the data, as the group of one row */
static void
vec_push_row(Vec *v, const SiftParGroupSpec *sp, const Data *d, uint32 i)
{
	SiftParGroupRow row = {.count = 1};

	for (int j = 0; j < sp->nk; j++)
		if (sift_par_group_is_str(sp, j))
			row.str[j] = d->str[j][i];
		else
			row.key[j] = d->key[j][i];
	for (int m = 0; m < sp->nsum; m++)
		row.sum[m] = d->arg[m][i];
	vec_push(v, sp, &row);
}

static int
rec_key_cmp(const Rec *a, const Rec *b)
{
	for (int j = 0; j < cur_spec->nk; j++)
	{
		int			c;

		if (!sift_par_group_is_str(cur_spec, j))
			c = a->key[j] == b->key[j] ? 0 : a->key[j] < b->key[j] ? -1 : 1;
		else if (a->len[j] == SIFT_STR_NULL || b->len[j] == SIFT_STR_NULL)
			c = a->len[j] == b->len[j] ? 0 : a->len[j] == SIFT_STR_NULL ? -1 : 1;
		else if ((c = memcmp(a->str[j], b->str[j], Min(a->len[j], b->len[j]))) == 0)
			c = a->len[j] == b->len[j] ? 0 : a->len[j] < b->len[j] ? -1 : 1;
		if (c != 0)
			return c;
	}
	return 0;
}

static int
cmp_key_q(const void *a, const void *b)
{
	return rec_key_cmp(a, b);
}

static int64
rec_metric(const Rec *r)
{
	if (cur_spec->order == SIFT_PAR_GROUP_BY_KEY)
		return 0;
	return (int64) (cur_spec->order == 0 ? r->count : r->sum[cur_spec->order - 1]);
}

static int
cmp_rank_q(const void *a, const void *b)
{
	const int64 ma = rec_metric(a),
				mb = rec_metric(b);

	if (ma != mb)
		return (ma < mb) == cur_spec->desc ? 1 : -1;
	return rec_key_cmp(a, b);
}

/* Sorted by key, the equal keys merged: what the aggregation of the rows is */
static void
vec_normalize(Vec *v, const SiftParGroupSpec *sp)
{
	uint32		o = 0;

	cur_spec = sp;
	qsort(v->r, v->n, sizeof(Rec), cmp_key_q);
	for (uint32 i = 0; i < v->n; i++)
	{
		if (o > 0 && rec_key_cmp(&v->r[o - 1], &v->r[i]) == 0)
		{
			v->r[o - 1].count += v->r[i].count;
			for (int m = 0; m < sp->nsum; m++)
				v->r[o - 1].sum[m] += v->r[i].sum[m];
			rec_free(&v->r[i]);
		}
		else
			v->r[o++] = v->r[i];
	}
	v->n = o;
}

/* The K best of the groups, back in the order of the keys */
static void
vec_best(Vec *v, const SiftParGroupSpec *sp)
{
	cur_spec = sp;
	if (sp->topk == 0 || v->n <= sp->topk)
		return;
	qsort(v->r, v->n, sizeof(Rec), cmp_rank_q);
	for (uint32 i = sp->topk; i < v->n; i++)
		rec_free(&v->r[i]);
	v->n = sp->topk;
	qsort(v->r, v->n, sizeof(Rec), cmp_key_q);
}

static Vec
reference(const Data *d, const SiftParGroupSpec *sp)
{
	Vec			v = {0};

	for (uint32 i = 0; i < d->n; i++)
		vec_push_row(&v, sp, d, i);
	vec_normalize(&v, sp);
	vec_best(&v, sp);
	return v;
}

static bool
vec_same(const Vec *a, const Vec *b, const SiftParGroupSpec *sp)
{
	cur_spec = sp;
	if (a->n != b->n)
		return false;
	for (uint32 i = 0; i < a->n; i++)
		if (rec_key_cmp(&a->r[i], &b->r[i]) != 0 || a->r[i].count != b->r[i].count ||
			memcmp(a->r[i].sum, b->r[i].sum, sizeof(uint64) * sp->nsum) != 0)
			return false;
	return true;
}

#endif							/* SIFT_PAR_REF_H */
