/*
 * agg/v6/v6_rows_topn.c - the heap of the best rows of a scan (agg/v6/v6_rows_topn.h).
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/group/ht.h"
#include "agg/v6/group_int.h"
#include "agg/v6/v6_rows_topn.h"
#include "agg/v6/v6_topk_run.h"
#include "core/compat.h"

#define SHORT_TEXT_MAX	126

struct SiftV6Rows
{
	const SiftV6Spec *spec;
	SiftV6TopKRun *run;
	MemoryContext tmp;
	Datum		values[SIFT_V6_MAX_KEYS + 1];
	bool		isnull[SIFT_V6_MAX_KEYS + 1];
	char		sbuf[SIFT_V6_MAX_KEYS][SHORT_TEXT_MAX + 1];
	SiftF2Bound bound;
	bool		bounded;		/* the first sort column is an integer: the heap's worst row bounds what a granule must hold */
	bool		by_code;		/* the first sort column is a text in byte order: a dictionary lists its strings in that order, so the codes bound the rows as the strings do */
	bool		desc;
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
key_datum(const SiftV6Col *c, const SiftVec *v, uint16 row, bool *isnull, char *sbuf)
{
	*isnull = !is_valid(v, row);
	if (*isnull)
		return (Datum) 0;
	switch (c->typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) int_at(v, row));
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) int_at(v, row));
		case TEXTOID:
		case VARCHAROID:
			{
				uint32		len;
				const uint8 *p = text_at(v, row, &len);

				return text_datum(p, len, sbuf);
			}
		default:
			return Int64GetDatum(int_at(v, row));
	}
}

static Datum
cand_value(void *ctx, int item, bool *isnull)
{
	RowCand    *c = ctx;
	const int	pos = c->r->spec->topk_item[item].pos;

	return key_datum(&c->r->spec->key[pos], c->keyvec[pos], c->row, isnull, c->sbuf);
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

SiftV6Rows *
sift_v6_rows_new(MemoryContext cx, const SiftV6Spec *spec)
{
	SiftV6Rows *r = MemoryContextAllocZero(cx, sizeof(SiftV6Rows));
	MemoryContext old = MemoryContextSwitchTo(cx);
	TupleDesc	desc = CreateTemplateTupleDesc(spec->nkeys + 1);

	for (int i = 0; i < spec->nkeys; i++)
		TupleDescInitEntry(desc, i + 1, NULL, spec->key[i].typid, -1, 0);
	TupleDescInitEntry(desc, spec->nkeys + 1, NULL, INT8OID, -1, 0);
	sift_tupdesc_finalize(desc);
	r->spec = spec;
	r->run = sift_v6_topk_run_new(cx, spec, desc);
	r->bounded = bound_type(spec->key[spec->topk_item[0].pos].typid);
	r->by_code = text_type(spec->key[spec->topk_item[0].pos].typid) && OidIsValid(spec->topk_item[0].collid) && sift_collate_is_c(spec->topk_item[0].collid);
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
	if (!sift_v6_topk_run_worst(r->run, 0, &worst, &worst_null) || worst_null)
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
			if (!sift_v6_topk_run_wants(r->run, cand_value, &c))
				continue;
			for (int i = 0; i < spec->nkeys; i++)
				r->values[i] = key_datum(&spec->key[i], keyvec[i], c.row, &r->isnull[i], r->sbuf[i]);
			r->values[spec->nkeys] = Int64GetDatum((int64) tid[c.row]);
			r->isnull[spec->nkeys] = false;
			sift_v6_topk_run_put(r->run, r->values, r->isnull);
			if (coded)
				code_range(r, k0, &lo, &hi);
		}
	MemoryContextSwitchTo(old);
	MemoryContextReset(r->tmp);
	if (r->bounded)
	{
		Datum		worst;
		bool		worst_null;

		r->bound.active = sift_v6_topk_run_worst(r->run, 0, &worst, &worst_null) && !worst_null;
		if (r->bound.active)
			r->bound.value = bound_value(spec->key[spec->topk_item[0].pos].typid, worst);
	}
}

void
sift_v6_rows_finish(SiftV6Rows *r)
{
	sift_v6_topk_run_finish(r->run);
}

bool
sift_v6_rows_next(SiftV6Rows *r, uint64 *tid)
{
	if (!sift_v6_topk_run_next(r->run, r->values, r->isnull))
		return false;
	*tid = (uint64) DatumGetInt64(r->values[r->spec->nkeys]);
	return true;
}
