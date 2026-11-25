/*
 * col/v1_engine/store_ops_heap.c - what the unified scan of Format 1 reads from the heap row of the rows a batch keeps because the leaves cannot hold it: a jsonb document, the value of a key
 * of a document, the float8 of a member of a map, and the JSON type of each row of a key (col/v1/store_ops.c gives the batches).
 */
/* check-memory: allow-allocs every array here follows the columns of one scan or the rows of one batch, not the rows of the relation */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/builtins.h"

#include "agg/batch_source.h"
#include "col/v1/store_ops.h"
#include "col/v1/store_src.h"
#include "core/compat.h"

/* the JSON type of each row of the unit the column i reads: the rows of one type at a time, which the reader answers for the stored rows and the values the leaf cannot hold */
void
sift_v1_src_read_types(SiftV1Src *s, int i)
{
	const int	nw = SIFT_COL_NWORDS(s->unit.nrow);
	uint64	   *bits = MemoryContextAlloc(s->unit_cx, nw * sizeof(uint64));

	s->jtag[i] = MemoryContextAllocZero(s->unit_cx, Max(s->unit.nrow, 1));
	for (int t = 0; t < SIFT_V1_NJSON_TYPES; t++)
	{
		memcpy(bits, s->bits, nw * sizeof(uint64));
		if (!s->rd->filter_types(s->src, &s->unit, s->jleaf[i]->id, sift_v1_json_types[t].test, bits))
			continue;
		for (int w = 0; w < nw; w++)
			for (uint64 b = bits[w]; b != 0; b &= b - 1)
				s->jtag[i][w * 64 + pg_rightmost_one_pos64(b)] = (uint8) t;
	}
}

void
sift_v1_src_put_type(SiftV1Src *s, int i, uint32 r, int n)
{
	const char *name = sift_v1_json_types[s->jtag[i][r]].name;

	s->valid[i][n >> 6] |= (uint64) (name != NULL) << (n & 63);
	((SiftStrView *) s->data[i])[n] = name != NULL ? sift_vstr_make(name, strlen(name)) : (SiftStrView) {0};
}

/* the jsonb documents of the rows the batch keeps: each one as PostgreSQL reads it from the heap row the snapshot sees (a NULL one is not valid) */
static void
fill_docs(SiftV1Src *s, const SiftV6Batch *b)
{
	for (uint32 r = 0; s->ndocs > 0 && r < b->n; r++)
	{
		Datum		vals[SIFT_V6_MAX_COLS];
		bool		nulls[SIFT_V6_MAX_COLS];
		bool		found;

		if (b->mask != NULL && !sift_src_bit(b->mask, r))
			continue;
		found = sift_v1_src_fetch(&s->base, b->tag[r], s->doc_att, s->ndocs, vals, nulls);
		SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its document is read");
		for (int d = 0; d < s->ndocs; d++)
		{
			const int	i = s->doc_col[d];
			MemoryContext old = MemoryContextSwitchTo(s->batch_cx);

			if (!nulls[d])
			{
				s->valid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
				((Datum *) s->data[i])[r] = PointerGetDatum(PG_DETOAST_DATUM_COPY(vals[d]));
			}
			MemoryContextSwitchTo(old);
		}
	}
}

/* the value of the key of the document of a heap row the leaf of the key could not hold */
static void
put_exception(SiftV1Src *s, int i, uint32 r, Datum doc)
{
	const bool	f64 = s->in.col[i].type == SVEC_F64;
	const char *text = NULL;
	uint32		tlen = 0;
	double		f = 0;

	if (!sift_v1_json_member(doc, strVal(linitial(s->col_leaf[i]->path)), f64 ? SV6C_F64 : SV6C_TEXT, s->batch_cx, &text, &tlen, &f))
		return;
	if (f64)
		((double *) s->data[i])[r] = f;
	else
		((SiftStrView *) s->data[i])[r] = sift_vstr_make(text, tlen);
	s->valid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
}

/* the rows the batch keeps whose leaf of a key could not hold its value, read from the heap row of each */
static void
fill_exceptions(SiftV1Src *s, const SiftV6Batch *b)
{
	MemoryContext old = MemoryContextSwitchTo(s->batch_cx);

	for (int i = 0; i < s->in.ncols; i++)
		for (uint32 r = 0; s->has_jexc[i] && r < b->n; r++)
		{
			Datum		doc;
			bool		isnull;
			bool		found;

			if (!sift_src_bit(s->jexc[i], r) || (b->mask != NULL && !sift_src_bit(b->mask, r)))
				continue;
			found = sift_v1_src_fetch(&s->base, b->tag[r], &s->jatt[i], 1, &doc, &isnull);
			SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its document is read");
			if (!isnull)
				put_exception(s, i, r, doc);
		}
	MemoryContextSwitchTo(old);
}

/* the members of the rows the batch keeps whose leaves cannot hold them: as PostgreSQL reads them from the document of the heap row the snapshot sees */
static void
fill_members(SiftV1Src *s, const SiftV6Batch *b)
{
	for (int i = 0; i < s->in.ncols; i++)
		for (uint32 r = 0; s->col_mem[i] != NULL && r < b->n; r++)
		{
			Datum		doc;
			bool		isnull,
						found;
			double		v;

			if (!sift_src_bit(sift_v1_member_poison(s->col_mem[i]), r) || (b->mask != NULL && !sift_src_bit(b->mask, r)))
				continue;
			found = sift_v1_src_fetch(&s->base, b->tag[r], &s->mem_att[i], 1, &doc, &isnull);
			SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its document is read");
			if (!isnull && sift_v1_member_doc(s->col_mem[i], doc, &v))
			{
				((double *) s->data[i])[r] = v;
				s->valid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
			}
			else
				s->valid[i][r >> 6] &= ~(UINT64CONST(1) << (r & 63));
		}
}

void
sift_v1_src_fill_heap(SiftV1Src *s, const SiftV6Batch *b)
{
	fill_docs(s, b);
	fill_exceptions(s, b);
	fill_members(s, b);
}
