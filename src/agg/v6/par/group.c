/*
 * agg/v6/par/group.c - the parallel grouped aggregation of agg/v6/par/group.h. Nothing here calls the server: the memory of a thread is its arenas, and what does not fit is an error code.
 */
#include "postgres.h"

#include <stdatomic.h>

#include "port/pg_bitutils.h"

#include "agg/v6/par/group_key.h"
#include "core/par/topk.h"
#include "core/popcount.h"

#define NO_ROOM "parallel aggregation: the arena of a thread has no room for a batch"
#define TAB_FIRST			256
#define TAB_PART_MAX		16384

typedef struct SiftTopEnt
{
	SiftParGroupRow row;
	uint8	   *buf[SIFT_PAR_GROUP_MAX_KEYS];	/* the bytes of its texts, scap of them */
	uint32		scap[SIFT_PAR_GROUP_MAX_KEYS];
} SiftTopEnt;

typedef struct GroupWorker
{
	SiftCompact local;
	bool		radix;			/* its rows go to the partitions */
	SiftRadixOut raw;			/* the rows */
	SiftRadixOut pa;			/* partial aggregates: the groups a table gave up */
	SiftRadixOut fin;			/* no K: the groups it delivered, as partial aggregates, for the leader to hand to the consumer */
	SiftParTopK tk;
} GroupWorker;

struct SiftParGroup
{
	SiftParGroupSpec sp;
	SiftParGroupHooks hooks;
	SiftParArenas *ar;
	int			nw;
	SiftCompact proto;			/* the layout of every table of the run */
	atomic_bool radix;
	atomic_bool done[SIFT_RADIX_PARTS];
	SiftArena	merge;			/* the K best of every thread, in order */
	GroupWorker *w;
};

static bool
fail_mem(SiftErr *err, const char *msg)
{
	sift_err_fill(err, SIFT_ERR_WORK_MEM, 0, 0, msg);
	return false;
}

static bool
fail_bug(SiftErr *err, const char *msg)
{
	sift_err_fill(err, SIFT_ERR_INVALID_PARAM, 0, 0, msg);
	return false;
}

typedef struct Walk
{
	SiftParGroup *g;
	int			t;
	SiftCompact *from;			/* the table that is walked */
	SiftCompact *to;			/* merge: where the groups go */
	uint64	   *bytes;			/* spill: the bytes of the partial aggregates of the groups */
} Walk;

typedef bool (*WalkFn) (const Walk *wk, const SiftParGroupRow *r);

static bool
table_each(const Walk *wk, WalkFn fn)
{
	const SiftCompact *c = wk->from;
	SiftParGroupRow r;

	for (uint64 i = 0; i < c->cap; i++)
	{
		const uint64 *s = c->tab + i * c->sw;

		if (cmp_count(c, s) == 0)
			continue;
		pgk_slot_row(c, &wk->g->sp, s, &r);
		if (!fn(wk, &r))
			return false;
	}
	if (c->strm && cmp_count(c, c->null_group) != 0)
	{
		pgk_slot_row(c, &wk->g->sp, c->null_group, &r);
		return fn(wk, &r);
	}
	return true;
}

static bool
spill_row(const Walk *wk, const SiftParGroupRow *r)
{
	return pgk_put(wk->from, &wk->g->sp, &wk->g->w[wk->t].pa, true, r);
}

static bool
size_row(const Walk *wk, const SiftParGroupRow *r)
{
	*wk->bytes += pgk_rec_size(&wk->g->sp, true, r);
	return true;
}

/* The groups of the table of a thread as partial aggregates in the partitions, and the thread goes on in the second regime: all of them or, with no room, none (false, nothing written) */
static bool
spill_local(SiftParGroup *g, int t, SiftErr *err)
{
	GroupWorker *w = &g->w[t];
	uint64		bytes = 0;
	const Walk	wk = {.g = g,.t = t,.from = &w->local,.bytes = &bytes};

	table_each(&wk, size_row);
	if (!sift_radix_fits(&w->pa, bytes))
		return fail_mem(err, NO_ROOM);
	if (!table_each(&wk, spill_row))
		return fail_bug(err, "parallel aggregation: a spill that fits did not fit");
	memset(w->local.tab, 0, w->local.cap * w->local.sw * sizeof(uint64));
	memset(w->local.null_group, 0, sizeof(w->local.null_group));
	w->local.count = 0;
	w->radix = true;
	atomic_store_explicit(&g->radix, true, memory_order_relaxed);
	return true;
}

/* Whether the arenas of the thread hold the batch whole: no row of it is folded before the others are sure to be */
static bool
batch_room(SiftParGroup *g, int t, const SiftParGroupBatch *b, SiftErr *err)
{
	GroupWorker *w = &g->w[t];
	const SiftParGroupSpec *sp = &g->sp;
	Size		strbytes;
	const uint64 bytes = pgk_batch_bytes(sp, b, &strbytes);

	if (!w->radix && w->local.count > SIFT_PAR_GROUP_LOW && !spill_local(g, t, err))
		return false;
	if (w->radix)
		return sift_radix_fits(&w->raw, bytes) || fail_mem(err, NO_ROOM);
	if (!cmp_reserve(&w->local, b->n, UINT64_MAX))
		return fail_mem(err, NO_ROOM);
	if (w->local.strm)
		return sstr_put(&w->local, NULL, strbytes) || fail_mem(err, NO_ROOM);
	return !sp->strs || cmp_intern_reserve(&w->local, b->n * sift_pc64(sp->strs), strbytes) || fail_mem(err, NO_ROOM);
}

/* The rows of integer keys of a batch to the partitions: the record of a row is its key words and arguments, in the partition of the hash of its key */
static bool
scatter_words(SiftParGroup *g, GroupWorker *w, const SiftParGroupBatch *b)
{
	const SiftParGroupSpec *sp = &g->sp;
	const uint32 words = pgk_word_rec_size(sp, false) / 8;

	for (uint32 i = 0; i < b->n; i++)
	{
		uint64		k[SIFT_PAR_GROUP_MAX_KEYS];
		uint64	   *rec;
		uint32		n = 0;

		for (int j = 0; j < sp->nk; j++)
			k[j] = b->key[j][i];
		rec = sift_radix_reserve(&w->raw, sift_radix_part(cmp_key_hash(&w->local, k)), 8 * words);
		if (rec == NULL)
			return false;
		for (int j = 0; j < sp->nk; j++)
			rec[n++] = k[j];
		for (int m = 0; m < sp->nsum; m++)
			rec[n++] = b->arg[m][i];
		for (; n < words; n++)
			rec[n] = 0;
	}
	return true;
}

static void
load_row(const SiftParGroupSpec *sp, const SiftParGroupBatch *b, uint32 i, SiftParGroupRow *r)
{
	for (int j = 0; j < sp->nk; j++)
		if (sift_par_group_is_str(sp, j))
			r->str[j] = b->str[j][i];
		else
			r->key[j] = b->key[j][i];
	for (int m = 0; m < sp->nsum; m++)
		r->sum[m] = b->arg[m][i];
}

static bool
fold_batch(SiftParGroup *g, int t, const SiftParGroupBatch *b)
{
	GroupWorker *w = &g->w[t];
	const SiftParGroupSpec *sp = &g->sp;
	SiftParGroupRow r = {.count = 1};

	if (w->radix && !sp->strs)
		return scatter_words(g, w, b);
	for (uint32 i = 0; i < b->n; i++)
	{
		load_row(sp, b, i, &r);
		if (w->radix ? !pgk_put(&w->local, sp, &w->raw, false, &r) : !pgk_add(&w->local, sp, &r, NULL))
			return false;
	}
	return true;
}

static bool
group_phase1(void *vg, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	SiftParGroup *g = vg;
	SiftParGroupBatch b = {0};

	*done = 0;
	if (!g->hooks.fill(g->hooks.arg, t, lo, hi, scratch, &b, err))
		return false;
	if (b.n == 0)
		return true;
	return batch_room(g, t, &b, err) && (fold_batch(g, t, &b) || fail_bug(err, "parallel aggregation: a batch that fits did not fit"));
}

static inline int64
metric_of(const SiftParGroupSpec *sp, const SiftParGroupRow *r)
{
	return sp->order == SIFT_PAR_GROUP_BY_KEY ? 0 : (int64) (sp->order == 0 ? r->count : r->sum[sp->order - 1]);
}

/* whether a ranks before b: the metric, then the key (a total order, so the K best do not depend on the threads) */
static bool
ent_better(const void *a, const void *b, const void *ctx)
{
	const SiftParGroupSpec *sp = ctx;
	const SiftParGroupRow *x = &((const SiftTopEnt *) a)->row,
			   *y = &((const SiftTopEnt *) b)->row;
	const int64 mx = metric_of(sp, x),
				my = metric_of(sp, y);

	if (mx != my)
		return sp->desc ? mx > my : mx < my;
	return pgk_cmp(sp, x, y) < 0;
}

/* the group into entry dst, whose bytes of its texts are copied here, once the group is among the K best */
static bool
ent_store(void *dst, const void *cand, SiftArena *str, const void *ctx)
{
	const SiftParGroupSpec *sp = ctx;
	const SiftParGroupRow *r = &((const SiftTopEnt *) cand)->row;
	SiftTopEnt *e = dst;

	for (int j = 0; j < sp->nk; j++)
	{
		const uint32 len = r->str[j].len;

		if (sift_par_group_is_str(sp, j) && len != SIFT_STR_NULL && (e->buf[j] == NULL || len > e->scap[j]))
		{
			const uint32 cap = Max(64, pg_nextpower2_32(len));
			uint8	   *buf = sift_arena_alloc(str, cap, 8);

			if (buf == NULL)
				return false;
			e->buf[j] = buf;
			e->scap[j] = cap;
		}
	}
	e->row = *r;
	for (int j = 0; j < sp->nk; j++)
		if (sift_par_group_is_str(sp, j) && r->str[j].len != SIFT_STR_NULL)
		{
			if (r->str[j].len > 0)
				memcpy(e->buf[j], r->str[j].p, r->str[j].len);
			e->row.str[j].p = e->buf[j];
		}
	return true;
}

static bool
deliver_row(const Walk *wk, const SiftParGroupRow *r)
{
	SiftParGroup *g = wk->g;

	if (g->sp.topk == 0)
		return pgk_put(wk->from, &g->sp, &g->w[wk->t].fin, true, r);
	return sift_par_topk_offer(&g->w[wk->t].tk, &(SiftTopEnt) {.row = *r});
}

static bool
merge_row(const Walk *wk, const SiftParGroupRow *r)
{
	return pgk_add(wk->to, &wk->g->sp, r, NULL);
}

/* The groups of a table to the consumer: offered to the K best or, with no K, all of them to the thread's stream for the leader, or none when the stream has no room (nothing is written) */
static bool
deliver_table(const Walk *wk)
{
	uint64		bytes = 0;
	const Walk	sz = {.g = wk->g,.t = wk->t,.from = wk->from,.bytes = &bytes};

	if (wk->g->sp.topk == 0)
	{
		table_each(&sz, size_row);
		if (!sift_radix_fits(&wk->g->w[wk->t].fin, bytes))
			return false;
	}
	return table_each(wk, deliver_row);
}

/* the table of a partition or a merge, in the work arena of thread t, for about `groups` groups */
static bool
tab_open(SiftParGroup *g, SiftCompact *tab, SiftArena *work, uint64 groups)
{
	*tab = g->proto;
	tab->arena = work;
	return cmp_init(tab, pg_nextpower2_64(Max(TAB_FIRST, Min(groups, TAB_PART_MAX) * 2)));
}

static bool
group_seal(void *vg, uint32 *nparts, SiftErr *err)
{
	SiftParGroup *g = vg;
	SiftArena  *work = &g->ar->w[0].work;
	const SiftArenaMark mark = sift_arena_mark(work);
	SiftCompact m;
	Walk		wk = {.g = g,.to = &m};
	uint64		groups = 0;
	bool		ok;

	*nparts = 0;
	if (atomic_load_explicit(&g->radix, memory_order_relaxed))
	{
		for (int t = 0; t < g->nw; t++)
			if (!g->w[t].radix && !spill_local(g, t, err))
				return false;
		*nparts = SIFT_RADIX_PARTS;
		return true;
	}
	for (int t = 0; t < g->nw; t++)
		groups = Max(groups, g->w[t].local.count);
	ok = tab_open(g, &m, work, groups);
	for (int t = 0; ok && t < g->nw; t++)
	{
		wk.from = &g->w[t].local;
		ok = table_each(&wk, merge_row);
	}
	wk.from = &m;
	ok = ok && deliver_table(&wk);
	sift_arena_rewind(work, mark);
	return ok || fail_mem(err, "parallel aggregation: the arena of the merge is full");
}

typedef struct Fold
{
	const SiftParGroupSpec *sp;
	SiftCompact *tab;
	bool		partial;
} Fold;

/* the records of words only of a chunk: the table has room for all of them at once, and each is found and folded where it is */
static bool
fold_words(const Fold *f, const uint8 *b, uint32 len)
{
	SiftCompact *c = f->tab;
	const uint32 rb = pgk_word_rec_size(f->sp, f->partial);

	if (!cmp_reserve(c, len / rb, UINT64_MAX))
		return false;
	for (uint32 off = 0; off < len; off += rb)
	{
		const uint64 *w = (const uint64 *) (b + off);

		cmp_fold(c, cmp_find(c, cmp_key_hash(c, w), w), f->partial ? w[f->sp->nk] : 1, w + f->sp->nk + f->partial);
	}
	return true;
}

/* the records of a chunk, each as its table takes it */
static bool
fold_chunk(void *arg, const uint8 *b, uint32 len)
{
	const Fold *f = arg;

	if (!f->sp->strs)
		return fold_words(f, b, len);
	for (uint32 off = 0; off < len;)
	{
		SiftParGroupRow r;
		uint32		h32[SIFT_PAR_GROUP_MAX_KEYS];

		off += pgk_parse(f->sp, f->partial, b + off, &r, h32);
		if (!pgk_add(f->tab, f->sp, &r, h32))
			return false;
	}
	return true;
}

static bool
fold_partition(SiftParGroup *g, uint32 part, SiftCompact *tab, SiftArena *work)
{
	Fold		f = {.sp = &g->sp,.tab = tab};
	uint64		bound = 0;

	for (int v = 0; v < g->nw; v++)
		bound += sift_radix_nrec(&g->w[v].raw, part) + sift_radix_nrec(&g->w[v].pa, part);
	if (!tab_open(g, tab, work, bound))
		return false;
	for (int v = 0; v < g->nw; v++)
	{
		f.partial = false;
		if (!sift_radix_scan(&g->w[v].raw, part, fold_chunk, &f))
			return false;
		f.partial = true;
		if (!sift_radix_scan(&g->w[v].pa, part, fold_chunk, &f))
			return false;
	}
	return true;
}

static bool
group_phase2(void *vg, int t, uint32 part, SiftArena *work, SiftErr *err)
{
	SiftParGroup *g = vg;
	SiftCompact tab;
	Walk		wk = {.g = g,.t = t,.from = &tab};

	if (!fold_partition(g, part, &tab, work) || !deliver_table(&wk))
		return fail_mem(err, "parallel aggregation: the arena of a partition is full");
	atomic_store_explicit(&g->done[part], true, memory_order_release);
	return true;
}

static void flush_fin(const SiftParGroup *g);

static void
group_finish(void *vg)
{
	SiftParGroup *g = vg;
	SiftParTopK *heap[SIFT_PAR_MAX_THREADS] = {0};
	const void **sorted;
	uint32		n;
	bool		merged;

	if (g->sp.topk == 0)
	{
		flush_fin(g);
		return;
	}
	for (int t = 0; t < g->nw; t++)
		heap[t] = &g->w[t].tk;
	sift_arena_reset(&g->merge);
	merged = sift_par_topk_merge(heap, g->nw, &g->merge, &sorted, &n);
	SIFT_INVARIANT(merged, "parallel aggregation: the arena of the K best was made too small");
	for (uint32 i = 0; i < n; i++)
		g->hooks.emit(g->hooks.arg, 0, &((const SiftTopEnt *) sorted[i])->row);
}

static bool
drain_row(const Walk *wk, const SiftParGroupRow *r)
{
	wk->g->hooks.emit(wk->g->hooks.arg, wk->t, r);
	return true;
}

typedef struct Drain
{
	const SiftParGroup *g;
	int			worker;
	bool		partial;
} Drain;

static bool
drain_chunk(void *arg, const uint8 *b, uint32 len)
{
	const Drain *d = arg;

	for (uint32 off = 0; off < len;)
	{
		SiftParGroupRow row = {0};
		uint32		h32[SIFT_PAR_GROUP_MAX_KEYS];

		off += pgk_parse(&d->g->sp, d->partial, b + off, &row, h32);
		d->g->hooks.emit(d->g->hooks.arg, d->worker, &row);
	}
	return true;
}

/* The groups that the threads delivered, which no K held back, to the consumer */
static void
flush_fin(const SiftParGroup *g)
{
	for (int t = 0; t < g->nw; t++)
	{
		Drain		d = {.g = g,.worker = t,.partial = true};

		for (uint32 p = 0; p < SIFT_RADIX_PARTS; p++)
			sift_radix_scan(&g->w[t].fin, p, drain_chunk, &d);
	}
}

static void
group_overflow(void *vg, const SiftParRange *left, uint32 nleft)
{
	SiftParGroup *g = vg;

	if (g->sp.topk == 0)
		flush_fin(g);
	for (int v = 0; v < g->nw; v++)
	{
		const Walk	wk = {.g = g,.t = v,.from = &g->w[v].local};

		if (g->w[v].local.tab != NULL)
			table_each(&wk, drain_row);
	}
	for (uint32 p = 0; p < SIFT_RADIX_PARTS; p++)
	{
		if (g->sp.topk == 0 && atomic_load_explicit(&g->done[p], memory_order_acquire))
			continue;
		for (int v = 0; v < g->nw; v++)
		{
			Drain		d = {.g = g,.worker = v};

			sift_radix_scan(&g->w[v].raw, p, drain_chunk, &d);
			d.partial = true;
			sift_radix_scan(&g->w[v].pa, p, drain_chunk, &d);
		}
	}
	g->hooks.overflow(g->hooks.arg, left, nleft);
}

const SiftParOps sift_par_group_ops = {.phase1 = group_phase1,.seal = group_seal,.phase2 = group_phase2,.finish = group_finish,.overflow_drain = group_overflow};

static bool
worker_open(SiftParGroup *g, int t, bool high_card, SiftErr *err)
{
	GroupWorker *w = &g->w[t];
	SiftArena  *state = &g->ar->w[t].state;
	SiftArena  *work = &g->ar->w[t].work;

	if (state->cap > UINT32_MAX - 8 || work->cap > UINT32_MAX - 8)
		return sift_err_fill(err, SIFT_ERR_UNSUPPORTED, t, 0, "parallel aggregation: an arena of 4 GB"), false;
	w->local = g->proto;
	w->local.arena = state;
	w->radix = high_card;
	sift_radix_init(&w->raw, state);
	sift_radix_init(&w->pa, state);
	sift_radix_init(&w->fin, state);
	if (g->sp.topk > 0 && !sift_par_topk_init(&w->tk, work, g->sp.topk, sizeof(SiftTopEnt), g->sp.strs ? (work->cap - work->used) / 8 : 0, ent_better, ent_store, &g->sp))
		return fail_mem(err, "parallel aggregation: an arena is too small for its K best");
	if (!high_card && !cmp_init(&w->local, TAB_FIRST))
		return fail_mem(err, "parallel aggregation: an arena is too small for its first table");
	return true;
}

SiftParGroup *
sift_par_group_create(SiftParArenas *ar, const SiftParGroupSpec *spec, bool high_card, uint64 rows_bound, const SiftParGroupHooks *hooks, SiftErr *err)
{
	SiftArena  *boot = &ar->w[0].state;
	SiftParGroup *g;

	if (!sift_par_group_spec_holds(spec))
		return sift_err_fill(err, SIFT_ERR_INVALID_PARAM, spec->nk, spec->nsum, "parallel aggregation: the keys, the sums, K or a slot of more than 8 words"), NULL;
	g = sift_arena_alloc0(boot, sizeof(SiftParGroup), SIFT_ARENA_ALIGN);
	if (g != NULL)
		g->w = sift_arena_alloc0(boot, sizeof(GroupWorker) * ar->n, SIFT_ARENA_ALIGN);
	if (g == NULL || g->w == NULL)
		return fail_mem(err, "parallel aggregation: no room for the state"), NULL;
	g->sp = *spec;
	g->hooks = *hooks;
	g->ar = ar;
	g->nw = ar->n;
	if (!pgk_layout(&g->proto, spec, rows_bound))
		return sift_err_fill(err, SIFT_ERR_UNSUPPORTED, spec->nk, spec->nsum, "parallel aggregation: a slot of more than 8 words"), NULL;
	atomic_init(&g->radix, high_card);
	for (int p = 0; p < SIFT_RADIX_PARTS; p++)
		atomic_init(&g->done[p], false);
	if (spec->topk > 0)
	{
		const size_t bytes = 2 * (size_t) ar->n * spec->topk * sizeof(void *);
		uint8	   *p = sift_arena_alloc(boot, bytes, SIFT_ARENA_ALIGN);

		if (p == NULL)
			return fail_mem(err, "parallel aggregation: no room for the merge of the K best"), NULL;
		sift_arena_init(&g->merge, p, bytes);
	}
	for (int t = 0; t < g->nw; t++)
		if (!worker_open(g, t, high_card, err))
			return NULL;
	return g;
}
