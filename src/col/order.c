/*
 * col/order.c - key-cut units: the rows of a build, a promotion or a merge are sorted by the declared order key (the option key_cut) and cut
 * into units of a range of the key, whatever their heap pages (docs/COL.md 2.16). Each unit lists its TIDs (enc.c, unit.c).
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
#define ORDER_MAX_ROWS		6144		/* the directory of a unit is read with the head of its record (SIFT_COL_UNIT_HEAD) */

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

static int
tid_cmp(const void *a, const void *b)
{
	SiftKey		x = ((const RowRef *) a)->b->keys[((const RowRef *) a)->r],
				y = ((const RowRef *) b)->b->keys[((const RowRef *) b)->r];

	return x < y ? -1 : x > y;
}

/*
 * Rows a key-cut unit aims at: the option, else the rows of 256 average heap pages (pg_class: ANALYZE or VACUUM keeps reltuples and relpages), so that the
 * number of units stays what the superblock cut gave and the planner's per-unit costs are as before.
 */
uint64
sift_colk_row_bytes(const SiftColSchema *schema)
{
	return 32 + (uint64) schema->ncols * 16;
}

uint32
sift_colk_unit_rows(Relation index)
{
	uint32		n = sift_col_rel(index)->cut_rows;
	HeapTuple	tp;

	if (n == 0 && (tp = SearchSysCache1(RELOID, ObjectIdGetDatum(index->rd_index->indrelid))) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tp);

		if (c->relpages > 0 && c->reltuples > 0)
			n = (uint32) Min((double) ORDER_MAX_ROWS, (double) SIFT_SB_PAGES * c->reltuples / c->relpages);
		ReleaseSysCache(tp);
	}
	return n == 0 ? ORDER_DEFAULT_ROWS : Min(Max(n, 64), ORDER_MAX_ROWS);
}

/*
 * Sorts the rows of the batches by the leaves of the order key and adds them to w as units of about unit_rows, each cut where the first leaf changes if that
 * is within a quarter of unit_rows of the count (so that one value of the leading leaf is in as few units as it can be), the rows of a unit in TID order.
 * Returns the number of units. Memory: one reference per row, and the batches themselves.
 */
int
sift_colk_write(SiftColSegWriter *w, SiftColBatch *const *in, int nin, const SiftColSchema *schema, const int *leaf, int nleaf, uint32 unit_rows,
				MemoryContext cx)
{
	OrderCtx	ctx = {schema, leaf, nleaf};
	uint64		total = 0,
				k = 0;
	RowRef	   *refs;
	SiftColBatch *out = sift_cbatch_create(cx, schema);
	uint32		seq = 0;

	for (int i = 0; i < nin; i++)
		total += in[i]->nrows;
	if (total == 0)
		return 0;
	refs = palloc_array(RowRef, total);
	for (int i = 0; i < nin; i++)
		for (uint32 r = 0; r < in[i]->nrows; r++)
			refs[k++] = (RowRef) {in[i], r};
	qsort_arg(refs, total, sizeof(RowRef), ref_cmp, &ctx);
	for (uint64 start = 0; start < total;)
	{
		uint64		end = Min(total, start + unit_rows),
					slack = unit_rows / 4;

		if (end < total)
		{
			uint64		best = 0;

			for (uint64 d = 0; d < slack + 1 && best == 0; d++)
			{
				if (end >= start + d + 1 && key_cmp(&refs[end - d - 1], &refs[end - d], &ctx, 1) != 0)
					best = end - d;
				else if (end + d < total && key_cmp(&refs[end + d - 1], &refs[end + d], &ctx, 1) != 0 && d > 0)
					best = end + d;
			}
			if (best > start)
				end = best;
		}
		qsort(refs + start, end - start, sizeof(RowRef), tid_cmp);
		sift_cbatch_reset(out, seq++);
		out->keyed = true;
		for (uint64 i = start; i < end; i++)
			sift_cbatch_add_from(out, refs[i].b, refs[i].r);
		sift_colw_add(w, out);
		CHECK_FOR_INTERRUPTS();
		start = end;
	}
	pfree(refs);
	return seq;
}
