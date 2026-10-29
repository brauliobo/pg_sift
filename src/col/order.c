/*
 * col/order.c - key-cut units: the rows of a build, a promotion or a merge are sorted by the declared order key (the option key_cut) and cut
 * into units of a range of the key, whatever their heap pages. Each unit lists its TIDs (enc.c, unit.c).
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_class.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "core/cmp.h"
#include "col/am.h"
#include "col/enc.h"
#include "col/pg.h"
#include "col/runs.h"
#include "col/seg.h"

#define ORDER_DEFAULT_ROWS	1024
#define ORDER_MAX_ROWS		32768		/* the most rows of a unit (key_cut_rows); the directory of a unit is read with the head of its record (seg.c sift_cseg_unit_head) */

typedef struct RowRef
{
	const SiftColBatch *b;
	uint32		r;
	uint32		chunk;			/* a merge: the chunk of its input that b is, to unpin when the row has left the window */
} RowRef;

typedef struct OrderCtx
{
	const SiftColSchema *schema;
	const int  *leaf;
	int			nleaf;
} OrderCtx;

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

uint64
sift_colk_row_bytes(const SiftColSchema *schema)
{
	return 32 + (uint64) schema->ncols * 16;
}

/*
 * Rows a key-cut unit aims at: the rows of 256 average heap pages (pg_class: ANALYZE or VACUUM keeps reltuples and relpages) or the option, so that the number of
 * units stays what the superblock cut gave and the planner's per-unit costs are as before; 0 when unknown. A unit has at most ORDER_MAX_ROWS of them.
 */
uint32
sift_colk_aim_rows(Relation index)
{
	uint32		n = sift_col_rel(index)->cut_rows;
	HeapTuple	tp;

	if (n == 0 && (tp = SearchSysCache1(RELOID, ObjectIdGetDatum(index->rd_index->indrelid))) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tp);

		if (c->relpages > 0 && c->reltuples > 0)
			n = (uint32) Min((double) SIFT_COL_UNIT_MAX_ROWS, (double) SIFT_SB_PAGES * c->reltuples / c->relpages);
		ReleaseSysCache(tp);
	}
	return n;
}

/* The superblocks of the heap as of its last VACUUM or ANALYZE, which a merge compares its rows with to tell whether they are sparse (0 without statistics) */
uint64
sift_colk_heap_sbs(Relation index)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(index->rd_index->indrelid));
	uint64		sbs = 0;

	if (tp != NULL)
	{
		sbs = Max(((Form_pg_class) GETSTRUCT(tp))->relpages, 0) / SIFT_SB_PAGES;
		ReleaseSysCache(tp);
	}
	return sbs;
}

/* The rows of a unit for an aim (0: unknown) */
uint32
sift_colk_unit_rows(uint32 aim)
{
	return aim == 0 ? ORDER_DEFAULT_ROWS : Min(Max(aim, 64), ORDER_MAX_ROWS);
}

/* The aim for the superblock batches of a build run: the statistics of the heap, or without them (a table never analyzed) the rows a superblock holds in the run */
uint32
sift_colk_run_rows(Relation index, SiftColBatch *const *in, int n)
{
	uint32		aim = sift_colk_aim_rows(index);
	uint64		rows = 0;

	if (aim > 0 || n == 0)
		return aim;
	for (int i = 0; i < n; i++)
		rows += in[i]->nrows;
	return (uint32) Min(rows / n, (uint64) SIFT_COL_UNIT_MAX_ROWS);
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
	int64		k;
	SiftKey		tid;
	RowRef		ref;
	bool		null;
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
		int			r = key_cmp_from(&x->ref, &y->ref, c, 1, c->nleaf);

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
	SortEnt    *e = palloc_array(SortEnt, n);

	for (uint64 i = 0; i < n; i++)
	{
		const RowRef *x = &refs[i];
		e[i].ref = *x;
		e[i].tid = x->b->keys[x->r];
		e[i].null = ctx->nleaf > 0 && ref_null(x, ctx->leaf[0]);
		e[i].k = ctx->nleaf == 0 || e[i].null ? 0 : sift_crun_key(&ctx->schema->cols[ctx->leaf[0]], x->b->col[ctx->leaf[0]].key[x->r]);
	}
	sort_ents(e, n, (OrderCtx *) ctx);
	for (uint64 i = 0; i < n; i++)
		refs[i] = e[i].ref;
	pfree(e);
}

#define ST_SORT sort_by_tid
#define ST_ELEMENT_TYPE RowRef
#define ST_COMPARE(p, q) SIFT_CMP3((p)->b->keys[(p)->r], (q)->b->keys[(q)->r])
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

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
		 MemoryContext cx, SiftColCutFn cutfn, void *cutarg)
{
	uint64		slack = unit_rows / 4;
	uint64		want = unit_rows + slack + 1;
	SiftColBatch *out = sift_cbatch_create(cx, schema);
	RowRef	   *win = refs ? NULL : palloc_array(RowRef, want);
	uint64		n = 0,
				pos = 0;
	uint32		seq = 0,
				num = 0;
	bool		eof = false,
				split = false;

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
			while (!eof && n < want)
				if (next(state, &win[n]))
					n++;
				else
					eof = true;
			avail = n;
			u = win;
		}
		if (avail == 0)
			break;
		end = cut_at(ctx, u, avail, unit_rows, slack, &split);
		sort_by_tid(u, end);
		/*
		 * The number of a unit is the sequence of the cut by a key. Without a key the units are in the order of the TIDs and take the superblock of their first
		 * row (or the one after the unit before it), so that a walk of the superblocks meets an overlay next to the units of the heap it overlays
		 */
		if (ctx->nleaf == 0)
			num = Max(num + (seq > 0), sift_key_block(u[0].b->keys[u[0].r]) / SIFT_SB_PAGES);
		else
			num = seq;
		sift_cbatch_reset(out, num);
		seq++;
		out->keyed = true;
		for (uint64 i = 0; i < end; i++)
			sift_cbatch_add_from(out, u[i].b, u[i].r);
		if (w == NULL)
			w = cutfn(cutarg, NULL);
		sift_colw_add(w, out);
		if (cutfn)
			w = cutfn(cutarg, w);
		if (refs)
			pos += end;
		else
		{
			if (done)
				done(state, u, end);
			memmove(win, win + end, (n - end) * sizeof(RowRef));
			n -= end;
		}
		CHECK_FOR_INTERRUPTS();
	}
	return seq;
}

/*
 * Sorts the rows of the batches by the leaves of the order key and adds them to w as units (cut_rows). Returns the number of units. Memory: one
 * reference per row, and the batches themselves; sift_colk_merge is the form that holds a unit of each input.
 */
int
sift_colk_write(SiftColSegWriter *w, SiftColBatch *const *in, int nin, const SiftColSchema *schema, const int *leaf, int nleaf, uint32 unit_rows,
				MemoryContext cx)
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
	refs = palloc_array(RowRef, total);
	for (int i = 0; i < nin; i++)
		for (uint32 r = 0; r < in[i]->nrows; r++)
			refs[k++] = (RowRef) {in[i], r};
	sort_refs(refs, total, &ctx);
	units = cut_rows(w, refs, total, NULL, NULL, NULL, schema, &ctx, unit_rows, cx, NULL, NULL);
	pfree(refs);
	return units;
}

/* One input of a merge: a key-cut segment, whose units follow each other in the order of the key, or an older one, which is loaded and sorted whole */
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
} MergeIn;

/* The rows of a chunk stay where they were decoded until the window of the cut has copied them into a unit: a chunk is reused when no row of it is pinned by the window and its input has loaded another */
typedef struct MergeChunk
{
	SiftColBatch *b;
	uint32		pins;
	bool		held;			/* the loaded chunk of its input */
} MergeChunk;

typedef struct MergeSrc
{
	MergeChunk *chunk;
	int			nchunk;
	MergeIn    *in;
	int			n;
	const OrderCtx *ctx;
	SiftColFreezeFn freeze;
	void	   *freeze_arg;
	MemoryContext scratch;
	uint64		budget;
	uint64		loaded;			/* the rows of the older segments loaded whole */
	MemoryContext cx;
	MergeIn   **heap;			/* the inputs with rows left, the one whose next row comes first at 0 */
	int			nheap;
} MergeSrc;

static int
chunk_get(MergeSrc *m, const SiftColSchema *schema)
{
	for (int i = 0; i < m->nchunk; i++)
		if (m->chunk[i].pins == 0 && !m->chunk[i].held)
			return i;
	m->chunk = m->chunk ? repalloc(m->chunk, (m->nchunk + 1) * sizeof(MergeChunk)) : MemoryContextAlloc(m->cx, sizeof(MergeChunk));
	m->chunk[m->nchunk] = (MergeChunk) {.b = sift_cbatch_create(m->cx, schema)};
	return m->nchunk++;
}

static void
load_chunk(MergeSrc *m, MergeIn *in)
{
	SiftColSegment *seg = in->seg;
	const SiftColSchema *schema = seg->schema;
	MemoryContext old;
	int			prev = in->chunk;
	int			cur = chunk_get(m, schema);
	SiftColBatch *b = m->chunk[cur].b;

	sift_cbatch_reset(b, 0);
	old = MemoryContextSwitchTo(m->scratch);
	if (in->keyed)
	{
		while (b->nrows == 0 && in->next_unit < seg->nsb)
			sift_cseg_decode_unit_freeze(seg, in->next_unit++, b, m->freeze, m->freeze_arg);
	}
	else
	{
		SiftColBatch *one = sift_cbatch_create(m->cx, schema);

		b->keyed = true;		/* the rows of many superblocks */
		m->loaded += sift_cseg_live_rows(seg);
		if (m->loaded * sift_colk_row_bytes(schema) > m->budget)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift_col merge of an index with key_cut holds the rows of the segments written before the option in memory and needs more than maintenance_work_mem"),
							errhint("Raise maintenance_work_mem, or REINDEX the index.")));
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
	if (in->n == 0)
		in->done = true;
	else
	{
		if (in->ordcap < in->n)
		{
			in->ordcap = Max(in->n, 2 * in->ordcap);
			in->ord = in->ord ? repalloc(in->ord, in->ordcap * sizeof(RowRef)) : MemoryContextAlloc(m->cx, in->ordcap * sizeof(RowRef));
		}
		for (uint32 r = 0; r < in->n; r++)
			in->ord[r] = (RowRef) {b, r, cur};
		sort_refs(in->ord, in->n, m->ctx);
		if (in->keyed && prev >= 0 && m->chunk[prev].b->nrows > 0)
		{
			RowRef		last = {m->chunk[prev].b, m->chunk[prev].b->nrows - 1};

			/* the units of a segment follow each other in the order of the key it was cut by: another key (the option was changed) cannot be merged as a stream */
			if (ref_cmp(&last, &in->ord[0], (void *) m->ctx) > 0)
				ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("segment %llu of a key_cut index is not in the order of the key", (unsigned long long) in->gen),
								errhint("REINDEX the index.")));
		}
	}
	m->chunk[cur].held = !in->done;
	if (prev >= 0)
		m->chunk[prev].held = false;
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

static bool
merge_next(void *state, RowRef *r)
{
	MergeSrc   *m = state;
	MergeIn    *best;

	if (m->heap == NULL)
	{
		m->heap = MemoryContextAlloc(m->cx, m->n * sizeof(MergeIn *));
		for (int i = 0; i < m->n; i++)
		{
			load_chunk(m, &m->in[i]);
			if (!m->in[i].done)
				m->heap[m->nheap++] = &m->in[i];
		}
		for (int i = m->nheap / 2 - 1; i >= 0; i--)
			heap_down(m, i);
	}
	if (m->nheap == 0)
		return false;
	best = m->heap[0];
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
		m->chunk[refs[i].chunk].pins--;
}

/*
 * The merge of segments of an index with key_cut: the live rows of the inputs, in the order of the key, cut again into units. The units of a key-cut
 * segment follow each other in the order of the key, so each input holds one unit (a segment written before the option is loaded whole and sorted, up
 * to memory_kb) and the cut holds a window of rows: the memory does not depend on the number of rows. Returns the number of units.
 */
int
sift_colk_merge(SiftColSegWriter *w, const SiftSegEntry *ent, SiftColSegment *const *seg, int n, SiftColFreezeFn freeze, void *freeze_arg, const SiftColSchema *schema,
				 const int *leaf, int nleaf, uint32 unit_rows, uint64 memory_kb, MemoryContext cx, SiftColCutFn cutfn, void *cutarg)
{
	OrderCtx	ctx = {schema, leaf, nleaf};
	MergeSrc	m = {.in = palloc0_array(MergeIn, n), .n = n, .ctx = &ctx, .freeze = freeze, .freeze_arg = freeze_arg, .cx = cx,
					 .scratch = AllocSetContextCreate(cx, "sift_col merge unit", ALLOCSET_DEFAULT_SIZES), .budget = memory_kb * 1024};
	int			units;

	for (int i = 0; i < n; i++)
		m.in[i] = (MergeIn) {.seg = seg[i], .gen = ent[i].gen, .keyed = nleaf == 0 || sift_cseg_keyed(&ent[i]), .chunk = -1};
	units = cut_rows(w, NULL, 0, merge_next, merge_done, &m, schema, &ctx, unit_rows, cx, cutfn, cutarg);
	MemoryContextDelete(m.scratch);
	return units;
}
