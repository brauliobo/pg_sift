/*
 * agg/stream_exec.c - the sorted GROUP BY and Top-N of agg/stream_exec.h.
 */
#include "core/host.h"

#include <math.h>

#include "agg/group/state.h"
#include "agg/stream_exec.h"

void
sift_stream_init(SiftStreamExec *se, uint8 nkeys, bool descending, bool rows_mode, uint64 offset, uint64 limit,
				 void (*emit_group) (void *, const SiftStreamGroup *), void (*emit_row) (void *, const SiftStreamRow *), void *arg)
{
	Assert(nkeys >= 1 && nkeys <= SIFT_STREAM_MAX_KEYS);
	memset(se, 0, sizeof(*se));
	se->nkeys = nkeys;
	se->descending = descending;
	se->rows_mode = rows_mode;
	se->offset_count = offset;
	se->limit_count = limit;
	se->done = limit == 0;
	se->emit_group = emit_group;
	se->emit_row = emit_row;
	se->arg = arg;
}

void
sift_stream_add_source(SiftStreamExec *se, SiftStreamNext next, void *src)
{
	se->cursors = se->cursors ? repalloc(se->cursors, sizeof(SiftPartCursor) * (se->n_parts + 1))
		: palloc(sizeof(SiftPartCursor));
	se->cursors[se->n_parts] = (SiftPartCursor) {.next = next, .src = src, .live = true};
	se->n_parts++;
}

void
sift_stream_free(SiftStreamExec *se)
{
	if (se->cursors)
		pfree(se->cursors);
	se->cursors = NULL;
	se->n_parts = 0;
}

static inline int64
vec_i64(const SiftVec *v, uint32 r)
{
	return v->type == SVEC_I32 ? ((const int32 *) v->data)[r] : ((const int64 *) v->data)[r];
}

static inline bool
vec_null(const SiftVec *v, uint32 r)
{
	return !v->all_valid && !((v->valid[r >> 6] >> (r & 63)) & 1);
}

static inline bool
row_active(const SiftStreamBatch *b, uint32 r)
{
	return b->mask == NULL || ((b->mask[r >> 6] >> (r & 63)) & 1);
}

static int
f8_cmp(double a, double b)
{
	const bool	an = a != a,
				bn = b != b;

	if (an || bn)
		return an == bn ? 0 : (an ? 1 : -1);
	return a < b ? -1 : a > b;
}

static void
batch_row(const SiftStreamBatch *b, uint8 nkeys, uint32 r, SiftStreamRow *out)
{
	out->key_nulls = 0;
	for (uint8 i = 0; i < nkeys; i++)
	{
		const bool	isnull = vec_null(b->keys[i], r);

		out->key_nulls |= (uint32) isnull << i;
		out->key[i] = isnull ? 0 : vec_i64(b->keys[i], r);
	}
	out->val_null = b->nvals == 0 || b->val[0] == NULL || b->val[0]->type == SVEC_F64 || vec_null(b->val[0], r);
	out->val = out->val_null ? 0 : vec_i64(b->val[0], r);
}

/* The order of the parts: NULLs last, reversed when descending */
static int
row_cmp(const SiftStreamExec *se, const SiftStreamRow *a, const SiftStreamRow *b)
{
	int			r = 0;

	for (uint8 i = 0; i < se->nkeys && r == 0; i++)
	{
		const bool	an = (a->key_nulls >> i) & 1,
					bn = (b->key_nulls >> i) & 1;

		if (an != bn)
			r = an ? 1 : -1;
		else if (!an && a->key[i] != b->key[i])
			r = a->key[i] < b->key[i] ? -1 : 1;
	}
	return se->descending ? -r : r;
}

static void
finish_one(SiftStreamExec *se)
{
	se->emitted++;
	if (se->limit_count != SIFT_STREAM_NO_LIMIT && se->emitted >= se->offset_count + se->limit_count)
		se->done = true;
}

static void
group_close(SiftStreamExec *se)
{
	if (!se->has_cur_group)
		return;
	se->has_cur_group = false;
	if (se->emitted >= se->offset_count)
		se->emit_group(se->arg, &se->cur_group);
	finish_one(se);
}

static bool
group_is(const SiftStreamExec *se, const SiftStreamRow *r)
{
	const SiftStreamGroup *g = &se->cur_group;

	for (uint8 i = 0; i < se->nkeys; i++)
		if (g->key[i] != r->key[i])
			return false;
	return g->key_nulls == r->key_nulls;
}

static void
group_open(SiftStreamExec *se, const SiftStreamRow *r)
{
	SiftStreamGroup *g = &se->cur_group;

	memset(g, 0, sizeof(*g));
	for (uint8 i = 0; i < se->nkeys; i++)
		g->key[i] = r->key[i];
	g->key_nulls = r->key_nulls;
	se->has_cur_group = true;
}

static void
fold_f64(SiftStreamVal *v, double x)
{
	if (v->n == 0)
		v->fsum = v->fmin = v->fmax = x;
	else
	{
		const double sum = v->fsum + x;

		if (isinf(sum) && !isinf(v->fsum) && !isinf(x))
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value out of range: overflow")));
		v->fsum = sum;
		if (f8_cmp(x, v->fmin) < 0)
			v->fmin = x;
		if (f8_cmp(x, v->fmax) > 0)
			v->fmax = x;
	}
	v->n++;
}

static void
fold_i64(SiftStreamVal *v, int64 x)
{
	if (v->n++ == 0)
		v->min = v->max = x;
	else
	{
		v->min = Min(v->min, x);
		v->max = Max(v->max, x);
	}
	sift_i128_add(&v->sum, sift_i128_of_i64(x));
}

static void
fold_slots(SiftStreamGroup *g, const SiftStreamBatch *b, uint32 r)
{
	for (uint8 s = 0; s < b->nvals; s++)
	{
		const SiftVec *v = b->val[s];

		if (b->filter[s] != NULL && !((b->filter[s][r >> 6] >> (r & 63)) & 1))
			continue;
		if (v == NULL)
			g->v[s].n++;
		else if (vec_null(v, r))
			continue;
		else if (v->type == SVEC_F64)
			fold_f64(&g->v[s], ((const double *) v->data)[r]);
		else
			fold_i64(&g->v[s], vec_i64(v, r));
	}
}

static void
feed(SiftStreamExec *se, const SiftStreamBatch *b, uint32 br, const SiftStreamRow *r)
{
	SiftStreamGroup *g = &se->cur_group;

	if (se->rows_mode)
	{
		if (se->emitted >= se->offset_count)
			se->emit_row(se->arg, r);
		finish_one(se);
		return;
	}
	if (se->has_cur_group && !group_is(se, r))
		group_close(se);
	if (se->done)
		return;
	if (!se->has_cur_group)
		group_open(se, r);
	g->rows++;
	fold_slots(g, b, br);
}

/* The first active row of the part at or after its position, refilling from its source; false when the part has no more */
static bool
cursor_head(SiftStreamExec *se, SiftPartCursor *c, SiftStreamRow *head)
{
	while (c->live)
	{
		while (c->pos < c->batch.n && !row_active(&c->batch, c->pos))
			c->pos++;
		if (c->pos < c->batch.n)
		{
			batch_row(&c->batch, se->nkeys, c->pos, head);
			return true;
		}
		c->pos = 0;
		c->batch.n = 0;
		c->live = c->next(c->src, &c->batch);
	}
	return false;
}

/* Feeds the rows of the part up to bound (inclusive; NULL: to the end of its batch) */
static void
cursor_consume(SiftStreamExec *se, SiftPartCursor *c, const SiftStreamRow *bound)
{
	for (; c->pos < c->batch.n && !se->done; c->pos++)
	{
		SiftStreamRow r;

		if (!row_active(&c->batch, c->pos))
			continue;
		batch_row(&c->batch, se->nkeys, c->pos, &r);
		if (bound && row_cmp(se, &r, bound) > 0)
			return;
		feed(se, &c->batch, c->pos, &r);
	}
}

bool
sift_stream_step(SiftStreamExec *se)
{
	SiftStreamRow head,
				best,
				second;
	SiftPartCursor *first = NULL;
	bool		have_second = false;

	if (se->done)
		return false;
	for (uint32 i = 0; i < se->n_parts; i++)
	{
		SiftPartCursor *c = &se->cursors[i];

		if (!cursor_head(se, c, &head))
			continue;
		if (first == NULL || row_cmp(se, &head, &best) < 0)
		{
			if (first)
			{
				second = best;
				have_second = true;
			}
			first = c;
			best = head;
		}
		else if (!have_second || row_cmp(se, &head, &second) < 0)
		{
			second = head;
			have_second = true;
		}
	}
	if (first == NULL)
	{
		group_close(se);
		se->done = true;
		return false;
	}
	cursor_consume(se, first, have_second ? &second : NULL);
	return true;
}

void
sift_stream_run(SiftStreamExec *se)
{
	while (sift_stream_step(se))
		;
}
