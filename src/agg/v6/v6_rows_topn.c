/*
 * agg/v6/v6_rows_topn.c - the heap of the best rows of a scan (agg/v6/v6_rows_topn.h).
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "agg/group/ht.h"
#include "agg/v6/group_int.h"
#include "agg/v6/v6_rows_topn.h"
#include "agg/v6/v6_topk_run.h"

struct SiftV6Rows
{
	const SiftV6Spec *spec;
	SiftV6TopKRun *run;
	MemoryContext tmp;
	Datum		values[SIFT_V6_MAX_KEYS + 1];
	bool		isnull[SIFT_V6_MAX_KEYS + 1];
};

typedef struct RowCand
{
	const SiftV6Rows *r;
	SiftVec *const *keyvec;
	uint16		row;
} RowCand;

static Datum
key_datum(const SiftV6Col *c, const SiftVec *v, uint16 row, bool *isnull)
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
				const SiftStrView *sv = &((const SiftStrView *) v->data)[row];

				return PointerGetDatum(cstring_to_text_with_len((const char *) sift_strview_bytes(sv), (int) sv->len));
			}
		default:
			return Int64GetDatum(int_at(v, row));
	}
}

static Datum
cand_value(void *ctx, int item, bool *isnull)
{
	const RowCand *c = ctx;
	const int	pos = c->r->spec->topk_item[item].pos;

	return key_datum(&c->r->spec->key[pos], c->keyvec[pos], c->row, isnull);
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
	r->spec = spec;
	r->run = sift_v6_topk_run_new(cx, spec, desc);
	r->tmp = AllocSetContextCreate(cx, "sift agg v6 rows", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(old);
	return r;
}

void
sift_v6_rows_update(SiftV6Rows *r, const SiftBatchSel *bs, SiftVec *const *keyvec, const uint64 *tid)
{
	const SiftV6Spec *spec = r->spec;
	MemoryContext old = MemoryContextSwitchTo(r->tmp);

	for (int w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const RowCand c = {r, keyvec, (uint16) (w * 64 + __builtin_ctzll(bits))};

			if (!sift_v6_topk_run_wants(r->run, cand_value, (void *) &c))
				continue;
			for (int i = 0; i < spec->nkeys; i++)
				r->values[i] = key_datum(&spec->key[i], keyvec[i], c.row, &r->isnull[i]);
			r->values[spec->nkeys] = Int64GetDatum((int64) tid[c.row]);
			r->isnull[spec->nkeys] = false;
			sift_v6_topk_run_put(r->run, r->values, r->isnull);
		}
	MemoryContextSwitchTo(old);
	MemoryContextReset(r->tmp);
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
