/*
 * col/scan/point.c - the rows a scan lists by heap address or by key
 * point lists, the write buffer lookup, the stream of addresses
 * and the selection of the rows of a unit
 */
#include "postgres.h"
#include "col/scan_int.h"

/* The lists of the segments are ascending runs: merging the runs takes a few passes over the addresses where a sort takes n log n comparisons */
void
scan_ptid_merge_runs(SiftKey *a, uint32 n)
{
	SiftKey    *buf = palloc(n * sizeof(SiftKey)),
			   *from = a,
			   *to = buf,
			   *tmp;
	uint32	   *edge = palloc((n + 1) * sizeof(uint32));
	uint32		runs = 0;

	for (uint32 k = 0; k < n; k++)
		if (k == 0 || a[k] < a[k - 1])
			edge[runs++] = k;
	edge[runs] = n;
	while (runs > 1)
	{
		uint32		out = 0;

		for (uint32 r = 0; r < runs; r += 2)
		{
			uint32		i = edge[r],
						mid = edge[r + 1],
						j = mid,
						end = r + 1 < runs ? edge[r + 2] : mid,
						o = i;

			while (i < mid && j < end)
				to[o++] = from[i] <= from[j] ? from[i++] : from[j++];
			while (i < mid)
				to[o++] = from[i++];
			while (j < end)
				to[o++] = from[j++];
			edge[out++] = edge[r];
		}
		edge[out] = n;
		runs = out;
		tmp = from;
		from = to;
		to = tmp;
	}
	if (from != a)
		memcpy(a, from, n * sizeof(SiftKey));
	pfree(buf);
	pfree(edge);
}

const SiftKey *
sift_col_point_tids(const SiftColSource *src, uint32 *n)
{
	*n = src->nptid;
	return src->ptid;
}

/*
 * Can the rows of the write buffers an equality or IN predicate on leaf def matches be found in the records, without the batches and units a begin builds from them:
 * a plain integer-like, text or uuid leaf and a short list of values.
 */
bool
sift_col_wbuf_lookup_ok(const SiftColDef *def, const SiftColPred *p)
{
	if ((p->op != SIFT_COLOP_EQ && p->op != SIFT_COLOP_IN) || p->nvals < 1 || p->nvals > SIFT_CWBUF_LOOKUP_MAX)
		return false;
	if (def->parent >= 0 || def->kind != SIFT_COLK_SCALAR || def->lanes != 0 || def->lane != 0 || (def->flags & (SIFT_COLD_LOSSY | SIFT_COLD_ASTEXT)) ||
		sift_col_def_has_kinds(def))
		return false;
	switch (def->type)
	{
		case SIFT_COLT_INT2:
		case SIFT_COLT_INT4:
		case SIFT_COLT_INT8:
		case SIFT_COLT_DATE:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
		case SIFT_COLT_TEXT:
		case SIFT_COLT_UUID:
			return true;
		default:
			return false;
	}
}

/*
 * The heap addresses (in mcxt, each once) of the rows of the write buffers that p can match, for the heap to check; a source opened with SIFT_COLO_NOWBUF has loaded none of them.
 * The heap rejects the rows the other conditions do: a lookup that keeps more than a quarter of the buffers' rows (an IN list of the scope and no equality) narrowed nothing, so
 * the source loads the buffers, whose batches test every condition, and none is returned (before sift_col_begin).
 */
ItemPointerData *
sift_col_wbuf_lookup(SiftColSource *src, const SiftColPred *p, MemoryContext mcxt, uint32 *n)
{
	ItemPointerData *all = NULL;
	uint32		nall = 0;
	uint64		rows = 0;

	for (int i = 0; i < src->dir.nsegs; i++)
	{
		ItemPointerData *t;
		uint32		m;
		uint64		r;

		if (src->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			continue;
		t = sift_cwbuf_lookup(src->index, &src->dir.segs[i], src->schema, p, src->mcxt, &m, &r);
		rows += r;
		if (m > 0)
		{
			all = all ? repalloc(all, (nall + m) * sizeof(ItemPointerData)) : MemoryContextAlloc(mcxt, m * sizeof(ItemPointerData));
			memcpy(all + nall, t, m * sizeof(ItemPointerData));
			nall += m;
		}
	}
	if ((uint64) nall * 4 > rows)
	{
		src->flags &= ~SIFT_COLO_NOWBUF;
		nall = 0;
		all = NULL;
	}
	*n = nall;
	return all;
}

bool
sift_col_point_lazy(const SiftColSource *src)
{
	for (int i = 0; i < src->nseg; i++)
		if (src->seg[i].ps != NULL)
			return true;
	return false;
}

/* The addresses of the segments' lists, in the order of the segments, those of the units that are not read: the units with a dead row wait for the end of the segment's list */
uint32
sift_col_point_next(SiftColSource *src, SiftKey *out, uint32 max)
{
	uint32		n = 0;

	while (n < max && src->pstream < src->nseg)
	{
		ColCursor  *c = &src->seg[src->pstream];
		SiftColSegment *seg = c->seg;
		SiftKey		buf[128];
		uint32		got,
					sb = PG_UINT32_MAX,
					u = 0;
		int			mode = 0;		/* of the unit u: 0 not kept, 1 handed over, 2 read as a unit */

		if (c->ps == NULL)
		{
			src->pstream++;
			continue;
		}
		got = sift_cseg_point_stream_next(c->ps, buf, Min(max - n, lengthof(buf)));
		for (uint32 k = 0; k < got; k++)
		{
			uint32		b = sift_block_sb(sift_key_block(buf[k]));

			if (b != sb)
			{
				sb = b;
				u = sift_cseg_unit_of_sb(seg, sb);
				SIFT_INVARIANT(u != SIFT_CUNIT_UNKNOWN, "sift_col: the addresses of a segment cut by key name no unit");
				mode = u >= seg->nsb || !sift_col_bit_test(c->pkept, u) ? 0 : sift_cseg_dirty(seg, u) ? 2 : 1;
				if (mode == 2)
					sift_col_bit_set(c->ppend, u);
			}
			if (mode == 1)
				out[n++] = buf[k];
		}
		if (got == 0)
		{
			memcpy(c->keep, c->ppend, SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
			c->ps = NULL;
			src->pstream++;
		}
	}
	return n;
}

int
sift_col_point_sel(const SiftColUnit *unit, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	uint64	   *keep;
	uint64		base;

	if (cu->prow == NULL)
		return -1;
	keep = MemoryContextAllocZero(cu->src->arena, sel->nwords * sizeof(uint64));
	base = cu->seg->sbdir[cu->u].rank_base;
	for (uint32 k = 0; k < cu->nprow; k++)
	{
		uint32		r = (uint32) (cu->prow[k] - base);

		if (r < sel->n && sift_col_bit_test(sel->bits, r))
			sift_col_bit_set(keep, r);
	}
	memcpy(sel->bits, keep, sel->nwords * sizeof(uint64));
	sift_col_sel_count(sel);
	return cu->ppred;
}

void
sift_col_point_narrow(const SiftColUnit *unit, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	uint64	   *keep;

	if (cu->prow == NULL && cu->ptid == NULL)
		return;
	keep = MemoryContextAllocZero(cu->src->arena, sel->nwords * sizeof(uint64));
	if (cu->prow != NULL)
	{
		uint64		base = cu->seg->sbdir[cu->u].rank_base;

		for (uint32 k = 0; k < cu->nprow; k++)
		{
			uint32		r = (uint32) (cu->prow[k] - base);

			if (r < sel->n && sift_col_bit_test(sel->bits, r))
				sift_col_bit_set(keep, r);
		}
	}
	else
	{
		uint32	   *rows = palloc(Max(cu->nptid, 1) * sizeof(uint32));
		uint32		n = sift_cunit_find_rows(&cu->v, cu->sb, cu->ptid, cu->nptid, rows);

		for (uint32 k = 0; k < n; k++)
			if (rows[k] < sel->n && sift_col_bit_test(sel->bits, rows[k]))
				sift_col_bit_set(keep, rows[k]);
	}
	memcpy(sel->bits, keep, sel->nwords * sizeof(uint64));
	cu->src->cnt.point_units++;
	cu->src->cnt.point_rows += sift_col_sel_count(sel);
}

uint64
sift_col_point_exact(const SiftColUnit *unit)
{
	const ColUnit *cu = unit->priv;

	return cu->prow != NULL ? cu->pexact : 0;
}
