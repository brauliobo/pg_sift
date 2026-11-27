/*
 * col/v1_engine/store_ops_heap.c - what the unified scan of Format 1 reads from the heap row of the rows a batch keeps because the leaves cannot hold it: a jsonb document, the value of a key
 * of a document, the float8 of a member of a map, the JSON type of each row of a key, and the ctid of a row the range of the statement bounds (col/v1/store_ops.c gives the batches).
 */
/* check-memory: allow-allocs every array here follows the columns of one scan or the rows of one batch, not the rows of the relation */
#include "postgres.h"

#include "access/tableam.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"

#include "agg/batch_source.h"
#include "agg/engine.h"
#include "col/v1/store_ops.h"
#include "col/v1/store_src.h"
#include "core/compat.h"

#define JT_NONE		5			/* the index of sift_v1_json_types of a path with no value */

/* the index of sift_v1_json_types of the type of the value the path of keys leads to in a document */
static uint8
doc_type(Datum doc, const List *path)
{
	JsonbContainer *c = &DatumGetJsonbP(doc)->root;
	JsonbValue	v = {0};

	foreach_node(String, key, path)
	{
		if (c == NULL || !JsonContainerIsObject(c) || getKeyJsonValueFromContainer(c, strVal(key), strlen(strVal(key)), &v) == NULL)
			return JT_NONE;
		c = v.type == jbvBinary ? v.val.binary.data : NULL;
	}
	switch (v.type)
	{
		case jbvString:
			return 0;
		case jbvNumeric:
			return 1;
		case jbvBool:
			return 2;
		case jbvBinary:
			return JsonContainerIsObject(c) ? 3 : 4;
		default:
			return 6;
	}
}

/* the type of the key of leaf l of the rows of the unit, from the document of each heap row; a row the snapshot does not see is cleared from bits */
static void
fetch_types(SiftV1Src *s, const SiftSrcLeaf *l, const uint32 *rows, int n, uint64 *bits, uint8 *tags)
{
	const AttrNumber att = sift_v1_att_heap_column(s->lay, l->attno);
	ItemPointerData tids[SIFT_VEC_LEN];

	s->rd->tids(s->src, &s->unit, rows, n, tids);
	for (int j = 0; j < n; j++)
	{
		const uint64 tag = (uint64) ItemPointerGetBlockNumber(&tids[j]) << 16 | ItemPointerGetOffsetNumber(&tids[j]);
		Datum		doc;
		bool		isnull;

		if (sift_v1_src_fetch(&s->base, tag, &att, 1, &doc, &isnull))
			tags[rows[j]] = isnull ? JT_NONE : doc_type(doc, l->path);
		else
			bits[rows[j] >> 6] &= ~(UINT64CONST(1) << (rows[j] & 63));
	}
}

/* the types of the rows of bits, for a unit whose segment does not tell them apart */
static void
heap_types(SiftV1Src *s, const SiftSrcLeaf *l, uint64 *bits, uint8 *tags)
{
	uint32		rows[SIFT_VEC_LEN];
	int			k = 0;

	for (uint32 r = 0; r < s->unit.nrow; r++)
	{
		if (!sift_src_bit(bits, r))
			continue;
		if (k == SIFT_VEC_LEN)
		{
			fetch_types(s, l, rows, k, bits, tags);
			k = 0;
		}
		rows[k++] = r;
	}
	fetch_types(s, l, rows, k, bits, tags);
}

bool
sift_v1_src_filter_types(SiftV1Src *s, const SiftSrcLeaf *l, uint32 types, uint64 *bits)
{
	const int	nw = SIFT_COL_NWORDS(s->unit.nrow);
	uint64	   *given = MemoryContextAlloc(s->unit_cx, nw * sizeof(uint64));
	uint8	   *tags;
	bool		known,
				left;

	memcpy(given, bits, nw * sizeof(uint64));
	left = s->rd->filter_types(s->src, &s->unit, l->id, types, bits, &known);
	if (known)
		return left;
	memcpy(bits, given, nw * sizeof(uint64));
	tags = MemoryContextAlloc(s->unit_cx, Max(s->unit.nrow, 1));
	heap_types(s, l, bits, tags);
	left = false;
	for (uint32 r = 0; r < s->unit.nrow; r++)
		if (sift_src_bit(bits, r) && !(sift_v1_json_types[tags[r]].test & types))
			bits[r >> 6] &= ~(UINT64CONST(1) << (r & 63));
		else
			left |= sift_src_bit(bits, r);
	return left;
}

/* the JSON type of each row of the unit the column i reads: the rows of one type at a time, which the reader answers for the stored rows and the values the leaf cannot hold, or the heap rows */
void
sift_v1_src_read_types(SiftV1Src *s, int i)
{
	const int	nw = SIFT_COL_NWORDS(s->unit.nrow);
	uint64	   *bits = MemoryContextAlloc(s->unit_cx, nw * sizeof(uint64));
	bool		known;

	s->jtag[i] = MemoryContextAllocZero(s->unit_cx, Max(s->unit.nrow, 1));
	for (int t = 0; t < SIFT_V1_NJSON_TYPES; t++)
	{
		memcpy(bits, s->bits, nw * sizeof(uint64));
		if (!s->rd->filter_types(s->src, &s->unit, s->jleaf[i]->id, sift_v1_json_types[t].test, bits, &known) && known)
			continue;
		if (!known)
		{
			heap_types(s, s->jleaf[i], s->bits, s->jtag[i]);
			return;
		}
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

/* the key of the document of the heap row of row r of the batch, whose value the leaf of column i could not hold (NULL: the row has no such key) */
static const Jsonb *
exception_member(SiftV1Src *s, int i, uint32 r)
{
	Datum		doc;
	bool		isnull;
	const bool	found = sift_v1_src_fetch(&s->base, s->tag[r], &s->jatt[i], 1, &doc, &isnull);

	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its document is read");
	return isnull ? NULL : sift_v1_json_member(doc, strVal(linitial(s->col_leaf[i]->path)));
}

static SiftV6Class
exception_class(const SiftV1Src *s, int i)
{
	return s->in.col[i].type == SVEC_F64 ? SV6C_F64 : SV6C_TEXT;
}

/* its value as `->>` reads it (and its float8 cast, or the float8 cast of the jsonb of a jsonb_cast column); a value whose cast raises raises at once, or is one of the raises of the batch when the operator raises them (raise_late) */
static void
put_exception(SiftV1Src *s, int i, uint32 r, const Jsonb *m)
{
	SiftV6JsonValue v;
	const SiftV6JsonExc x = sift_v6_json_exception_read(exception_class(s, i), s->in.col[i].jsonb_cast, (const char *) m, VARSIZE(m), s->batch_cx, &v);

	if (x == SV6JX_RAISES && !s->in.raise_late)
		sift_v6_json_exception_raise(exception_class(s, i), s->in.col[i].jsonb_cast, (const char *) m, VARSIZE(m), s->batch_cx);
	s->raises[r >> 6] |= (uint64) (x == SV6JX_RAISES) << (r & 63);
	s->has_raises |= x == SV6JX_RAISES;
	if (x != SV6JX_VALUE)
		return;
	if (s->in.col[i].type == SVEC_F64)
		((double *) s->data[i])[r] = DatumGetFloat8(v.value);
	else
		((SiftStrView *) s->data[i])[r] = sift_vstr_make(v.text, v.tlen);
	s->valid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
}

/* the rows the batch keeps whose leaf of a key could not hold its value, read from the heap row of each */
static void
fill_exceptions(SiftV1Src *s, SiftV6Batch *b)
{
	MemoryContext old = MemoryContextSwitchTo(s->batch_cx);

	memset(s->raises, 0, sizeof(s->raises));
	s->has_raises = false;
	for (int i = 0; i < s->in.ncols; i++)
		for (uint32 r = 0; s->has_jexc[i] && r < b->n; r++)
		{
			const Jsonb *m;

			if (sift_src_bit(s->jexc[i], r) && (b->mask == NULL || sift_src_bit(b->mask, r)) && (m = exception_member(s, i, r)) != NULL)
				put_exception(s, i, r, m);
		}
	b->raises = s->has_raises ? s->raises : NULL;
	MemoryContextSwitchTo(old);
}

void
sift_v1_src_raise(SiftV6Source *base, int row)
{
	SiftV1Src	   *s = (SiftV1Src *) base;
	SiftV6JsonValue v;

	for (int i = 0; i < s->in.ncols; i++)
	{
		const Jsonb *m = s->has_jexc[i] && sift_src_bit(s->jexc[i], row) ? exception_member(s, i, row) : NULL;

		if (m != NULL && sift_v6_json_exception_read(exception_class(s, i), s->in.col[i].jsonb_cast, (const char *) m, VARSIZE(m), s->batch_cx, &v) == SV6JX_RAISES)
			sift_v6_json_exception_raise(exception_class(s, i), s->in.col[i].jsonb_cast, (const char *) m, VARSIZE(m), s->batch_cx);
	}
	SIFT_INVARIANT(false, "row %d of a batch of relation %u has a value whose cast raises", row, s->in.indexoid);
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
sift_v1_src_fill_heap(SiftV1Src *s, SiftV6Batch *b)
{
	fill_docs(s, b);
	fill_exceptions(s, b);
	fill_members(s, b);
}

/* The heap row of tag into fslot: the version the snapshot sees of the chain an index entry names, or the row of a sift_table; false when it sees none */
bool
sift_v1_src_fetch_row(SiftV1Src *s, uint64 tag)
{
	ItemPointerData tid;
	bool		again = false,
				dead = false;

	ItemPointerSet(&tid, (BlockNumber) (tag >> 16), (OffsetNumber) (tag & 0xFFFF));
	return s->index_fetch != NULL ? table_index_fetch_tuple(s->index_fetch, &tid, s->snap, s->fslot, &again, &dead) :
		table_tuple_fetch_row_version(s->rels.heap, &tid, s->snap, s->fslot);
}

/*
 * Whether the heap row of tag has its ctid in [tid_lo, tid_hi). An index entry names the root of a HOT chain and the visible version is on the same page: the rows of the blocks between those
 * of tid_lo and tid_hi are in it, those of the two blocks are fetched
 */
static bool
tid_keeps(SiftV1Src *s, uint64 tag)
{
	const uint64 blk = tag >> 16;
	uint64		key;

	if (blk < s->tid_lo >> 16 || blk > s->tid_hi >> 16)
		return false;
	if ((blk > s->tid_lo >> 16 && blk < s->tid_hi >> 16) || s->index_fetch == NULL)
		return tag >= s->tid_lo && tag < s->tid_hi;
	if (!sift_v1_src_fetch_row(s, tag))
		return false;
	key = (uint64) ItemPointerGetBlockNumber(&s->fslot->tts_tid) << 16 | ItemPointerGetOffsetNumber(&s->fslot->tts_tid);
	return key >= s->tid_lo && key < s->tid_hi;
}

/* only the rows of the batch whose heap row's ctid is in the range of the scan */
bool
sift_v1_src_tid_narrow(SiftV1Src *s, SiftV6Batch *b)
{
	const int	words = (b->n + 63) / 64;
	uint64		left = 0;

	if (b->mask != s->mask)
	{
		memset(s->mask, 0xFF, words * sizeof(uint64));
		if (b->n & 63)
			s->mask[words - 1] = (UINT64CONST(1) << (b->n & 63)) - 1;
		if (b->mask != NULL)
			for (int w = 0; w < words; w++)
				s->mask[w] &= b->mask[w];
	}
	for (int r = 0; r < b->n; r++)
		if ((s->mask[r / 64] >> (r % 64) & 1) && !tid_keeps(s, b->tag[r]))
			s->mask[r / 64] &= ~(UINT64CONST(1) << (r % 64));
	for (int w = 0; w < words; w++)
		left |= s->mask[w];
	b->mask = s->mask;
	return left != 0;
}
