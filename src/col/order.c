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
key_cmp(const RowRef *x, const RowRef *y, const OrderCtx *c, int upto)
{
	for (int i = 0; i < upto; i++)
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

uint32
sift_colk_unit_rows(Relation index)
{
	uint32		n = sift_colk_aim_rows(index);

	return n == 0 ? ORDER_DEFAULT_ROWS : Min(Max(n, 64), ORDER_MAX_ROWS);
}

typedef bool (*RowNext) (void *state, RowRef *r);

static int
win_tid_cmp(const void *a, const void *b, void *arg)
{
	const SiftColBatch *w = arg;
	SiftKey		x = w->keys[*(const uint32 *) a],
				y = w->keys[*(const uint32 *) b];

	return x < y ? -1 : x > y;
}

/*
 * Cuts the rows of a stream, which arrives in the order of the key (and of the TID within a key), into units of about unit_rows, each cut where the
 * first leaf changes if that is within a quarter of unit_rows of the count (so that one value of the leading leaf is in as few units as it can be), the
 * rows of a unit in TID order. The rows wait in a window of unit_rows + unit_rows / 4 + 1 rows, which is the memory of the cut. Returns the number of units.
 */
static int
cut_stream(SiftColSegWriter *w, RowNext next, void *state, const SiftColSchema *schema, const OrderCtx *ctx, uint32 unit_rows, MemoryContext cx)
{
	uint64		slack = unit_rows / 4;
	uint64		want = unit_rows + slack + 1;
	SiftColBatch *win = sift_cbatch_create(cx, schema),
			   *spare = sift_cbatch_create(cx, schema),
			   *out = sift_cbatch_create(cx, schema);
	uint32	   *ord = palloc_array(uint32, want);
	uint32		seq = 0;
	bool		eof = false;

	win->keyed = spare->keyed = win->unsorted = spare->unsorted = true;

	for (;;)
	{
		RowRef		r;
		uint64		avail,
					end;

		while (!eof && win->nrows < want)
		{
			if (next(state, &r))
				sift_cbatch_add_from(win, r.b, r.r);
			else
				eof = true;
		}
		avail = win->nrows;
		if (avail == 0)
			break;
		end = Min(avail, unit_rows);
		if (end < avail)
		{
			uint64		best = 0;

			for (uint64 d = 0; d < slack + 1 && best == 0; d++)
			{
				RowRef		x = {win, 0},
							y = {win, 0};

				if (end >= d + 1)
				{
					x.r = end - d - 1;
					y.r = end - d;
					if (key_cmp(&x, &y, ctx, 1) != 0)
						best = end - d;
				}
				if (best == 0 && d > 0 && end + d < avail)
				{
					x.r = end + d - 1;
					y.r = end + d;
					if (key_cmp(&x, &y, ctx, 1) != 0)
						best = end + d;
				}
			}
			if (best > 0)
				end = best;
		}
		for (uint64 i = 0; i < end; i++)
			ord[i] = i;
		qsort_arg(ord, end, sizeof(uint32), win_tid_cmp, win);
		sift_cbatch_reset(out, seq++);
		out->keyed = true;
		for (uint64 i = 0; i < end; i++)
			sift_cbatch_add_from(out, win, ord[i]);
		sift_colw_add(w, out);
		sift_cbatch_reset(spare, 0);
		spare->keyed = spare->unsorted = true;
		for (uint64 i = end; i < avail; i++)
			sift_cbatch_add_from(spare, win, i);
		{
			SiftColBatch *t = win;

			win = spare;
			spare = t;
		}
		CHECK_FOR_INTERRUPTS();
	}
	pfree(ord);
	return seq;
}

typedef struct ArraySrc
{
	const RowRef *refs;
	uint64		n;
	uint64		i;
} ArraySrc;

static bool
array_next(void *state, RowRef *r)
{
	ArraySrc   *a = state;

	if (a->i == a->n)
		return false;
	*r = a->refs[a->i++];
	return true;
}

/*
 * Sorts the rows of the batches by the leaves of the order key and adds them to w as units (cut_stream). Returns the number of units. Memory: one
 * reference per row, and the batches themselves; sift_colk_merge is the form that holds a unit of each input.
 */
int
sift_colk_write(SiftColSegWriter *w, SiftColBatch *const *in, int nin, const SiftColSchema *schema, const int *leaf, int nleaf, uint32 unit_rows,
				MemoryContext cx)
{
	OrderCtx	ctx = {schema, leaf, nleaf};
	uint64		total = 0,
				k = 0;
	ArraySrc	src;
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
	qsort_arg(refs, total, sizeof(RowRef), ref_cmp, &ctx);
	src = (ArraySrc) {refs, total, 0};
	units = cut_stream(w, array_next, &src, schema, &ctx, unit_rows, cx);
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
	SiftColBatch *buf[2];		/* the loaded chunk and the one before it, whose last row the new one must not precede */
	int			cur;
	RowRef	   *ord;			/* the rows of the chunk in the order of the key */
	uint32		ordcap;
	uint32		n;
	uint32		pos;
	bool		done;
} MergeIn;

typedef struct MergeSrc
{
	MergeIn    *in;
	int			n;
	const OrderCtx *ctx;
	SiftColFreezeFn freeze;
	void	   *freeze_arg;
	MemoryContext scratch;
	uint64		budget;
	uint64		loaded;			/* the rows of the older segments loaded whole */
	MemoryContext cx;
} MergeSrc;

static void
load_chunk(MergeSrc *m, MergeIn *in)
{
	SiftColSegment *seg = in->seg;
	const SiftColSchema *schema = seg->schema;
	MemoryContext old;
	SiftColBatch *prev = in->buf[in->cur];
	uint32		prevn = in->n;
	SiftColBatch *b;

	in->cur ^= 1;
	b = in->buf[in->cur];
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
	in->n = b->nrows;
	in->pos = 0;
	if (in->n == 0)
	{
		in->done = true;
		return;
	}
	if (in->ordcap < in->n)
	{
		in->ordcap = Max(in->n, 2 * in->ordcap);
		in->ord = in->ord ? repalloc(in->ord, in->ordcap * sizeof(RowRef)) : MemoryContextAlloc(m->cx, in->ordcap * sizeof(RowRef));
	}
	for (uint32 r = 0; r < in->n; r++)
		in->ord[r] = (RowRef) {b, r};
	qsort_arg(in->ord, in->n, sizeof(RowRef), ref_cmp, (void *) m->ctx);
	if (in->keyed && prevn > 0)
	{
		RowRef		last = {prev, prevn - 1};

		/* the units of a segment follow each other in the order of the key it was cut by: another key (the option was changed) cannot be merged as a stream */
		if (ref_cmp(&last, &in->ord[0], (void *) m->ctx) > 0)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("segment %llu of a key_cut index is not in the order of the key", (unsigned long long) in->gen),
							errhint("REINDEX the index.")));
	}
}

static bool
merge_next(void *state, RowRef *r)
{
	MergeSrc   *m = state;
	MergeIn    *best = NULL;

	for (int i = 0; i < m->n; i++)
	{
		MergeIn    *in = &m->in[i];

		if (in->done)
			continue;
		if (in->pos == in->n)
			load_chunk(m, in);
		if (in->done)
			continue;
		if (best == NULL || ref_cmp(&in->ord[in->pos], &best->ord[best->pos], (void *) m->ctx) < 0)
			best = in;
	}
	if (best == NULL)
		return false;
	*r = best->ord[best->pos++];
	return true;
}

/*
 * The merge of segments of an index with key_cut: the live rows of the inputs, in the order of the key, cut again into units. The units of a key-cut
 * segment follow each other in the order of the key, so each input holds one unit (a segment written before the option is loaded whole and sorted, up
 * to memory_kb) and the cut holds a window of rows: the memory does not depend on the number of rows. Returns the number of units.
 */
int
sift_colk_merge(SiftColSegWriter *w, const SiftSegEntry *ent, SiftColSegment *const *seg, int n, SiftColFreezeFn freeze, void *freeze_arg, const SiftColSchema *schema,
				 const int *leaf, int nleaf, uint32 unit_rows, uint64 memory_kb, MemoryContext cx)
{
	OrderCtx	ctx = {schema, leaf, nleaf};
	MergeSrc	m = {.in = palloc0_array(MergeIn, n), .n = n, .ctx = &ctx, .freeze = freeze, .freeze_arg = freeze_arg, .cx = cx,
					 .scratch = AllocSetContextCreate(cx, "sift_col merge unit", ALLOCSET_DEFAULT_SIZES), .budget = memory_kb * 1024};
	int			units;

	for (int i = 0; i < n; i++)
	{
		m.in[i] = (MergeIn) {.seg = seg[i], .gen = ent[i].gen, .keyed = ent[i].format == SIFT_COL_FORMAT_KEYED,
							 .buf = {sift_cbatch_create(cx, schema), sift_cbatch_create(cx, schema)}};
		m.in[i].pos = m.in[i].n = 0;
	}
	units = cut_stream(w, merge_next, &m, schema, &ctx, unit_rows, cx);
	MemoryContextDelete(m.scratch);
	return units;
}
