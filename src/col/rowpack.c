/*
 * col/rowpack.c - a row of a column batch as bytes and back (rowpack.h)
 */
#include "postgres.h"
#include "col/rowpack.h"

/* the values of a leaf that are bytes (v.s, len), not a number */
static bool
is_bytes(const SiftColDef *def)
{
	return def->store == SIFT_COLS_STR || def->store == SIFT_COLS_VAR;
}

static void
put_val(StringInfo out, const SiftColDef *def, const SiftColValue *v)
{
	uint8		head[2] = {(v->isnull ? 1 : 0) | (v->overflow ? 2 : 0) | (v->lo != 0 ? 4 : 0), v->tag};

	appendBinaryStringInfo(out, (char *) head, 2);
	if (!v->isnull)
	{
		if (is_bytes(def))
		{
			appendBinaryStringInfo(out, (const char *) &v->len, sizeof(v->len));
			appendBinaryStringInfo(out, v->v.s, v->len);
		}
		else
			appendBinaryStringInfo(out, (const char *) &v->v, sizeof(v->v));
	}
	if (head[0] & 4)
		appendBinaryStringInfo(out, (const char *) &v->lo, sizeof(v->lo));
}

static const char *
get_val(const char *p, const SiftColDef *def, SiftColValue *v)
{
	uint8		head[2];

	memcpy(head, p, 2);
	p += 2;
	memset(v, 0, sizeof(*v));
	v->isnull = head[0] & 1;
	v->overflow = (head[0] & 2) != 0;
	v->tag = head[1];
	if (!v->isnull)
	{
		if (is_bytes(def))
		{
			memcpy(&v->len, p, sizeof(v->len));
			p += sizeof(v->len);
			v->v.s = p;
			p += v->len;
		}
		else
		{
			memcpy(&v->v, p, sizeof(v->v));
			p += sizeof(v->v);
		}
	}
	if (head[0] & 4)
	{
		memcpy(&v->lo, p, sizeof(v->lo));
		p += sizeof(v->lo);
	}
	return p;
}

void
sift_colrow_pack(StringInfo out, const SiftColSchema *sc, const SiftColRow *row)
{
	appendBinaryStringInfo(out, (const char *) &row->tid, sizeof(row->tid));
	for (int c = 0; c < sc->ncols; c++)
		if (sc->cols[c].parent < 0)
			put_val(out, &sc->cols[c], &row->val[c]);
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		uint8		has;

		if (def->parent < 0)
			continue;
		has = row->elem[c] != NULL;
		appendBinaryStringInfo(out, (char *) &has, 1);
		if (has)
		{
			int64		n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;

			for (int64 i = 0; i < n; i++)
				put_val(out, def, &row->elem[c][i]);
		}
	}
}

const char *
sift_colrow_unpack(const char *p, const SiftColSchema *sc, SiftColRow *row)
{
	memcpy(&row->tid, p, sizeof(row->tid));
	p += sizeof(row->tid);
	row->ncols = sc->ncols;
	memset(row->val, 0, sc->ncols * sizeof(SiftColValue));
	memset(row->elem, 0, sc->ncols * sizeof(SiftColValue *));
	for (int c = 0; c < sc->ncols; c++)
		if (sc->cols[c].parent < 0)
			p = get_val(p, &sc->cols[c], &row->val[c]);
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];

		if (def->parent < 0 || *p++ == 0)
			continue;
		{
			int64		n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;

			row->elem[c] = palloc(Max(n, 1) * sizeof(SiftColValue));
			for (int64 i = 0; i < n; i++)
				p = get_val(p, def, &row->elem[c][i]);
		}
	}
	return p;
}
