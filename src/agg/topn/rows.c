/*
 * agg/topn/rows.c - the heap of the best rows of a scan (agg/topn/rows.h).
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "common/int.h"
#include "executor/executor.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"

#include "agg/ht/ht.h"
#include "agg/group/int.h"
#include "agg/datum.h"
#include "agg/topn/rows.h"
#include "agg/topn/topk_run.h"
#include "core/compat.h"

#define SHORT_TEXT_MAX	126

struct SiftV6Rows
{
	const SiftV6Spec *spec;
	SiftV6TopKRun *run;
	Tuplesortstate *sort;		/* instead of run, for a K past SIFT_TOPK_MAX_K: every row the scan keeps, sorted, bounded by K */
	TupleTableSlot *in;			/* the row offered to the sort */
	TupleTableSlot *out;		/* the row it gives back */
	MemoryContext tmp;
	Datum		values[SIFT_V6_MAX_KEYS + 1];
	bool		isnull[SIFT_V6_MAX_KEYS + 1];
	Oid			base_typid[SIFT_V6_MAX_KEYS];	/* of each key: the type of a domain's values */
	char		sbuf[SIFT_V6_MAX_KEYS][SHORT_TEXT_MAX + 1];
	SiftF2Bound bound;
	bool		bounded;		/* the first sort column is an integer: the heap's worst row bounds what a granule must hold */
	bool		by_code;		/* the first sort column is a text in byte order: a dictionary lists its strings in that order, so the codes bound the rows as the strings do */
	bool		desc;
	uint64		source;			/* the partition being scanned, in the bits of its rows' addresses above SIFT_V6_ROWS_ADDR_BITS */
};

typedef struct RowCand
{
	const SiftV6Rows *r;
	SiftVec *const *keyvec;
	uint16		row;
	char		sbuf[SHORT_TEXT_MAX + 1];
} RowCand;

static const uint8 *
text_at(const SiftVec *v, uint16 row, uint32 *len)
{
	if (v->type == SVEC_DICT32)
	{
		const SiftGranuleDict *d = v->dict;
		const uint32 code = ((const uint32 *) v->data)[row];

		*len = d->off[code + 1] - d->off[code];
		return (const uint8 *) d->bytes + d->off[code];
	}
	*len = ((const SiftStrView *) v->data)[row].len;
	return sift_strview_bytes(&((const SiftStrView *) v->data)[row]);
}

static Datum
text_datum(const uint8 *p, uint32 len, char *sbuf)
{
	if (len > SHORT_TEXT_MAX)
		return PointerGetDatum(cstring_to_text_with_len((const char *) p, (int) len));
	SET_VARSIZE_SHORT(sbuf, len + VARHDRSZ_SHORT);
	memcpy(sbuf + VARHDRSZ_SHORT, p, len);
	return PointerGetDatum(sbuf);
}

static Datum
int_key_datum(Oid typid, int64 x)
{
	switch (typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) x);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) x);
		default:
			return Int64GetDatum(x);
	}
}

static Datum
key_datum(const SiftV6Rows *r, int k, const SiftVec *v, uint16 row, bool *isnull, char *sbuf)
{
	const SiftV6Col *c = &r->spec->key[k];

	if (v->type == SVEC_DICT32 || ((c->typid == TEXTOID || c->typid == VARCHAROID) && v->type == SVEC_STRVIEW))
	{
		uint32		len;
		const uint8 *p;

		*isnull = !is_valid(v, row);
		if (*isnull)
			return (Datum) 0;
		p = text_at(v, row, &len);
		return text_datum(p, len, sbuf);
	}
	return sift_v6_vec_datum(v, row, r->base_typid[k], c->scale, isnull);
}

static Datum
cand_value(void *ctx, int item, bool *isnull)
{
	RowCand    *c = ctx;
	const int	pos = c->r->spec->topk_item[item].pos;

	return key_datum(c->r, pos, c->keyvec[pos], c->row, isnull, c->sbuf);
}

typedef struct DatumCand
{
	const SiftV6Rows *r;
	const Datum *values;
	const bool *isnull;
} DatumCand;

static Datum
datum_value(void *ctx, int item, bool *isnull)
{
	const DatumCand *c = ctx;
	const int	pos = c->r->spec->topk_item[item].pos;

	*isnull = c->isnull[pos];
	return c->values[pos];
}

/* the row would stay: the heap is not full or the row beats its worst; a sort takes every row */
static bool
rows_wants(SiftV6Rows *r, SiftTopKValue value, void *ctx)
{
	return r->sort != NULL || sift_v6_topk_run_wants(r->run, value, ctx);
}

/* the row of r->values and r->isnull among the best */
static void
rows_keep(SiftV6Rows *r)
{
	const int	n = r->spec->nkeys + 1;

	if (r->sort == NULL)
	{
		sift_v6_topk_run_put(r->run, r->values, r->isnull);
		return;
	}
	ExecClearTuple(r->in);
	memcpy(r->in->tts_values, r->values, n * sizeof(Datum));
	memcpy(r->in->tts_isnull, r->isnull, n * sizeof(bool));
	tuplesort_puttupleslot(r->sort, ExecStoreVirtualTuple(r->in));
}

/* the sort column item of the worst row held, when a full heap has one */
static bool
rows_worst(const SiftV6Rows *r, int item, Datum *value, bool *isnull)
{
	return r->sort == NULL && sift_v6_topk_run_worst(r->run, item, value, isnull);
}

void
sift_v6_rows_put(SiftV6Rows *r, const Datum *values, const bool *isnull, uint64 tid)
{
	const DatumCand c = {.r = r,.values = values,.isnull = isnull};
	const int	nkeys = r->spec->nkeys;

	if (!rows_wants(r, datum_value, (void *) &c))
		return;
	memcpy(r->values, values, nkeys * sizeof(Datum));
	memcpy(r->isnull, isnull, nkeys * sizeof(bool));
	r->values[nkeys] = Int64GetDatum((int64) (tid | r->source));
	r->isnull[nkeys] = false;
	rows_keep(r);
}

static bool
text_type(Oid typid)
{
	return typid == TEXTOID || typid == VARCHAROID;
}

static bool
bound_type(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == DATEOID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID;
}

static int64
bound_value(Oid typid, Datum d)
{
	return typid == INT2OID ? DatumGetInt16(d) : typid == INT4OID || typid == DATEOID ? DatumGetInt32(d) : DatumGetInt64(d);
}

const SiftF2Bound *
sift_v6_rows_bound(const SiftV6Rows *r)
{
	return r->bounded ? &r->bound : NULL;
}

uint32
sift_v6_rows_given_k(const List *count, ExprContext *ec)
{
	const Node *offset = lsecond(count);
	bool		limit_null,
				offset_null = true;
	const int64 limit = DatumGetInt64(ExecEvalExprSwitchContext(ExecInitExpr(linitial(count), NULL), ec, &limit_null));
	const int64 skip = offset == NULL ? 0 : DatumGetInt64(ExecEvalExprSwitchContext(ExecInitExpr((Expr *) offset, NULL), ec, &offset_null));
	int64		sum;

	if (limit_null || limit < 0 || pg_add_s64_overflow(limit, offset_null || skip < 0 ? 0 : skip, &sum) || sum >= SIFT_V6_ROWS_ALL)
		return SIFT_V6_ROWS_ALL;
	return (uint32) sum;
}

/* the rows sorted by the sort columns of the spec (the positions of desc, the keys then the address), the first K of them kept */
static void
rows_sort_begin(SiftV6Rows *r, TupleDesc desc)
{
	const SiftV6Spec *spec = r->spec;
	AttrNumber	cols[SIFT_V6_MAX_TOPK_ITEMS];
	Oid			ops[SIFT_V6_MAX_TOPK_ITEMS];
	Oid			colls[SIFT_V6_MAX_TOPK_ITEMS];
	bool		nulls_first[SIFT_V6_MAX_TOPK_ITEMS];

	for (int i = 0; i < spec->ntopk; i++)
	{
		cols[i] = (AttrNumber) (spec->topk_item[i].pos + 1);
		ops[i] = spec->topk_item[i].sortop;
		colls[i] = spec->topk_item[i].collid;
		nulls_first[i] = spec->topk_item[i].nulls_first;
	}
	r->sort = tuplesort_begin_heap(desc, spec->ntopk, cols, ops, colls, nulls_first, work_mem, NULL, TUPLESORT_NONE);
	if (spec->topk != SIFT_V6_ROWS_ALL)
		tuplesort_set_bound(r->sort, spec->topk);
	r->in = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	r->out = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
}

SiftV6Rows *
sift_v6_rows_new(MemoryContext cx, const SiftV6Spec *spec)
{
	SiftV6Rows *r = MemoryContextAllocZero(cx, sizeof(SiftV6Rows));
	MemoryContext old = MemoryContextSwitchTo(cx);
	TupleDesc	desc = CreateTemplateTupleDesc(spec->nkeys + 1);
	const SiftV6Col *key0 = &spec->key[spec->topk_item[0].pos];

	for (int i = 0; i < spec->nkeys; i++)
	{
		TupleDescInitEntry(desc, i + 1, NULL, spec->key[i].typid, -1, 0);
		r->base_typid[i] = getBaseType(spec->key[i].typid);
	}
	TupleDescInitEntry(desc, spec->nkeys + 1, NULL, INT8OID, -1, 0);
	sift_tupdesc_finalize(desc);
	r->spec = spec;
	if (spec->topk > SIFT_TOPK_MAX_K)
		rows_sort_begin(r, desc);
	else
		r->run = sift_v6_topk_run_new(cx, spec, desc);
	r->bounded = key0->derive == 0 && key0->src != SV6SRC_DIM && bound_type(key0->typid);
	r->by_code = key0->derive == 0 && key0->src != SV6SRC_DIM && text_type(key0->typid) && OidIsValid(spec->topk_item[0].collid) && sift_collate_is_c(spec->topk_item[0].collid);
	if (r->bounded || r->by_code)
	{
		Oid			opfamily,
					opcintype;
		SiftCmpStrategy	cmptype;

		get_ordering_op_properties(spec->topk_item[0].sortop, &opfamily, &opcintype, &cmptype);
		r->desc = cmptype == SIFT_CMP_GT;
		r->bound = (SiftF2Bound) {.desc = r->desc,.nulls_first = spec->topk_item[0].nulls_first,.ties = spec->ntopk > 1};
	}
	r->tmp = sift_mem_allocset_create(cx, "sift agg v6 rows");
	MemoryContextSwitchTo(old);
	return r;
}

/* The first dictionary entry at or past the string (strict: past it) */
static uint32
dict_search(const SiftGranuleDict *d, const uint8 *p, uint32 len, bool strict)
{
	uint32		lo = 0,
				hi = d->n;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;
		const uint32 elen = d->off[mid + 1] - d->off[mid];
		const int	cmp = memcmp(d->bytes + d->off[mid], p, Min(elen, len));
		const bool	before = cmp != 0 ? cmp < 0 : (strict ? elen <= len : elen < len);

		if (before)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* The codes of the dictionary of the first sort column that a row must have to beat the heap's worst: the others sort after it, or with a lone sort column are equal to it */
static void
code_range(const SiftV6Rows *r, const SiftVec *k0, uint32 *lo, uint32 *hi)
{
	const SiftGranuleDict *d = k0->dict;
	Datum		worst;
	bool		worst_null;
	const text *t;

	*lo = 0;
	*hi = UINT32_MAX;
	if (!rows_worst(r, 0, &worst, &worst_null) || worst_null)
		return;
	t = DatumGetTextPP(worst);
	if (r->desc)
		*lo = dict_search(d, (const uint8 *) VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), r->spec->ntopk == 1);
	else if ((*hi = dict_search(d, (const uint8 *) VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), r->spec->ntopk > 1)) == 0)
		*lo = 1;
	else
		(*hi)--;
}

void
sift_v6_rows_update(SiftV6Rows *r, const SiftBatchSel *bs, SiftVec *const *keyvec, const uint64 *tid)
{
	const SiftV6Spec *spec = r->spec;
	const SiftVec *k0 = keyvec[spec->topk_item[0].pos];
	const bool	coded = r->by_code && k0->type == SVEC_DICT32;
	MemoryContext old = MemoryContextSwitchTo(r->tmp);
	uint32		lo = 0,
				hi = UINT32_MAX;

	if (coded)
		code_range(r, k0, &lo, &hi);
	for (int w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			RowCand		c = {.r = r,.keyvec = keyvec,.row = (uint16) (w * 64 + __builtin_ctzll(bits))};

			if (coded && is_valid(k0, c.row) && (((const uint32 *) k0->data)[c.row] > hi || ((const uint32 *) k0->data)[c.row] < lo))
				continue;
			if (!rows_wants(r, cand_value, &c))
				continue;
			for (int i = 0; i < spec->nkeys; i++)
				r->values[i] = key_datum(r, i, keyvec[i], c.row, &r->isnull[i], r->sbuf[i]);
			r->values[spec->nkeys] = Int64GetDatum((int64) (tid[c.row] | r->source));
			r->isnull[spec->nkeys] = false;
			rows_keep(r);
			if (coded)
				code_range(r, k0, &lo, &hi);
		}
	MemoryContextSwitchTo(old);
	MemoryContextReset(r->tmp);
	if (r->bounded)
	{
		Datum		worst;
		bool		worst_null;

		r->bound.active = rows_worst(r, 0, &worst, &worst_null) && !worst_null;
		if (r->bound.active)
			r->bound.value = bound_value(spec->key[spec->topk_item[0].pos].typid, worst);
	}
}

typedef struct PutCand
{
	const SiftV6Rows *r;
	const Datum *values;
	const bool *isnull;
} PutCand;

static Datum
put_value(void *ctx, int item, bool *isnull)
{
	const PutCand *c = ctx;
	const int	pos = c->r->spec->topk_item[item].pos;

	*isnull = c->isnull[pos];
	return c->values[pos];
}

void
sift_v6_rows_put_raw(SiftV6Rows *r, const int64 *ival, const char *const *str, const uint32 *slen, const bool *isnull, uint64 tid)
{
	const SiftV6Spec *spec = r->spec;
	MemoryContext old = MemoryContextSwitchTo(r->tmp);

	for (int i = 0; i < spec->nkeys; i++)
	{
		r->isnull[i] = isnull[i];
		if (isnull[i])
			r->values[i] = (Datum) 0;
		else if (text_type(spec->key[i].typid))
			r->values[i] = text_datum((const uint8 *) str[i], slen[i], r->sbuf[i]);
		else
			r->values[i] = int_key_datum(spec->key[i].typid, ival[i]);
	}
	r->values[spec->nkeys] = Int64GetDatum((int64) (tid | r->source));
	r->isnull[spec->nkeys] = false;
	if (rows_wants(r, put_value, &(PutCand) {r, r->values, r->isnull}))
		rows_keep(r);
	MemoryContextSwitchTo(old);
	MemoryContextReset(r->tmp);
}

void
sift_v6_rows_finish(SiftV6Rows *r)
{
	if (r->sort != NULL)
		tuplesort_performsort(r->sort);
	else
		sift_v6_topk_run_finish(r->run);
}

void
sift_v6_rows_end(SiftV6Rows *r)
{
	if (r->run != NULL)
	{
		sift_v6_topk_run_end(r->run);
		return;
	}
	if (r->sort == NULL)
		return;
	tuplesort_end(r->sort);
	ExecDropSingleTupleTableSlot(r->in);
	ExecDropSingleTupleTableSlot(r->out);
	r->sort = NULL;
}

SiftV6Group *
sift_v6_rows_totals_new(MemoryContext cx, const SiftV6Spec *spec)
{
	SiftV6Spec *totals = MemoryContextAlloc(cx, sizeof(SiftV6Spec));
	TupleDesc	desc = CreateTemplateTupleDesc(spec->naggs);

	*totals = *spec;
	totals->nkeys = 0;
	totals->rows = false;
	totals->topk = 0;
	for (int j = 0; j < spec->naggs; j++)
		TupleDescInitEntry(desc, j + 1, NULL, spec->agg[j].restype, -1, 0);
	sift_tupdesc_finalize(desc);
	return sift_v6_group_new(totals, cx, desc);
}

void
sift_v6_rows_source(SiftV6Rows *r, uint32 source)
{
	r->source = (uint64) source << SIFT_V6_ROWS_ADDR_BITS;
}

/* the next best row into r->values and r->isnull, valid until the next one */
static bool
rows_next(SiftV6Rows *r)
{
	const int	n = r->spec->nkeys + 1;

	if (r->sort == NULL)
		return sift_v6_topk_run_next(r->run, r->values, r->isnull);
	if (!tuplesort_gettupleslot(r->sort, true, false, r->out, NULL))
		return false;
	slot_getallattrs(r->out);
	memcpy(r->values, r->out->tts_values, n * sizeof(Datum));
	memcpy(r->isnull, r->out->tts_isnull, n * sizeof(bool));
	return true;
}

bool
sift_v6_rows_next(SiftV6Rows *r, uint32 *source, uint64 *tid)
{
	uint64		addr;

	if (!rows_next(r))
		return false;
	addr = (uint64) DatumGetInt64(r->values[r->spec->nkeys]);
	*source = (uint32) (addr >> SIFT_V6_ROWS_ADDR_BITS);
	*tid = addr & ((UINT64CONST(1) << SIFT_V6_ROWS_ADDR_BITS) - 1);
	return true;
}

Datum
sift_v6_rows_key(const SiftV6Rows *r, int key, bool *isnull)
{
	*isnull = r->isnull[key];
	return r->values[key];
}
