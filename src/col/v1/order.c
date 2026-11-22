/*
 * col/v1/order.c - key-cut units: the rows of a build, a promotion or a merge are sorted by the declared order key (the option key_cut) and cut
 * into units of a range of the key, whatever their heap pages. Each unit lists its TIDs (enc.c, unit.c).
 */
#include "postgres.h"

#include "core/cmp.h"
#include "col/v1/build.h"
#include "col/v1/enc.h"
#include "col/v1/order_bound.h"
#include "col/v1/order_sort.h"
#include "col/v1/runs.h"
#include "col/v1/seg.h"
#include "core/damage.h"
#include "core/memtrack.h"

#define SORT_CHUNK_ROWS		4096		/* the rows of the sort that one read of the merge takes */

typedef struct RowRef
{
	const SiftColBatch *b;
	uint32		r;
	uint32		chunk;			/* a merge: the chunk of its input that b is, to unpin when the row has left the window */
} RowRef;


static inline bool
ref_null(const RowRef *x, int leaf)
{
	const SiftCVals *v = &x->b->col[leaf];

	return v->nulls != NULL && sift_col_bit_test(v->nulls, x->r);
}

static int
key_cmp_from(const RowRef *x, const RowRef *y, const OrderCtx *c, int from, int upto)
{
	for (int i = from; i < upto && i < c->nleaf; i++)
	{
		int			leaf = c->leaf[i];
		bool		nx = ref_null(x, leaf),
					ny = ref_null(y, leaf);
		int64		kx,
					ky;

		if (nx || ny)
		{
			if (nx != ny)
				return nx ? 1 : -1;	/* NULL sorts last, as PostgreSQL's default */
			continue;
		}
		kx = sift_crun_key(&c->schema->cols[leaf], x->b->col[leaf].key[x->r]);
		ky = sift_crun_key(&c->schema->cols[leaf], y->b->col[leaf].key[y->r]);
		if (kx != ky)
			return kx < ky ? -1 : 1;
	}
	return 0;
}

static inline int
key_cmp(const RowRef *x, const RowRef *y, const OrderCtx *c, int upto)
{
	return key_cmp_from(x, y, c, 0, upto);
}

static int
ref_cmp(const void *a, const void *b, void *arg)
{
	const RowRef *x = a,
			   *y = b;
	int			c = key_cmp(x, y, arg, ((const OrderCtx *) arg)->nleaf);

	if (c != 0)
		return c;
	return x->b->keys[x->r] < y->b->keys[y->r] ? -1 : x->b->keys[x->r] > y->b->keys[y->r];
}

typedef bool (*RowNext) (void *state, RowRef *r);
/* the rows that left the window, for a source that owns what they point into */
typedef void (*RowDone) (void *state, const RowRef *refs, uint64 n);

/*
 * The rows of a build in the order of the key: the first leaf's key and null flag and the TID are copied next to the reference, so that the sort compares
 * neighbouring memory and not two rows of two batches; the other leaves are read from the batches only for rows equal on the first.
 */
typedef struct SortEnt
{
	int64		k,
				k2;				/* the second leaf, when there is one */
	SiftKey		tid;
	RowRef		ref;
	bool		null,
				null2;
} SortEnt;

static inline int
ent_cmp(const OrderCtx *c, const SortEnt *x, const SortEnt *y)
{
	if (x->null != y->null)
		return x->null ? 1 : -1;
	if (!x->null && x->k != y->k)
		return x->k < y->k ? -1 : 1;
	if (c->nleaf > 1)
	{
		int			r;

		if (x->null2 != y->null2)
			return x->null2 ? 1 : -1;
		if (!x->null2 && x->k2 != y->k2)
			return x->k2 < y->k2 ? -1 : 1;
		r = c->nleaf > 2 ? key_cmp_from(&x->ref, &y->ref, c, 2, c->nleaf) : 0;
		if (r != 0)
			return r;
	}
	return x->tid < y->tid ? -1 : x->tid > y->tid;
}

#define ST_SORT sort_ents
#define ST_ELEMENT_TYPE SortEnt
#define ST_COMPARE_ARG_TYPE OrderCtx
#define ST_COMPARE(a, b, c) ent_cmp(c, a, b)
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

/* refs[0, n) sorted as ref_cmp orders them */
static void
sort_refs(RowRef *refs, uint64 n, const OrderCtx *ctx)
{
	SortEnt    *e = palloc((Size) n * sizeof(SortEnt));

	for (uint64 i = 0; i < n; i++)
	{
		const RowRef *x = &refs[i];
		e[i].ref = *x;
		e[i].tid = x->b->keys[x->r];
		e[i].null = ctx->nleaf > 0 && ref_null(x, ctx->leaf[0]);
		e[i].k = ctx->nleaf == 0 || e[i].null ? 0 : sift_crun_key(&ctx->schema->cols[ctx->leaf[0]], x->b->col[ctx->leaf[0]].key[x->r]);
		e[i].null2 = ctx->nleaf > 1 && ref_null(x, ctx->leaf[1]);
		e[i].k2 = ctx->nleaf < 2 || e[i].null2 ? 0 : sift_crun_key(&ctx->schema->cols[ctx->leaf[1]], x->b->col[ctx->leaf[1]].key[x->r]);
	}
	sort_ents(e, n, (OrderCtx *) ctx);
	for (uint64 i = 0; i < n; i++)
		refs[i] = e[i].ref;
	pfree(e);
}

/* A row's TID next to its place, so that the sort by TID compares dense pairs and not the batch of each row (a cache miss a comparison) */
typedef struct TidPos
{
	SiftKey		tid;
	uint32		pos;
} TidPos;

#define ST_SORT sort_tid_pos
#define ST_ELEMENT_TYPE TidPos
#define ST_COMPARE(p, q) SIFT_CMP3((p)->tid, (q)->tid)
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

/* refs[0, n) in TID order; tp and tmp: room for n entries */
static void
sort_by_tid(RowRef *refs, uint64 n, TidPos *tp, RowRef *tmp)
{
	for (uint64 i = 0; i < n; i++)
		tp[i] = (TidPos) {refs[i].b->keys[refs[i].r], (uint32) i};
	sort_tid_pos(tp, n);
	for (uint64 i = 0; i < n; i++)
		tmp[i] = refs[tp[i].pos];
	memcpy(refs, tmp, n * sizeof(RowRef));
}

/* the rows of refs[0, avail) that have the first leaf of refs[0] (the rows are in the order of the key) */
static uint64
key_run(const OrderCtx *ctx, const RowRef *refs, uint64 avail)
{
	uint64		lo = 1,
				hi = avail;

	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;

		if (key_cmp(&refs[0], &refs[mid], ctx, 1) == 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* the first row of refs[0, end] that has the first leaf of refs[end] */
static uint64
key_start(const OrderCtx *ctx, const RowRef *refs, uint64 end)
{
	uint64		lo = 0,
				hi = end;

	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;

		if (key_cmp(&refs[mid], &refs[end], ctx, 1) == 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

/* The first leaf of the order is the derived key of a scope: the segment is a scope segment */
static inline bool
scope_order(const OrderCtx *c)
{
	return c->nleaf > 0 && (c->schema->cols[c->leaf[0]].flags & SIFT_COLD_SCOPE) != 0;
}

/* Puts an address in the open-addressing set seen (a power of two entries, 0 is empty); true when it was there */
static bool
scope_seen(uint64 *seen, uint64 hsize, SiftKey tid)
{
	uint64		x = (uint64) tid + 1;
	uint64		h = (x * UINT64CONST(0x9E3779B97F4A7C15)) >> 32;

	for (h &= hsize - 1; seen[h] != 0; h = (h + 1) & (hsize - 1))
		if (seen[h] == x)
			return true;
	seen[h] = x;
	return false;
}

/*
 * The rows of a fact are replicated once for each scope key of its member, and a unit lists an address once. A unit of a scope segment holds the rows of one scope key, or, for a key
 * of fewer than SIFT_COLK_SCOPE_FLOOR rows, the whole runs of the keys after it that fit in that many rows together and have no address in common: a unit costs the same bytes
 * whatever its rows, and most keys are small. The zone of the derived key still names the keys of the unit, and a read of one key tests the key of each row (at most the floor in
 * rows). refs[0, avail) are in the order of the key, the unit's rows are refs[0, end) at most, and avail < want when refs ends with the rows of the build (a run that reaches the
 * end of refs is whole only then).
 */
static uint64
scope_cut(const OrderCtx *ctx, const RowRef *refs, uint64 avail, uint64 end, uint64 want)
{
	uint64		limit = Min(end, SIFT_COLK_SCOPE_FLOOR),
				n = key_run(ctx, refs, end),
				hsize = 16,
			   *seen;

	if (n >= limit)
		return n;
	while (hsize < 2 * limit)
		hsize *= 2;
	seen = palloc0(hsize * sizeof(uint64));
	for (uint64 i = 0; i < n; i++)
		scope_seen(seen, hsize, refs[i].b->keys[refs[i].r]);
	while (n < limit)
	{
		uint64		run = key_run(ctx, refs + n, avail - n);
		bool		clash = n + run > limit || (n + run == avail && avail >= want);

		for (uint64 i = 0; i < run && !clash; i++)
			clash = scope_seen(seen, hsize, refs[n + i].b->keys[refs[n + i].r]);
		if (clash)
			break;
		n += run;
	}
	pfree(seen);
	return n;
}

/*
 * Where a window of avail rows refs[0, avail) in the order of the key is cut: after unit_rows of them, or a little earlier or later where the first leaf changes
 * (within slack rows), so that one value of the leading leaf is in as few units as it can be. A key of at least 2 * SIFT_COLK_SECOND_ROWS rows is sorted by the second leaf
 * and cut into units of SIFT_COLK_SECOND_ROWS, the last one ending with the key (*split: the cut before this one was one of these), and starts its own unit: a unit of the
 * whole aim spans most of the key's range, and a window of the second leaf could skip none.
 */
static uint64
cut_at(const OrderCtx *ctx, const RowRef *refs, uint64 avail, uint64 unit_rows, uint64 slack, bool *split)
{
	uint64		end = Min(avail, unit_rows);
	bool		second = ctx->nleaf > 1 && unit_rows >= 2 * SIFT_COLK_SECOND_ROWS;

	if (second)
	{
		uint64		run = key_run(ctx, refs, Min(avail, 2 * SIFT_COLK_SECOND_ROWS));
		bool		more = run == 2 * SIFT_COLK_SECOND_ROWS;

		if (more || *split)
		{
			*split = more;
			return more ? SIFT_COLK_SECOND_ROWS : run;
		}
	}
	*split = false;
	if (scope_order(ctx))
		return scope_cut(ctx, refs, avail, end, unit_rows + slack + 1);
	if (end < avail && ctx->nleaf > 0)
	{
		uint64		best = 0;

		for (uint64 d = 0; d < slack + 1 && best == 0; d++)
		{
			if (end >= d + 1 && key_cmp(&refs[end - d - 1], &refs[end - d], ctx, 1) != 0)
				best = end - d;
			if (best == 0 && d > 0 && end + d < avail && key_cmp(&refs[end + d - 1], &refs[end + d], ctx, 1) != 0)
				best = end + d;
		}
		if (best > 0)
			end = best;
	}
	if (second && end < avail && key_cmp(&refs[end - 1], &refs[end], ctx, 1) == 0)
	{
		uint64		start = key_start(ctx, refs, end);

		if (start > 0 && key_run(ctx, refs + start, Min(avail - start, 2 * SIFT_COLK_SECOND_ROWS)) == 2 * SIFT_COLK_SECOND_ROWS)
			end = start;
	}
	return end;
}

/*
 * Cuts the rows into units of about unit_rows, each cut where the first leaf changes if that is within a quarter of unit_rows of the count (so that one value of the
 * leading leaf is in as few units as it can be), the rows of a unit in TID order. The rows are in the order of the key (and of the TID within a key): either the
 * array refs (which outlives the call), read where it is, or the stream next, whose rows wait as references in a window of unit_rows + unit_rows / 4 + 1, which is
 * the memory of the cut; done tells the stream which rows left, so that it can free what they point into. Each row is copied once, into its unit. Returns the number of units.
 */
static int
cut_rows(SiftColSegWriter *w, RowRef *refs, uint64 total, RowNext next, RowDone done, void *state, const SiftColSchema *schema, const OrderCtx *ctx, uint32 unit_rows,
		 MemoryContext cx, SiftColCutFn cutfn, void *cutarg, uint32 seq0)
{
	uint64		slack = unit_rows / 4;
	uint64		want = unit_rows + slack + 1;
	SiftColBatch *out = sift_cbatch_create(cx, schema);
	RowRef	   *win = refs ? NULL : palloc_array(RowRef, 2 * want + 2);
	TidPos	   *tidpos = palloc_array(TidPos, want);
	RowRef	   *tidtmp = palloc_array(RowRef, want);
	uint64		n = 0, pos = 0, whead = 0;
	uint32		seq = seq0, num = 0;
	bool		eof = false, split = false;

	for (;;)
	{
		uint64		avail,
					end;
		RowRef	   *u;

		if (refs)
		{
			avail = Min(want, total - pos);
			u = refs + pos;
		}
		else
		{
			if (whead > want)
			{
				memmove(win, win + whead, n * sizeof(RowRef));
				whead = 0;
			}
			while (!eof && n < want)
				if (next(state, &win[whead + n]))
					n++;
				else
					eof = true;
			avail = n;
			u = win + whead;
		}
		if (avail == 0)
			break;
		end = cut_at(ctx, u, avail, unit_rows, slack, &split);
		sort_by_tid(u, end, tidpos, tidtmp);
		/*
		 * The number of a unit is the sequence of the cut by a key. Without a key the units are in the order of the TIDs and take the superblock of their first
		 * row (or the one after the unit before it), so that a walk of the superblocks meets an overlay next to the units of the heap it overlays
		 */
		if (ctx->nleaf == 0)
			num = Max(num + (seq > 0), sift_key_block(u[0].b->keys[u[0].r]) / SIFT_SB_PAGES);
		else
			num = seq;
		sift_colb_peak_note();
		sift_cbatch_reset(out, num);
		sift_cbatch_reserve(out, (uint32) end);
		seq++;
		out->keyed = true;
		sift_cbatch_add_scoped_rows(out, (const SiftColRowRef *) u, sizeof(RowRef), (uint32) end, -1, 0, NULL);
		if (w == NULL)
			w = cutfn(cutarg, NULL, false);
		sift_colw_add(w, out);
		if (cutfn)
		{
			if (refs == NULL && n == end && !eof)
				eof = !next(state, &u[n++]);
			w = cutfn(cutarg, w, ctx->nleaf > 0 && (refs ? avail : n) > end && key_cmp(&u[end - 1], &u[end], ctx, 1) == 0);
		}
		if (refs)
			pos += end;
		else
		{
			if (done)
				done(state, u, end);
			whead += end;
			n -= end;
		}
		CHECK_FOR_INTERRUPTS();
	}
	pfree(tidpos);
	pfree(tidtmp);
	return seq - seq0;
}

/*
 * Sorts the rows of the batches by the leaves of the order key and adds them to w as units (cut_rows). Returns the number of units; the first is numbered unit_base. Memory: one
 * reference per row, and the batches themselves; sift_colk_merge is the form that holds a unit of each input.
 */
int
sift_colk_write(SiftColSegWriter *w, SiftColBatch *const *in, int nin, const SiftColSchema *schema, const int *leaf, int nleaf, uint32 unit_rows,
				MemoryContext cx, uint32 unit_base)
{
	OrderCtx	ctx = {schema, leaf, nleaf};
	uint64		total = 0,
				k = 0;
	RowRef	   *refs;
	int			units;

	for (int i = 0; i < nin; i++)
		total += in[i]->nrows;
	if (total == 0)
		return 0;
	/* check-memory: allow the rows of the units of the inputs a merge holds in its window */
	refs = palloc((Size) total * sizeof(RowRef));
	for (int i = 0; i < nin; i++)
		for (uint32 r = 0; r < in[i]->nrows; r++)
			refs[k++] = (RowRef) {in[i], r};
	sort_refs(refs, total, &ctx);
	units = cut_rows(w, refs, total, NULL, NULL, NULL, schema, &ctx, unit_rows, cx, NULL, NULL, unit_base);
	pfree(refs);
	return units;
}

/* The order of row ra of batch a and row rb of batch b by the leaves, then by address, as the cut sorts the rows (sift_colk_streams checks a segment against it) */
int
sift_colk_row_cmp(const SiftColSchema *schema, const int *leaf, int nleaf, const SiftColBatch *a, uint32 ra, const SiftColBatch *b, uint32 rb)
{
	OrderCtx	ctx = {schema, leaf, nleaf};
	RowRef		x = {a, ra, 0},
				y = {b, rb, 0};

	return ref_cmp(&x, &y, &ctx);
}

/* One input of a merge: a key-cut segment, whose units follow each other in the order of the key, one cut another way, whose rows are loaded and sorted whole, or the sort of those when they do not fit */
typedef struct MergeIn
{
	SiftColSegment *seg;
	uint64		gen;
	bool		keyed;
	uint32		next_unit;
	int			chunk;			/* the loaded chunk (MergeSrc.chunk), -1 before the first; the one before it is kept until the new one is checked against it */
	RowRef	   *ord;			/* the rows of the chunk in the order of the key */
	uint32		ordcap;
	uint32		n;
	uint32		pos;
	bool		done;
	bool		bounded;		/* the lo and hi of the merge cut its rows */
	bool		sorted;			/* the rows come from the sort of the inputs that are not streams (MergeSrc.sorter): the last input of the merge, whose rows the sort has ordered */
	BoundKey	head;			/* a stream that waits (MergeSrc.wait): no row of it is below this, from the zones of its next unit */
} MergeIn;

/* The rows of a chunk stay where they were decoded until the window of the cut has copied them into a unit: a chunk is reused when no row of it is pinned by the window and its input has loaded another */
typedef struct MergeChunk
{
	SiftColBatch *b;
	uint32		pins;
	bool		held;			/* the loaded chunk of its input */
	int			next_free;		/* the next chunk of the free list, -1 at the end; -2: not on it */
} MergeChunk;

typedef struct MergeSrc
{
	MergeChunk *chunk;
	int			nchunk;
	int			chunk_cap;
	int			free_head;		/* the first chunk no input holds and no row of the window points into, -1: none */
	MergeIn    *in;
	int			n;
	const OrderCtx *ctx;
	SiftColFreezeFn freeze;
	void	   *freeze_arg;
	MemoryContext scratch;
	uint64		budget;
	SiftColSorter *sorter;		/* the rows of the inputs that are not streams, when they do not fit the budget loaded whole: a sort that spills (order_sort.c) */
	bool		presorted;		/* the sort was given and is over: the merge only reads it */
	MemoryContext cx;
	MergeIn   **heap;			/* the inputs with rows left, the one whose next row comes first at 0 */
	int			nheap;
	MergeIn   **wait;			/* the streams not read yet, the one whose head is lowest at 0: an input is opened (its unit decoded, its dictionaries parsed) when the rows reach its head */
	int			nwait;
	const BoundKey *lo;			/* a slice of the key range: the rows from lo (NULL: from the first) up to hi (NULL: to the last), not including it */
	const BoundKey *hi;
	SiftMemTracker *inputs;		/* the open inputs of the merge, measured when a chunk is loaded; NULL when the caller measures none */
} MergeSrc;

/* A chunk no input holds and no row of the window points into gives its arrays back: the memory of a merge is the chunks of its inputs in hand, not the largest one each slot held; it is the next to be reused */
static void
chunk_free(MergeSrc *m, int i)
{
	if (m->chunk[i].pins != 0 || m->chunk[i].held || m->chunk[i].next_free != -2)
		return;
	sift_cbatch_release(m->chunk[i].b);
	m->chunk[i].next_free = m->free_head;
	m->free_head = i;
}

static int
chunk_get(MergeSrc *m, const SiftColSchema *schema)
{
	int			i = m->free_head;

	if (i >= 0)
	{
		m->free_head = m->chunk[i].next_free;
		m->chunk[i].next_free = -2;
		return i;
	}
	if (m->nchunk == m->chunk_cap)
	{
		m->chunk_cap = Max(16, m->chunk_cap * 2);
		m->chunk = m->chunk ? repalloc(m->chunk, m->chunk_cap * sizeof(MergeChunk)) : MemoryContextAlloc(m->cx, m->chunk_cap * sizeof(MergeChunk));
	}
	m->chunk[m->nchunk] = (MergeChunk) {.b = sift_cbatch_create(m->cx, schema), .next_free = -2};
	return m->nchunk++;
}

static void
load_chunk(MergeSrc *m, MergeIn *in)
{
	SiftColSegment *seg = in->seg;
	const SiftColSchema *schema = m->ctx->schema;
	MemoryContext old;
	int			prev = in->chunk;
	int			cur = chunk_get(m, schema);
	SiftColBatch *b = m->chunk[cur].b;

	sift_cbatch_reset(b, 0);
	old = MemoryContextSwitchTo(m->scratch);
	if (in->sorted)
	{
		b->keyed = true;
		b->unsorted = true;
		while (b->nrows < SORT_CHUNK_ROWS && sift_colsort_next(m->sorter, b))
			;
		if (b->nrows == 0)
		{
			sift_colsort_end(m->sorter);
			m->sorter = NULL;
		}
	}
	else if (in->keyed)
	{
		if (m->lo && in->bounded && prev < 0)
			in->next_unit = sift_colk_first_unit(m->ctx, seg, m->lo);
		while (b->nrows == 0 && in->next_unit < seg->nsb)
		{
			sift_cbatch_reserve(b, seg->sbdir[in->next_unit].nrows);
			sift_cseg_decode_unit_freeze(seg, in->next_unit++, b, m->freeze, m->freeze_arg);
		}
	}
	else
	{
		SiftColBatch *one = sift_cbatch_create(m->cx, schema);

		b->keyed = true;		/* the rows of many superblocks */
		b->unsorted = true;		/* the units of a segment cut by another key each span the heap; the rows are sorted by reference below */
		sift_cbatch_reserve(b, (uint32) sift_cseg_live_rows(seg));
		for (; in->next_unit < seg->nsb; in->next_unit++)
		{
			sift_cseg_decode_unit_freeze(seg, in->next_unit, one, m->freeze, m->freeze_arg);
			for (uint32 r = 0; r < one->nrows; r++)
				sift_cbatch_add_from(b, one, r);
			MemoryContextReset(m->scratch);
		}
		pfree(one);
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(m->scratch);
	in->chunk = cur;
	in->n = b->nrows;
	in->pos = 0;
	if (m->inputs != NULL)
		sift_mem_assert_within(m->inputs);
	if (in->n == 0)
		in->done = true;
	else
	{
		if (in->ordcap < in->n)
		{
			in->ordcap = Max(in->n, 2 * in->ordcap);
			SIFT_REGROW_HUGE_IN(m->cx, in->ord, in->ordcap * sizeof(RowRef));
		}
		for (uint32 r = 0; r < in->n; r++)
			in->ord[r] = (RowRef) {b, r, cur};
		if (!in->sorted)
			sort_refs(in->ord, in->n, m->ctx);
		if (in->keyed && prev >= 0 && m->chunk[prev].b->nrows > 0)
		{
			RowRef		last = {m->chunk[prev].b, m->chunk[prev].b->nrows - 1};

			/* the units of a segment follow each other in the order of the key it was cut by: another key (the option was changed) cannot be merged as a stream */
			if (ref_cmp(&last, &in->ord[0], (void *) m->ctx) > 0)
				SIFT_DAMAGE_DATA(NULL, errmsg("segment %llu of a key_cut index is not in the order of the key", (unsigned long long) in->gen),
								errhint("REINDEX the index."));
		}
	}
	m->chunk[cur].held = !in->done;
	chunk_free(m, cur);
	if (prev >= 0)
	{
		m->chunk[prev].held = false;
		chunk_free(m, prev);
	}
	if (m->lo && in->bounded && !in->done)
	{
		while (in->pos < in->n && sift_colk_bound_cmp(m->ctx, in->ord[in->pos].b, in->ord[in->pos].r, m->lo) < 0)
			in->pos++;
		if (in->pos == in->n)
			load_chunk(m, in);
	}
}

/* Is row r of b inside the window of the merge (the slice or the window of the rewrite): from lo up to hi, not including it? */
static bool
in_window(const MergeSrc *m, const SiftColBatch *b, uint32 r)
{
	return (m->lo == NULL || sift_colk_bound_cmp(m->ctx, b, r, m->lo) >= 0) && (m->hi == NULL || sift_colk_bound_cmp(m->ctx, b, r, m->hi) < 0);
}

/* The rows of the inputs that are not streams go to the sort, but for the ones of an input the window cuts that are outside it (another merge writes them) */
static void
sort_fill(MergeSrc *m)
{
	for (int i = 0; i < m->n; i++)
	{
		MergeIn    *in = &m->in[i];
		SiftColBatch *one;

		if (in->keyed || in->sorted)
			continue;
		one = sift_cbatch_create(m->cx, m->ctx->schema);
		for (; in->next_unit < in->seg->nsb; in->next_unit++)
		{
			MemoryContext old = MemoryContextSwitchTo(m->scratch);

			sift_cseg_decode_unit_freeze(in->seg, in->next_unit, one, m->freeze, m->freeze_arg);
			for (uint32 r = 0; r < one->nrows; r++)
				if (!in->bounded || in_window(m, one, r))
					sift_colsort_put(m->sorter, one, r);
#ifdef SIFT_TESTING
				else
					sift_test_cut_sort_cut++;
#endif
			MemoryContextSwitchTo(old);
			MemoryContextReset(m->scratch);
		}
		in->done = true;
		pfree(one);
	}
	sift_colsort_done(m->sorter);
}

/* Does the next row of input a come before that of b? Equal rows (one row of two inputs) go in the order of the inputs */
static inline bool
in_before(const MergeSrc *m, const MergeIn *a, const MergeIn *b)
{
	int			c = ref_cmp(&a->ord[a->pos], &b->ord[b->pos], (void *) m->ctx);

	return c < 0 || (c == 0 && a < b);
}

static void
heap_down(MergeSrc *m, int i)
{
	MergeIn    *x = m->heap[i];

	for (int c; (c = 2 * i + 1) < m->nheap; i = c)
	{
		if (c + 1 < m->nheap && in_before(m, m->heap[c + 1], m->heap[c]))
			c++;
		if (!in_before(m, m->heap[c], x))
			break;
		m->heap[i] = m->heap[c];
	}
	m->heap[i] = x;
}

static void
heap_push(MergeSrc *m, MergeIn *in)
{
	int			i = m->nheap++;

	for (int p; i > 0 && in_before(m, in, m->heap[p = (i - 1) / 2]); i = p)
		m->heap[i] = m->heap[p];
	m->heap[i] = in;
}

static int
wait_order(const MergeIn *a, const MergeIn *b)
{
	int			c = sift_colk_bound_order(&a->head, &b->head);

	return c != 0 ? c : a < b ? -1 : 1;
}

static void
wait_up(MergeSrc *m, int i)
{
	MergeIn    *x = m->wait[i];

	for (int p; i > 0 && wait_order(x, m->wait[p = (i - 1) / 2]) < 0; i = p)
		m->wait[i] = m->wait[p];
	m->wait[i] = x;
}

static void
wait_down(MergeSrc *m, int i)
{
	MergeIn    *x = m->wait[i];

	for (int c; (c = 2 * i + 1) < m->nwait; i = c)
	{
		if (c + 1 < m->nwait && wait_order(m->wait[c + 1], m->wait[c]) < 0)
			c++;
		if (wait_order(m->wait[c], x) >= 0)
			break;
		m->wait[i] = m->wait[c];
	}
	m->wait[i] = x;
}

/* A stream that waits: it has rows at or above its head, none below. An input with no row in the merge's slice is done without being read */
static void
wait_add(MergeSrc *m, MergeIn *in)
{
	if (m->lo && in->bounded)
		in->next_unit = sift_colk_first_unit(m->ctx, in->seg, m->lo);
	if (in->next_unit >= in->seg->nsb)
	{
		in->done = true;
		return;
	}
	sift_colk_unit_head(m->ctx, in->seg, in->next_unit, &in->head);
	if (m->hi && in->bounded && sift_colk_bound_order(&in->head, m->hi) >= 0)
	{
		in->done = true;
		return;
	}
	m->wait[m->nwait] = in;
	wait_up(m, m->nwait++);
}

/* The waiting inputs the rows have reached are read: their head is not above the next row, and any when no input has one */
static void
wake_inputs(MergeSrc *m)
{
	while (m->nwait > 0)
	{
		MergeIn    *in = m->wait[0];

		if (m->nheap > 0)
		{
			const RowRef *next = &m->heap[0]->ord[m->heap[0]->pos];

			if (sift_colk_bound_cmp(m->ctx, next->b, next->r, &in->head) < 0)
				return;
		}
		m->wait[0] = m->wait[--m->nwait];
		if (m->nwait > 0)
			wait_down(m, 0);
		load_chunk(m, in);
		if (!in->done)
			heap_push(m, in);
	}
}

static bool
merge_lazy(const MergeSrc *m, const MergeIn *in)
{
#ifdef SIFT_TESTING
	if (sift_test_col_merge_eager)
		return false;
#endif
	return in->keyed && !in->sorted && m->ctx->nleaf > 0;
}

static bool
merge_next(void *state, RowRef *r)
{
	MergeSrc   *m = state;
	MergeIn    *best;

	if (m->heap == NULL)
	{
		m->heap = MemoryContextAlloc(m->cx, m->n * sizeof(MergeIn *));
		m->wait = MemoryContextAlloc(m->cx, m->n * sizeof(MergeIn *));
		if (m->sorter && !m->presorted)
			sort_fill(m);
		for (int i = 0; i < m->n; i++)
		{
			MergeIn    *in = &m->in[i];

			if (in->done)
				continue;
			if (merge_lazy(m, in))
			{
				wait_add(m, in);
				continue;
			}
			load_chunk(m, in);
			if (!in->done)
				m->heap[m->nheap++] = in;
		}
		for (int i = m->nheap / 2 - 1; i >= 0; i--)
			heap_down(m, i);
	}
	for (;;)
	{
		wake_inputs(m);
		if (m->nheap == 0)
			return false;
		best = m->heap[0];
		if (m->hi == NULL || !best->bounded || sift_colk_bound_cmp(m->ctx, best->ord[best->pos].b, best->ord[best->pos].r, m->hi) < 0)
			break;
		/* the rest of this input is in the slices above */
		m->chunk[best->chunk].held = false;
		chunk_free(m, best->chunk);
		sift_cseg_dict_release(best->seg);
		best->done = true;
		m->heap[0] = m->heap[--m->nheap];
		if (m->nheap > 0)
			heap_down(m, 0);
	}
	*r = best->ord[best->pos++];
	m->chunk[r->chunk].pins++;
	if (best->pos == best->n)
		load_chunk(m, best);
	if (best->done)
		m->heap[0] = m->heap[--m->nheap];
	if (m->nheap > 0)
		heap_down(m, 0);
	return true;
}

static void
merge_done(void *state, const RowRef *refs, uint64 n)
{
	MergeSrc   *m = state;

	for (uint64 i = 0; i < n; i++)
	{
		m->chunk[refs[i].chunk].pins--;
		chunk_free(m, refs[i].chunk);
	}
}

/*
 * The merge of segments of an index with key_cut: the live rows of the inputs, in the order of the key, cut again into units. The units of a key-cut
 * segment follow each other in the order of the key, so each input holds one unit and the cut holds a window of rows: the memory does not depend on the number of rows.
 * An input that stream does not take as one is cut another way (a segment written before the option, or by another key): its rows are loaded whole and sorted when they fit
 * memory_kb, else sorted together by a sort that spills to temporary files (order_sort.c), which is read as one more input. Returns the number of units.
 */
int
sift_colk_merge(SiftColSegWriter *w, const SiftSegEntry *ent, SiftColSegment *const *seg, int n, SiftColFreezeFn freeze, void *freeze_arg, const SiftColSchema *schema,
				 const int *leaf, int nleaf, uint32 unit_rows, uint64 memory_kb, MemoryContext cx, SiftColCutFn cutfn, void *cutarg, int slice, int nslices, uint32 unit_base, const bool *stream, const SiftColWindow *win, SiftColSorter *sorted, SiftMemTracker *inputs)
{
	OrderCtx	ctx = {schema, leaf, nleaf};
	MergeSrc	m = {.in = palloc0_array(MergeIn, n + 1), .n = n, .ctx = &ctx, .freeze = freeze, .freeze_arg = freeze_arg, .cx = cx,
					 .scratch = AllocSetContextCreate(cx, "sift_col merge unit", ALLOCSET_DEFAULT_SIZES), .budget = memory_kb * 1024, .inputs = inputs, .free_head = -1};
	BoundKey	wlo,
				whi;
	uint64		loose = 0;
	int			units;

	for (int i = 0; i < n; i++)
		m.in[i] = (MergeIn) {.seg = seg[i], .gen = ent[i].gen, .keyed = stream[i], .chunk = -1, .bounded = win ? win->bounded[i] : true};
	if (win)
	{
		Assert(nleaf > 0 && nslices == 1);
		if (win->has_lo)
		{
			sift_colk_bound_unpack(&ctx, &win->lo, &wlo);
			m.lo = &wlo;
		}
		if (win->has_hi)
		{
			sift_colk_bound_unpack(&ctx, &win->hi, &whi);
			m.hi = &whi;
		}
	}
	if (nslices > 1)
	{
		uint64		nb;
		BoundKey   *bk = sift_colk_slice_bounds(&ctx, seg, n, &nb);

		if (nb == 0)
			return 0;
		m.lo = slice > 0 ? sift_colk_slice_bound(bk, nb, slice, nslices, SIFT_SLICE_SLACK_PCT, NULL) : NULL;
		m.hi = slice < nslices - 1 ? sift_colk_slice_bound(bk, nb, slice + 1, nslices, SIFT_SLICE_SLACK_PCT, NULL) : NULL;
	}
	/* the rows of the inputs that are not streams are loaded whole and sorted when they fit the budget, else sorted by a sort that spills to temporary files */
	for (int i = 0; i < n; i++)
		if (!stream[i])
			loose += sift_cseg_live_rows(seg[i]);
	if (sorted != NULL || loose * sift_colk_row_bytes(schema) > m.budget)
	{
		m.sorter = sorted != NULL ? sorted : sift_colsort_begin(schema, leaf, nleaf, (int) Min(memory_kb / 2, (uint64) INT_MAX));
		m.presorted = sorted != NULL;
		m.in[m.n++] = (MergeIn) {.sorted = true, .chunk = -1};
	}
	units = cut_rows(w, NULL, 0, merge_next, merge_done, &m, schema, &ctx, unit_rows, cx, cutfn, cutarg, unit_base);
	MemoryContextDelete(m.scratch);
	return units;
}
