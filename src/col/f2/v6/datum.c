/*
 * col/f2/v6/datum.c - the datums of a column of the scan of an index or sift_table of format major 2 whose type no native vector holds (agg/v6/datum.h): a NaN, an infinity or a
 * numeric of any digits, money, time, an enum, an interval, a uuid, bytea, a network type, an array or range, char(n), name and every other type are the values they are, in SVEC_DATUM.
 *
 * A sift_col index keeps the value in the lanes of its leaves (col/api/leaf.h), a sift_table keeps the datum of the attribute in one cell.
 */
#include "postgres.h"

#include "utils/builtins.h"
#include "utils/numeric.h"

#include "col/am.h"
#include "col/f2/store/schema.h"
#include "col/f2/v6/scan_int.h"
#include "table/f2/blob.h"

/* the NULL of a numeric that carries a tag is its NaN, the one value a scaled integer or an image cannot hold */
static bool
nan_cell(const ColView *cv, int r)
{
	return cv->tag != NULL && cv->tag[r] != 0;
}

static Datum
nan_numeric(void)
{
	return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

static SiftColValue
leaf_value(const ColView *cv, int r)
{
	SiftColValue v = {.isnull = cv->isnull[r],.tag = cv->tag != NULL ? cv->tag[r] : 0};

	if (v.isnull)
		return v;
	switch (cv->kind)
	{
		case SIFT_CELL_INT:
			v.v.i = cv->i[r];
			break;
		case SIFT_CELL_F8:
			v.v.f = cv->f[r];
			break;
		default:
			v.v.s = cv->s[r];
			v.len = cv->slen[r];
	}
	return v;
}

static bool
index_datum(const V6Col *c, const ColView *views, int r, MemoryContext cx, Datum *d)
{
	SiftColValue v[SIFT_COL_MAX_LANES];
	const int	lanes = sift_col_def_value_lanes(c->def);

	for (int k = 0; k < lanes; k++)
		v[k] = leaf_value(&views[c->leaf + k], r);
	if (v[0].isnull)
	{
		if (c->def->type != SIFT_COLT_NUMERIC || !nan_cell(&views[c->leaf], r))
			return false;
		*d = nan_numeric();
		return true;
	}
	*d = sift_col_values_datum(c->def, c->typid, c->typlen, c->typbyval, v, cx);
	return true;
}

static Datum
int_datum(const V6Col *c, int64 i)
{
	if (c->def->type == SIFT_COLT_NUMERIC)
		return sift_col_key_numeric(c->def->scale, i);
	switch (c->typlen)
	{
		case 1:
			return CharGetDatum((char) i);
		case 2:
			return Int16GetDatum((int16) i);
		case 4:
			return Int32GetDatum((int32) i);
		default:
			return Int64GetDatum(i);
	}
}

static bool
table_datum(const V6Col *c, const ColView *cv, int r, MemoryContext cx, Datum *d)
{
	if (cv->isnull[r])
	{
		if (c->typid != NUMERICOID || !nan_cell(cv, r))
			return false;
		*d = nan_numeric();
		return true;
	}
	switch (cv->kind)
	{
		case SIFT_CELL_INT:
			*d = int_datum(c, cv->i[r]);
			break;
		case SIFT_CELL_F8:
			*d = c->typid == FLOAT4OID ? Float4GetDatum((float4) cv->f[r]) : Float8GetDatum(cv->f[r]);
			break;
		default:
			if (c->bare && !(cv->tag != NULL && cv->tag[r] == SIFT_COLTAG_WIDE_BLOB))
				*d = sift_f2_bare_datum_of(c->typid, cv->s[r], cv->slen[r], cx);
			else
				*d = PointerGetDatum(memcpy(MemoryContextAlloc(cx, cv->slen[r]), cv->s[r], cv->slen[r]));
	}
	return true;
}

void
sift_f2_v6_fill_datum(V6Col *c, const ColView *views, int off, int n, MemoryContext cx, const uint64 *mask)
{
	SiftVec    *v = &c->vec;
	Datum	   *out = v->data;
	const ColView *head = &views[c->leaf];

	memset(c->valid, 0, sizeof(c->valid));
	for (int w = 0; w < (n + 63) / 64; w++)
		for (uint64 bits = mask != NULL ? mask[w] : ~UINT64CONST(0); bits != 0; bits &= bits - 1)
		{
			const int	i = w * 64 + __builtin_ctzll(bits);

			if (i >= n)
				break;
			if (c->is_table ? table_datum(c, head, off + i, cx, &out[i]) : index_datum(c, views, off + i, cx, &out[i]))
				c->valid[w] |= UINT64CONST(1) << (i & 63);
		}
	v->type = SVEC_DATUM;
	v->valid = c->valid;
	v->nan = NULL;
	v->all_valid = false;
	v->len = n;
	v->dense = true;
	v->sel = NULL;
}
