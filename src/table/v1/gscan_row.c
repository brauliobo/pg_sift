/*
 * table/v1/gscan_row.c - the rows of the group scan: the values of an attribute, the attributes a reader wants, the tuple of a row
 */
#include "gscan_int.h"

/* Makes the window the one of the row at pos: the vectors of the leaves it held are read again on the first ask */
static void
win_move(SiftTblGroupScan *g, uint32 pos)
{
	MemoryContextReset(g->wincxt);
	for (int c = 0; c < g->s[g->cur].schema->ncols; c++)
		if (g->vwin[c])
		{
			g->vec[c].n = PG_UINT32_MAX;
			g->vwin[c] = false;
		}
	g->win_lo = pos / g->win_rows * g->win_rows;
	g->win_n = Min(g->win_rows, g->unit.nrows - g->win_lo);
	g->win_token = g->token;
}

/* The leaf c of the current unit with the row at pos, which is at *at in it: a window of rows, or every row of the unit for a leaf that is stored whole, read on first use */
static const SiftColVec *
col_vec(SiftTblGroupScan *g, int c, uint32 pos, uint32 *at)
{
	SiftColVec *vec = &g->vec[c];
	bool		windowed = vec->n == PG_UINT32_MAX ? sift_col_leaf_windowed(&g->unit, c) : g->vwin[c];

	if (windowed && (g->win_token != g->token || pos - g->win_lo >= g->win_n))
		win_move(g, pos);
	if (vec->n == PG_UINT32_MAX)
	{
		if (windowed)
			sift_col_read_rows(&g->unit, c, g->win_lo, g->win_n, vec, g->wincxt);
		else
		{
			MemoryContext old = MemoryContextSwitchTo(g->unitcxt);

			sift_col_read(&g->unit, c, NULL, vec);
			MemoryContextSwitchTo(old);
		}
		g->vwin[c] = windowed;
	}
	*at = windowed ? pos - g->win_lo : pos;
	return vec;
}

static bool
exc_find(const SiftColVec *ev, uint32 i, int attno, const char **p, uint32 *len)
{
	const char *b = ev->vbytes + ev->voff[i],
			   *end = ev->vbytes + ev->voff[i + 1];

	while (b < end)
	{
		uint16		a;

		memcpy(&a, b, sizeof(a));
		memcpy(len, b + sizeof(a), sizeof(*len));
		b += sizeof(a) + sizeof(*len);
		if (a == attno)
		{
			*p = b;
			return true;
		}
		b += *len;
	}
	return false;
}

/* The image of attribute attno of the row at pos in the exceptions of the unit */
static bool
exc_attr(SiftTblGroupScan *g, const GroupSrc *gs, uint32 pos, int attno, const char **p, uint32 *len)
{
	uint32		at;
	const SiftColVec *ev = col_vec(g, gs->exc, pos, &at);

	return exc_find(ev, at, attno, p, len);
}

/* Attribute a of the row at position pos of the vectors of the current unit, allocated in the current memory context */
static void
fill_attr(SiftTblGroupScan *g, uint32 pos, int a, Datum *value, bool *isnull)
{
	const GroupSrc *gs = &g->s[g->cur];
	Form_pg_attribute att = TupleDescAttr(g->desc, a - 1);
	int			leaf = gs->leaf[a];
	const SiftColVec *vec;
	uint32		at;

	*isnull = false;
	*value = (Datum) 0;
	if (att->attisdropped)
		*isnull = true;
	else if (leaf < 0)
	{
		*value = getmissingattr(g->desc, a, isnull);
		if (!*isnull)
			*value = datumCopy(*value, att->attbyval, att->attlen);	/* like every value: the caller owns it */
	}
	else if ((vec = col_vec(g, leaf, pos, &at))->nulls != NULL && sift_col_bit_test(vec->nulls, at))
	{
		const char *p;
		uint32		len;

		if (vec->ovf != NULL && sift_col_bit_test(vec->ovf, at) && exc_attr(g, gs, pos, a, &p, &len))
			*value = image_datum(att, p, len);
		else
			*isnull = true;
	}
	else
		*value = value_out(&gs->schema->cols[leaf], att, vec, at);
}

void
sift_tbl_gscan_want(SiftTblGroupScan *g, Bitmapset *want)
{
	g->want = want;
}

bool
sift_tbl_gscan_fill(SiftTblGroupScan *g, uint32 token, uint32 pos, int from, int to, Datum *values, bool *isnull)
{
	if (g->token != token)
		return false;
	for (int a = from + 1; a <= to; a++)
		if (g->want == NULL || bms_is_member(a - FirstLowInvalidHeapAttributeNumber, g->want))
			fill_attr(g, pos, a, &values[a - 1], &isnull[a - 1]);
		else
		{
			values[a - 1] = (Datum) 0;
			isnull[a - 1] = true;
		}
	if (g->park_fill)
		gscan_park(g);
	return true;
}

uint32
sift_tbl_gscan_cursor(const SiftTblGroupScan *g, ItemPointer tid, uint32 *pos)
{
	*tid = g->tids[g->i - 1];
	*pos = g->ord[g->i - 1];
	return g->token;
}

/* The tuple of row i of the vectors read (position i of the selection) */
HeapTuple
row_tuple(SiftTblGroupScan *g, uint32 i, ItemPointer tid)
{
	MemoryContext old = MemoryContextSwitchTo(g->rowcxt);
	HeapTuple	tup;

	for (int a = 1; a <= g->desc->natts; a++)
		fill_attr(g, i, a, &g->values[a - 1], &g->isnull[a - 1]);
	MemoryContextSwitchTo(old);
	tup = heap_form_tuple(g->desc, g->values, g->isnull);
	MemoryContextReset(g->rowcxt);
	tup->t_self = *tid;
	tup->t_tableOid = RelationGetRelid(g->rel);
	return tup;
}

HeapTuple
sift_tbl_gscan_tuple(SiftTblGroupScan *g)
{
	return row_tuple(g, g->ord[g->i - 1], &g->tids[g->i - 1]);
}
