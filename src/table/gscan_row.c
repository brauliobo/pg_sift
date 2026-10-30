/*
 * table/gscan_row.c - the rows of the group scan: the values of an attribute, the attributes a reader wants, the tuple of a row
 */
#include "gscan_int.h"

/* The leaf c of the current unit with every row (in the unit's own order: cheaper to decode than a selection that leaves a few dead rows out), read on first use */
static const SiftColVec *
col_vec(SiftTblGroupScan *g, int c)
{
	if (g->vec[c].n == PG_UINT32_MAX)
	{
		MemoryContext old = MemoryContextSwitchTo(g->unitcxt);

		if (!sift_col_read_codes(&g->unit, c, &g->vec[c]))
			sift_col_read(&g->unit, c, NULL, &g->vec[c]);
		MemoryContextSwitchTo(old);
	}
	return &g->vec[c];
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

/* The text of a code of a leaf read as codes (sift_col_read_codes): its string is read from the dictionary of the segment in place */
static Datum
code_text(SiftTblGroupScan *g, Form_pg_attribute att, int leaf, uint32 code)
{
	char		buf[SIFT_COL_DICT_MAX_ENTRY];
	uint32		len;
	const char *s = sift_col_code_str(&g->unit, leaf, code, &len, buf);
	text	   *t;

	if (att->atttypid == UUIDOID)
		return image_datum(att, s, len);
	t = palloc(VARHDRSZ + len);
	SET_VARSIZE(t, VARHDRSZ + len);
	memcpy(VARDATA(t), s, len);
	return PointerGetDatum(t);
}

/* Attribute a of the row at position pos of the vectors of the current unit, allocated in the current memory context */
static void
fill_attr(SiftTblGroupScan *g, uint32 pos, int a, Datum *value, bool *isnull)
{
	const GroupSrc *gs = &g->s[g->cur];
	Form_pg_attribute att = TupleDescAttr(g->desc, a - 1);
	int			leaf = gs->leaf[a];
	const SiftColVec *vec;

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
	else if ((vec = col_vec(g, leaf))->nulls != NULL && sift_col_bit_test(vec->nulls, pos))
	{
		const char *p;
		uint32		len;

		if (vec->ovf != NULL && sift_col_bit_test(vec->ovf, pos) && exc_find(col_vec(g, gs->exc), pos, a, &p, &len))
			*value = image_datum(att, p, len);
		else
			*isnull = true;
	}
	else if (vec->store == SIFT_COLS_STR && vec->dict == NULL)
		*value = code_text(g, att, leaf, vec->codes[pos]);
	else
		*value = value_out(&gs->schema->cols[leaf], att, vec, pos);
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
