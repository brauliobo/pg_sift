/*
 * col/v1/rowpack.c - a row of a column batch as bytes and back (rowpack.h)
 * A row is its TID, a bitmap, then the values. A value is its bytes: an integer as a zigzag varint, a float as its 8 bytes, a string as its length (a varint)
 * and its bytes. A value that is not a plain one (NULL, overflow, a second integer, a tag) has a control byte before its bytes; the bitmap says which leaves
 * have them: a bit for each top level leaf, and for each leaf of the elements of an array a bit for the array being there and a bit for the control bytes
 * of its elements.
 */
#include "postgres.h"
#include "col/v1/rowpack.h"
#include "core/varint.h"

#define PK_NULL		1
#define PK_OVERFLOW	2
#define PK_LO		4
#define PK_TAG		3				/* the shift of the tag, at most SIFT_COLTAG_UNSURE: 4 bits */
#define PK_VARINT_LEN	10

typedef struct PackShape
{
	int			ntop;			/* the leaves of a row, then the leaves of the elements: the bits of the bitmap are the control bits of the first, then the presence bits and the control bits of the second */
	int			nchild;
} PackShape;

static PackShape
pack_shape(const SiftColSchema *sc)
{
	PackShape	s = {0, 0};

	for (int c = 0; c < sc->ncols; c++)
		sc->cols[c].parent < 0 ? s.ntop++ : s.nchild++;
	return s;
}

static int
bitmap_bytes(PackShape s)
{
	return (s.ntop + 2 * s.nchild + 7) / 8;
}

static bool
bit_get(const uint8 *bm, int bit)
{
	return bm[bit / 8] >> (bit % 8) & 1;
}

static void
bit_set(uint8 *bm, int bit)
{
	bm[bit / 8] |= 1 << (bit % 8);
}

/* the values of a leaf that are bytes (v.s, len), not a number */
static bool
is_bytes(const SiftColDef *def)
{
	return def->store == SIFT_COLS_STR || def->store == SIFT_COLS_VAR;
}

static bool
is_plain(const SiftColValue *v)
{
	return !v->isnull && !v->overflow && v->lo == 0 && v->tag == 0;
}

static uint8
control(const SiftColValue *v)
{
	return (v->isnull ? PK_NULL : 0) | (v->overflow ? PK_OVERFLOW : 0) | (v->lo != 0 ? PK_LO : 0) | v->tag << PK_TAG;
}

static const char *
get_varint(const char *p, uint64 *x)
{
	p = (const char *) sift_get_varint64_n((const uint8 *) p, (const uint8 *) p + PK_VARINT_LEN, x);
	SIFT_INVARIANT(p != NULL, "sift_col: a packed row holds a varint that does not end");
	return p;
}

static void
put_val(StringInfo out, const SiftColDef *def, const SiftColValue *v, bool with_control)
{
	uint8	   *p;
	uint8	   *start;

	enlargeStringInfo(out, 1 + 2 * PK_VARINT_LEN + (is_bytes(def) ? v->len : sizeof(v->v)));
	start = p = (uint8 *) out->data + out->len;
	if (with_control)
		*p++ = control(v);
	if (!v->isnull)
	{
		if (is_bytes(def))
		{
			p = sift_put_varint(p, v->len);
			memcpy(p, v->v.s, v->len);
			p += v->len;
		}
		else if (def->store == SIFT_COLS_INT)
			p = sift_put_varint(p, sift_zigzag(v->v.i));
		else
		{
			memcpy(p, &v->v.f, sizeof(v->v.f));
			p += sizeof(v->v.f);
		}
	}
	if (v->lo != 0)
		p = sift_put_varint(p, sift_zigzag(v->lo));
	out->len += p - start;
	out->data[out->len] = '\0';
}

static const char *
get_val(const char *p, const SiftColDef *def, SiftColValue *v, bool with_control)
{
	uint8		ctl = with_control ? (uint8) *p++ : 0;
	uint64		x;

	memset(v, 0, sizeof(*v));
	v->isnull = ctl & PK_NULL;
	v->overflow = (ctl & PK_OVERFLOW) != 0;
	v->tag = ctl >> PK_TAG;
	if (!v->isnull)
	{
		if (is_bytes(def))
		{
			p = get_varint(p, &x);
			v->len = (uint32) x;
			v->v.s = p;
			p += v->len;
		}
		else if (def->store == SIFT_COLS_INT)
		{
			p = get_varint(p, &x);
			v->v.i = sift_unzigzag(x);
		}
		else
		{
			memcpy(&v->v.f, p, sizeof(v->v.f));
			p += sizeof(v->v.f);
		}
	}
	if (ctl & PK_LO)
	{
		p = get_varint(p, &x);
		v->lo = sift_unzigzag(x);
	}
	return p;
}

/* the leaf has a value that is not a plain one among the n of its elements */
static bool
any_irregular(const SiftColValue *v, int64 n)
{
	for (int64 i = 0; i < n; i++)
		if (!is_plain(&v[i]))
			return true;
	return false;
}

void
sift_colrow_pack(StringInfo out, const SiftColSchema *sc, const SiftColRow *row)
{
	PackShape	s = pack_shape(sc);
	int			nb = bitmap_bytes(s),
				bm_at,
				top = 0,
				child = 0;

	appendBinaryStringInfo(out, (const char *) &row->tid, sizeof(row->tid));
	bm_at = out->len;
	enlargeStringInfo(out, nb);
	memset(out->data + bm_at, 0, nb);
	out->len += nb;
	for (int c = 0; c < sc->ncols; c++)
		if (sc->cols[c].parent < 0)
		{
			const SiftColValue *v = &row->val[c];
			bool		irregular = !is_plain(v);

			if (irregular)
				bit_set((uint8 *) out->data + bm_at, top);
			put_val(out, &sc->cols[c], v, irregular);
			top++;
		}
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		int64		n;
		bool		irregular;

		if (def->parent < 0)
			continue;
		if (row->elem[c] != NULL)
		{
			n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;
			irregular = any_irregular(row->elem[c], n);
			bit_set((uint8 *) out->data + bm_at, s.ntop + child);
			if (irregular)
				bit_set((uint8 *) out->data + bm_at, s.ntop + s.nchild + child);
			for (int64 i = 0; i < n; i++)
				put_val(out, def, &row->elem[c][i], irregular);
		}
		child++;
	}
}

const char *
sift_colrow_unpack(const char *p, const SiftColSchema *sc, SiftColRow *row)
{
	PackShape	s = pack_shape(sc);
	const uint8 *bm;
	int			top = 0,
				child = 0;

	memcpy(&row->tid, p, sizeof(row->tid));
	p += sizeof(row->tid);
	bm = (const uint8 *) p;
	p += bitmap_bytes(s);
	row->ncols = sc->ncols;
	memset(row->val, 0, sc->ncols * sizeof(SiftColValue));
	memset(row->elem, 0, sc->ncols * sizeof(SiftColValue *));
	for (int c = 0; c < sc->ncols; c++)
		if (sc->cols[c].parent < 0)
			p = get_val(p, &sc->cols[c], &row->val[c], bit_get(bm, top++));
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];

		if (def->parent < 0)
			continue;
		if (bit_get(bm, s.ntop + child))
		{
			int64		n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;
			bool		irregular = bit_get(bm, s.ntop + s.nchild + child);

			row->elem[c] = palloc(Max(n, 1) * sizeof(SiftColValue));
			for (int64 i = 0; i < n; i++)
				p = get_val(p, def, &row->elem[c][i], irregular);
		}
		child++;
	}
	return p;
}
