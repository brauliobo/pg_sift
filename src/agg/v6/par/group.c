/*
 * agg/v6/par/group.c - the parallel grouped aggregation of agg/v6/par/group.h. Nothing here calls the server: the memory of a thread is its arenas, and what does not fit is an error code.
 */
#include "postgres.h"

#include <stdatomic.h>

#include "port/pg_bitutils.h"

#include "agg/v6/par/group.h"
#include "agg/v6/v6_group_cmp.h"
#include "core/par/radix.h"
#include "core/par/topk.h"

#define NO_ROOM "parallel aggregation: the arena of a thread has no room for a batch"
#define TAB_FIRST			256
#define TAB_PART_MAX		8192
#define ROWS_HALF_SLOT_MAX	(UINT64CONST(1) << 31)

typedef struct SiftTopEnt
{
	SiftParGroupRow row;
	uint8	   *buf;			/* the bytes of its text key, scap of them */
	uint32		scap;
} SiftTopEnt;

typedef struct GroupWorker
{
	SiftCompact local;
	bool		radix;			/* its rows go to the partitions */
	SiftRadixOut raw;			/* the rows */
	SiftRadixOut pa;			/* partial aggregates: the groups a table gave up */
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

static inline uint32
pad8(uint32 len)
{
	return len == SIFT_STR_NULL ? 0 : (len + 7) & ~7u;
}

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

/* ---- the groups of a table ---- */

static void
slot_row(const SiftCompact *c, const SiftParGroupSpec *sp, const uint64 *s, SiftParGroupRow *r)
{
	*r = (SiftParGroupRow) {0};
	if (!sp->str)
	{
		memcpy(r->key, s, sp->kw * sizeof(uint64));
		if (c->cnt_hi)
			r->key[1] = (uint32) s[1];
	}
	else if (s == c->null_group)
		r->len = SIFT_STR_NULL;
	else if (c->strm)
	{
		const char *p;

		sstr_slot_str(c, s, &p, &r->len);
		r->str = (const uint8 *) p;
	}
	else
	{
		const IStr *is = &c->in.str[s[0]];

		r->str = (const uint8 *) c->in.arena + is->off;
		r->len = is->len;
	}
	r->count = cmp_count(c, s);
	for (int m = 0; m < sp->nsum; m++)
		r->sum[m] = s[c->sum0 + m];
}

static uint64
row_hash(const SiftCompact *c, const SiftParGroupSpec *sp, const uint64 *k, const uint8 *p, uint32 len)
{
	if (!sp->str)
		return cmp_key_hash(c, k);
	return len == SIFT_STR_NULL ? 0 : cmp_str_hash((const char *) p, len);
}

/* The group into the table, added to what it has: a row (count 1) or a partial aggregate. false when the arena has no room */
static bool
add_group(SiftCompact *c, const SiftParGroupSpec *sp, const uint64 *k, const uint8 *p, uint32 len, uint32 h32, uint64 count, const uint64 *sum)
{
	uint64	   *s;

	if (sp->str && len == SIFT_STR_NULL)
		s = c->null_group;
	else
	{
		uint64		id;

		if (!cmp_reserve(c, 1, UINT64_MAX))
			return false;
		if (!sp->str)
			s = cmp_find(c, cmp_key_hash(c, k), k);
		else if (c->strm)
		{
			s = sstr_upsert(c, h32, (const char *) p, len);
			if (s == NULL)
				return false;
		}
		else
		{
			id = cmp_intern(c, (const char *) p, len);
			if (id == CMP_NO_ID)
				return false;
			s = cmp_find(c, sift_aggk6_hash1(id), &id);
		}
	}
	cmp_fold(c, s, count, sum);
	return true;
}

/* ---- the records: a row is the key words and the arguments (in 1, 2, 4 or 8 words); a text key's row is a word of its h32 and length, the bytes to 8, the arguments. A partial aggregate has the key as a row ---- */
/* ---- has it, then the count and the sums: the arguments of a row are its sums, and its count 1 ---- */

typedef struct Rec
{
	const uint64 *k;
	const uint8 *p;
	uint32		len;
	uint32		h32;
	uint64		count;
	const uint64 *sum;
} Rec;

static uint32
raw_words(const SiftParGroupSpec *sp)
{
	const uint32 n = sp->kw + sp->nsum;

	return n <= 1 ? 1 : cmp_slot_words(n);
}

/* The bytes of a record: a row (count 1) or a partial aggregate */
static uint32
rec_size(const SiftParGroupSpec *sp, bool partial, uint32 len)
{
	if (!sp->str)
		return 8 * (partial ? sp->kw + 1 + sp->nsum : raw_words(sp));
	return 8 + pad8(len) + 8 * (partial + sp->nsum);
}

static bool
put_rec(SiftRadixOut *o, uint32 part, const SiftParGroupSpec *sp, bool partial, const Rec *r)
{
	if (!sp->str)
	{
		const uint32 n = sp->kw + partial + sp->nsum;
		const uint32 words = partial ? n : raw_words(sp);
		uint64	   *w = sift_radix_reserve(o, part, rec_size(sp, partial, 0));
		uint32		i = sp->kw;

		if (w == NULL)
			return false;
		memcpy(w, r->k, sp->kw * sizeof(uint64));
		if (partial)
			w[i++] = r->count;
		memcpy(w + i, r->sum, sp->nsum * sizeof(uint64));
		memset(w + n, 0, (words - n) * sizeof(uint64));
	}
	else
	{
		const uint32 body = pad8(r->len);
		uint8	   *b = sift_radix_reserve(o, part, rec_size(sp, partial, r->len));
		const uint64 hdr = ((uint64) r->len << 32) | r->h32;
		uint64	   *t;

		if (b == NULL)
			return false;
		memcpy(b, &hdr, 8);
		if (body > 0)
		{
			memcpy(b + 8, r->p, r->len);
			memset(b + 8 + r->len, 0, body - r->len);
		}
		t = (uint64 *) (b + 8 + body);
		if (partial)
			*t++ = r->count;
		memcpy(t, r->sum, sp->nsum * sizeof(uint64));
	}
	return true;
}

/* The record at b, and the bytes it takes */
static uint32
parse_rec(const SiftParGroupSpec *sp, bool partial, const uint8 *b, Rec *r)
{
	if (!sp->str)
	{
		const uint64 *w = (const uint64 *) b;

		r->k = w;
		r->count = partial ? w[sp->kw] : 1;
		r->sum = w + sp->kw + partial;
	}
	else
	{
		uint64		hdr;
		const uint64 *t;

		memcpy(&hdr, b, 8);
		r->len = (uint32) (hdr >> 32);
		r->h32 = (uint32) hdr;
		r->p = b + 8;
		t = (const uint64 *) (b + 8 + pad8(r->len));
		r->count = partial ? t[0] : 1;
		r->sum = t + partial;
	}
	return rec_size(sp, partial, r->len);
}

/* The record of a row or a group into the partition of its key */
static bool
put_group(const SiftCompact *c, const SiftParGroupSpec *sp, SiftRadixOut *o, bool partial, const uint64 *k, const uint8 *p, uint32 len, uint64 count, const uint64 *sum)
{
	const uint64 h = row_hash(c, sp, k, p, len);
	const Rec	r = {.k = k,.p = p,.len = len,.h32 = cmp_h32(h),.count = count,.sum = sum};

	return put_rec(o, sift_radix_part(h), sp, partial, &r);
}

/* ---- the groups of a table, one by one, to the work of a thread ---- */

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
		slot_row(c, &wk->g->sp, s, &r);
		if (!fn(wk, &r))
			return false;
	}
	if (wk->g->sp.str && cmp_count(c, c->null_group) != 0)
	{
		slot_row(c, &wk->g->sp, c->null_group, &r);
		return fn(wk, &r);
	}
	return true;
}

/* ---- phase 1 ---- */

static bool
spill_row(const Walk *wk, const SiftParGroupRow *r)
{
	return put_group(wk->from, &wk->g->sp, &wk->g->w[wk->t].pa, true, r->key, r->str, r->len, r->count, r->sum);
}

static bool
size_row(const Walk *wk, const SiftParGroupRow *r)
{
	*wk->bytes += rec_size(&wk->g->sp, true, r->len);
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
	uint64		bytes = 0;
	Size		strbytes = 0;

	if (!w->radix && w->local.count > SIFT_PAR_GROUP_LOW && !spill_local(g, t, err))
		return false;
	if (!sp->str)
		bytes = (uint64) b->n * rec_size(sp, false, 0);
	for (uint32 i = 0; sp->str && i < b->n; i++)
	{
		bytes += rec_size(sp, false, b->str[i].len);
		strbytes += b->str[i].len == SIFT_STR_NULL ? 0 : b->str[i].len;
	}
	if (w->radix)
		return sift_radix_fits(&w->raw, bytes) || fail_mem(err, NO_ROOM);
	if (!cmp_reserve(&w->local, b->n, UINT64_MAX))
		return fail_mem(err, NO_ROOM);
	if (!sp->str)
		return true;
	return (w->local.strm ? sstr_put(&w->local, NULL, strbytes) : cmp_intern_reserve(&w->local, b->n, strbytes)) || fail_mem(err, NO_ROOM);
}

static bool
fold_batch(SiftParGroup *g, int t, const SiftParGroupBatch *b)
{
	GroupWorker *w = &g->w[t];
	const SiftParGroupSpec *sp = &g->sp;

	for (uint32 i = 0; i < b->n; i++)
	{
		uint64		k[SIFT_PAR_GROUP_MAX_KW];
		uint64		a[SIFT_PAR_GROUP_MAX_SUM];
		const uint8 *p = sp->str ? b->str[i].p : NULL;
		const uint32 len = sp->str ? b->str[i].len : 0;

		for (int j = 0; j < sp->kw; j++)
			k[j] = b->key[j][i];
		for (int m = 0; m < sp->nsum; m++)
			a[m] = b->arg[m][i];
		if (w->radix ? !put_group(&w->local, sp, &w->raw, false, k, p, len, 1, a)
			: !add_group(&w->local, sp, k, p, len, cmp_h32(row_hash(&w->local, sp, k, p, len)), 1, a))
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

/* ---- the groups to their consumer ---- */

static inline int64
metric_of(const SiftParGroupSpec *sp, const SiftParGroupRow *r)
{
	return (int64) (sp->order == 0 ? r->count : r->sum[sp->order - 1]);
}

static int
key_cmp(const SiftParGroupSpec *sp, const SiftParGroupRow *a, const SiftParGroupRow *b)
{
	int			c;

	if (!sp->str)
	{
		for (int i = 0; i < sp->kw; i++)
			if (a->key[i] != b->key[i])
				return a->key[i] < b->key[i] ? -1 : 1;
		return 0;
	}
	if (a->len == SIFT_STR_NULL || b->len == SIFT_STR_NULL)
		return a->len == b->len ? 0 : a->len == SIFT_STR_NULL ? -1 : 1;
	c = memcmp(a->str, b->str, Min(a->len, b->len));
	return c != 0 ? c : a->len == b->len ? 0 : a->len < b->len ? -1 : 1;
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
	return key_cmp(sp, x, y) < 0;
}

/* the group into entry dst, whose bytes of a text key are copied here, once the group is among the K best */
static bool
ent_store(void *dst, const void *cand, SiftArena *str, const void *ctx)
{
	const SiftParGroupRow *r = &((const SiftTopEnt *) cand)->row;
	SiftTopEnt *e = dst;
	const bool	copy = r->str != NULL && r->len != SIFT_STR_NULL;

	if (copy && (e->buf == NULL || r->len > e->scap))
	{
		const uint32 cap = Max(64, pg_nextpower2_32(r->len));
		uint8	   *buf = sift_arena_alloc(str, cap, 8);

		if (buf == NULL)
			return false;
		e->buf = buf;
		e->scap = cap;
	}
	e->row = *r;
	if (copy && r->len > 0)
		memcpy(e->buf, r->str, r->len);
	e->row.str = copy ? e->buf : NULL;
	return true;
}

static bool
deliver_row(const Walk *wk, const SiftParGroupRow *r)
{
	SiftParGroup *g = wk->g;

	if (g->sp.topk == 0)
	{
		g->hooks.emit(g->hooks.arg, wk->t, r);
		return true;
	}
	return sift_par_topk_offer(&g->w[wk->t].tk, &(SiftTopEnt) {.row = *r});
}

static bool
merge_row(const Walk *wk, const SiftParGroupRow *r)
{
	const SiftParGroupSpec *sp = &wk->g->sp;

	return add_group(wk->to, sp, r->key, r->str, r->len, cmp_h32(row_hash(wk->to, sp, r->key, r->str, r->len)), r->count, r->sum);
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
	ok = ok && table_each(&wk, deliver_row);
	sift_arena_rewind(work, mark);
	return ok || fail_mem(err, "parallel aggregation: the arena of the merge is full");
}

/* ---- phase 2 ---- */

typedef struct Fold
{
	const SiftParGroupSpec *sp;
	SiftCompact *tab;
	bool		partial;
} Fold;

/* the records of a chunk, each as its table takes it */
static bool
fold_chunk(void *arg, const uint8 *b, uint32 len)
{
	const Fold *f = arg;

	for (uint32 off = 0; off < len;)
	{
		Rec			r;

		off += parse_rec(f->sp, f->partial, b + off, &r);
		if (!add_group(f->tab, f->sp, r.k, r.p, r.len, r.h32, r.count, r.sum))
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

	if (!fold_partition(g, part, &tab, work) || !table_each(&wk, deliver_row))
		return fail_mem(err, "parallel aggregation: the arena of a partition is full");
	atomic_store_explicit(&g->done[part], true, memory_order_release);
	return true;
}

/* ---- the answer ---- */

static void
group_finish(void *vg)
{
	SiftParGroup *g = vg;
	SiftParTopK *heap[SIFT_PAR_MAX_THREADS] = {0};
	const void **sorted;
	uint32		n;
	bool		merged;

	if (g->sp.topk == 0)
		return;
	for (int t = 0; t < g->nw; t++)
		heap[t] = &g->w[t].tk;
	sift_arena_reset(&g->merge);
	merged = sift_par_topk_merge(heap, g->nw, &g->merge, &sorted, &n);
	SIFT_INVARIANT(merged, "parallel aggregation: the arena of the K best was made too small");
	for (uint32 i = 0; i < n; i++)
		g->hooks.emit(g->hooks.arg, 0, &((const SiftTopEnt *) sorted[i])->row);
}

/* ---- what was not delivered ---- */

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
	const SiftParGroupSpec *sp = &d->g->sp;

	for (uint32 off = 0; off < len;)
	{
		Rec			r;
		SiftParGroupRow row = {0};

		off += parse_rec(sp, d->partial, b + off, &r);
		if (!sp->str)
			memcpy(row.key, r.k, sp->kw * sizeof(uint64));
		row.str = r.p;
		row.len = r.len;
		row.count = r.count;
		memcpy(row.sum, r.sum, sp->nsum * sizeof(uint64));
		d->g->hooks.emit(d->g->hooks.arg, d->worker, &row);
	}
	return true;
}

static void
group_overflow(void *vg, const SiftParRange *left, uint32 nleft)
{
	SiftParGroup *g = vg;

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

/* ---- the state ---- */

static bool
spec_holds(const SiftParGroupSpec *sp)
{
	return (sp->str ? sp->kw == 0 : sp->kw >= 1 && sp->kw <= SIFT_PAR_GROUP_MAX_KW) && sp->nsum <= SIFT_PAR_GROUP_MAX_SUM && sp->topk <= SIFT_PAR_GROUP_TOPK_MAX &&
		sp->order <= sp->nsum;
}

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
	if (g->sp.topk > 0 && !sift_par_topk_init(&w->tk, work, g->sp.topk, sizeof(SiftTopEnt), g->sp.str ? (work->cap - work->used) / 8 : 0, ent_better, ent_store, &g->sp))
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

	if (!spec_holds(spec))
		return sift_err_fill(err, SIFT_ERR_INVALID_PARAM, spec->kw, spec->nsum, "parallel aggregation: the words of the key, the sums or K"), NULL;
	g = sift_arena_alloc0(boot, sizeof(SiftParGroup), SIFT_ARENA_ALIGN);
	if (g != NULL)
		g->w = sift_arena_alloc0(boot, sizeof(GroupWorker) * ar->n, SIFT_ARENA_ALIGN);
	if (g == NULL || g->w == NULL)
		return fail_mem(err, "parallel aggregation: no room for the state"), NULL;
	g->sp = *spec;
	g->hooks = *hooks;
	g->ar = ar;
	g->nw = ar->n;
	g->proto = (SiftCompact) {.kw = spec->str ? 1 : spec->kw,.strk = spec->str ? 0 : -1,.nsum = spec->nsum};
	if (!cmp_layout(&g->proto, spec->key1_u32 && rows_bound < ROWS_HALF_SLOT_MAX))
		return sift_err_fill(err, SIFT_ERR_UNSUPPORTED, spec->kw, spec->nsum, "parallel aggregation: a slot of more than 8 words"), NULL;
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
