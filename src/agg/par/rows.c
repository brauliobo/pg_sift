/*
 * agg/par/rows.c - the parallel Top-N of rows of agg/par/rows.h. Nothing here calls the server: the memory of a thread is its arenas, and what does not fit is an error code.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"

#include "agg/par/rows.h"

#define STR_BYTES_MAX	(64 << 10)

typedef struct RowEnt
{
	SiftParRowsRow row;
	uint8	   *buf[SIFT_PAR_ROWS_MAX_KEYS];	/* the bytes of its text keys, scap of them */
	uint32		scap[SIFT_PAR_ROWS_MAX_KEYS];
} RowEnt;

struct SiftParRows
{
	SiftParRowsSpec sp;
	SiftParRowsHooks hooks;
	SiftParArenas *ar;
	int			nw;
	SiftAtomicBound bound;		/* of the first sort column, when it is an integer */
	SiftArena	merge;
	SiftParTopK *tk;
};

static int
cmp_str(const SiftStrRef *a, const SiftStrRef *b)
{
	const uint32 n = Min(a->len, b->len);
	const int	c = n > 0 ? memcmp(a->p, b->p, n) : 0;

	return c != 0 ? c : a->len == b->len ? 0 : a->len < b->len ? -1 : 1;
}

/* Whether a ranks before b: the sort columns in turn, then the address */
static bool
row_better(const void *va, const void *vb, const void *ctx)
{
	const SiftParRowsSpec *sp = ctx;
	const SiftParRowsRow *a = &((const RowEnt *) va)->row,
			   *b = &((const RowEnt *) vb)->row;

	for (int j = 0; j < sp->nkeys; j++)
	{
		int			c;

		if (a->isnull[j] != b->isnull[j])
			return a->isnull[j] == sp->nulls_first[j];
		if (a->isnull[j])
			continue;
		c = sp->str[j] ? cmp_str(&a->str[j], &b->str[j]) : a->key[j] == b->key[j] ? 0 : a->key[j] < b->key[j] ? -1 : 1;
		if (c != 0)
			return sp->desc[j] ? c > 0 : c < 0;
	}
	return a->tid < b->tid;
}

/* The row into entry dst, whose bytes of the text keys are copied here, once the row is among the K best */
static bool
row_store(void *dst, const void *cand, SiftArena *str, const void *ctx)
{
	const SiftParRowsSpec *sp = ctx;
	const SiftParRowsRow *r = &((const RowEnt *) cand)->row;
	RowEnt	   *e = dst;
	uint8	   *buf[SIFT_PAR_ROWS_MAX_KEYS];

	memcpy(buf, e->buf, sizeof(buf));
	for (int j = 0; j < sp->nkeys; j++)
	{
		if (!sp->str[j] || r->isnull[j] || r->str[j].len <= e->scap[j])
			continue;
		e->scap[j] = Max(64, pg_nextpower2_32(r->str[j].len));
		if ((e->buf[j] = sift_arena_alloc(str, e->scap[j], 8)) == NULL)
		{
			memcpy(e->buf, buf, sizeof(buf));
			return false;
		}
	}
	e->row = *r;
	for (int j = 0; j < sp->nkeys; j++)
		if (sp->str[j] && !r->isnull[j])
		{
			if (r->str[j].len > 0)
				memcpy(e->buf[j], r->str[j].p, r->str[j].len);
			e->row.str[j].p = e->buf[j];
		}
	return true;
}

static bool
fail(SiftErr *err, SiftErrCode code, const char *msg)
{
	sift_err_fill(err, code, 0, 0, msg);
	return false;
}

/* a row whose first sort column is worse than the K-th best of some thread cannot be among the K best of all */
static inline bool
beaten(const SiftParRowsSpec *sp, uint64 bound, const SiftParRowsBatch *b, uint32 i)
{
	if (sp->str[0] || bound == 0)
		return false;
	if (b->isnull[0] != NULL && b->isnull[0][i])
		return !sp->nulls_first[0];
	return sift_bound_key_i64(b->key[0][i], sp->desc[0]) < bound;
}

static SiftParRowsRow
row_at(const SiftParRowsSpec *sp, const SiftParRowsBatch *b, uint32 i)
{
	SiftParRowsRow r = {.tid = b->tid[i]};

	for (int j = 0; j < sp->nkeys; j++)
		if (sp->str[j])
		{
			r.str[j] = b->str[j][i];
			r.isnull[j] = r.str[j].len == SIFT_STR_NULL;
		}
		else
		{
			r.key[j] = b->key[j][i];
			r.isnull[j] = b->isnull[j] != NULL && b->isnull[j][i];
		}
	return r;
}

static bool
rows_phase1(void *vr, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	SiftParRows *r = vr;
	SiftParTopK *tk = &r->tk[t];
	const SiftParRowsSpec *sp = &r->sp;
	SiftParRowsBatch b = {0};
	uint64		bound;

	*done = 0;
	if (!r->hooks.fill(r->hooks.arg, t, lo, hi, scratch, &b, err))
		return false;
	bound = sift_bound_load(&r->bound);
	for (uint32 i = 0; i < b.n; i++)
	{
		RowEnt		cand = {.row = row_at(sp, &b, i)};

		if (beaten(sp, bound, &b, i))
			continue;
		if (!sift_par_topk_offer(tk, &cand))
			return fail(err, SIFT_ERR_WORK_MEM, "parallel Top-N: the arena of the K best has no room for the text keys");
	}
	if (!sp->str[0] && sift_par_topk_worst(tk) != NULL && !((const RowEnt *) sift_par_topk_worst(tk))->row.isnull[0])
		sift_bound_raise(&r->bound, sift_bound_key_i64(((const RowEnt *) sift_par_topk_worst(tk))->row.key[0], sp->desc[0]));
	return true;
}

static void
rows_finish(void *vr)
{
	SiftParRows *r = vr;
	SiftParTopK *heap[SIFT_PAR_MAX_THREADS] = {0};
	const void **sorted;
	uint32		n;
	bool		merged;

	for (int t = 0; t < r->nw; t++)
		heap[t] = &r->tk[t];
	merged = sift_par_topk_merge(heap, r->nw, &r->merge, &sorted, &n);
	SIFT_INVARIANT(merged, "parallel Top-N: the arena of the merge was made too small");
	for (uint32 i = 0; i < n; i++)
		r->hooks.emit(r->hooks.arg, &((const RowEnt *) sorted[i])->row);
}

const SiftParOps sift_par_rows_ops = {.phase1 = rows_phase1,.finish = rows_finish};

bool
sift_par_rows_excludes(const SiftParRows *r, int64 min, int64 max, bool has_null)
{
	const SiftParRowsSpec *sp = &r->sp;
	const uint64 bound = sift_bound_load(&r->bound);

	if (sp->str[0] || bound == 0 || (has_null && sp->nulls_first[0]))
		return false;
	return sift_bound_key_i64(sp->desc[0] ? max : min, sp->desc[0]) < bound;
}

SiftParRows *
sift_par_rows_create(SiftParArenas *ar, const SiftParRowsSpec *spec, const SiftParRowsHooks *hooks, SiftErr *err)
{
	SiftArena  *boot = &ar->w[0].state;
	SiftParRows *r;
	size_t		merge_bytes;
	uint8	   *merge;

	if (spec->nkeys < 1 || spec->nkeys > SIFT_PAR_ROWS_MAX_KEYS || spec->k < 1 || spec->k > SIFT_PAR_ROWS_K_MAX)
		return fail(err, SIFT_ERR_INVALID_PARAM, "parallel Top-N: the sort columns or K"), NULL;
	merge_bytes = 2 * (size_t) ar->n * spec->k * sizeof(void *);
	r = sift_arena_alloc0(boot, sizeof(SiftParRows), SIFT_ARENA_ALIGN);
	if (r != NULL)
		r->tk = sift_arena_alloc0(boot, sizeof(SiftParTopK) * ar->n, SIFT_ARENA_ALIGN);
	merge = r != NULL && r->tk != NULL ? sift_arena_alloc(boot, merge_bytes, SIFT_ARENA_ALIGN) : NULL;
	if (merge == NULL)
		return fail(err, SIFT_ERR_WORK_MEM, "parallel Top-N: no room for the state"), NULL;
	r->sp = *spec;
	r->hooks = *hooks;
	r->ar = ar;
	r->nw = ar->n;
	sift_arena_init(&r->merge, merge, merge_bytes);
	sift_bound_init(&r->bound, 0);
	for (int t = 0; t < r->nw; t++)
	{
		SiftArena  *work = &ar->w[t].work;
		bool		text = false;

		for (int j = 0; j < spec->nkeys; j++)
			text |= spec->str[j];
		if (!sift_par_topk_init(&r->tk[t], work, spec->k, sizeof(RowEnt), text ? Min(STR_BYTES_MAX * (size_t) spec->k, work->cap / 2) : 0, row_better, row_store, &r->sp))
			return fail(err, SIFT_ERR_WORK_MEM, "parallel Top-N: an arena is too small for its K best"), NULL;
	}
	return r;
}
