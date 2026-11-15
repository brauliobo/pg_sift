/*
 * agg/combo/ids.c - combos: the ids of values and the binding of the leaves
 * The ids of the dictionary and the 64-bit values a combo reads, the prior ids, and the
 * binding of the leaves a combo is computed from.
 */
/* check-memory: allow-allocs the cache of combinations stops at COMBO_MAX_ENTRIES (16 million) and its hash slots follow them */
#include "postgres.h"

#include "common/hashfn.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"

#include "agg/agg.h"
#include "agg/combo/int.h"

/* the id (1..) of a value among the distinct values of a dictionary leaf; the bytes are copied when new */
uint32
intern_id(AggExec *ex, Intern *t, const char *b, uint32 len)
{
	uint32		hv = hash_bytes((const unsigned char *) b, len),
				h;
	char	   *cp;

	if (t->nslot == 0)
	{
		t->nslot = 64;
		t->cap = 16;
		t->slot = MemoryContextAllocZero(ex->cxt, t->nslot * sizeof(uint32));
		t->ptr = MemoryContextAlloc(ex->cxt, t->cap * sizeof(char *));
		t->len = MemoryContextAlloc(ex->cxt, t->cap * sizeof(uint32));
		t->hv = MemoryContextAlloc(ex->cxt, t->cap * sizeof(uint32));
	}
	h = hv & (t->nslot - 1);
	for (uint32 id; (id = t->slot[h]) != 0; h = (h + 1) & (t->nslot - 1))
		if (t->hv[id - 1] == hv && t->len[id - 1] == len && memcmp(t->ptr[id - 1], b, len) == 0)
			return id;
	if (t->n == t->cap)
	{
		t->cap *= 2;
		t->ptr = repalloc(t->ptr, t->cap * sizeof(char *));
		t->len = repalloc(t->len, t->cap * sizeof(uint32));
		t->hv = repalloc(t->hv, t->cap * sizeof(uint32));
	}
	cp = MemoryContextAlloc(ex->cxt, len + 1);
	memcpy(cp, b, len);
	cp[len] = '\0';
	t->ptr[t->n] = cp;
	t->len[t->n] = len;
	t->hv[t->n] = hv;
	t->slot[h] = ++t->n;
	if (t->n * 2 > t->nslot)
		SIFT_SLOT_REHASH(id, t->slot, t->nslot, MemoryContextAllocZero(ex->cxt, t->nslot * sizeof(uint32)), t->n, t->hv[id - 1]);
	return t->n;
}

/* the ids over all dictionaries of the entries of d, for dictionary leaf p of combo ci; an entry gets its id when a tuple first reads it (cmap_id) */
uint32 *
dict_cmap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ci, int p)
{
	int			slot = ex->spec->nkeys + ex->spec->naggs + ex->clbase[ci] + p;
	uint32	   *map = s->kmap[slot];

	if (map)
		return map;
	map = MemoryContextAlloc(ex->cxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
	memset(map, 0xFF, d->n * sizeof(uint32));
	map[d->n] = 0;
	s->kmap[slot] = map;
	return map;
}

void
id64_init(Id64 *m)
{
	intids_init(&m->ix);
	m->n = 0;
	m->cap = 2048;
	m->nan_id = 0;
	m->val = palloc(m->cap * sizeof(int64));
}

static uint32
id64_new(Id64 *m, int64 k)
{
	SIFT_GROW(m->val, m->n, m->cap);
	m->val[m->n] = k;
	return ++m->n;
}

uint32
id64_id(Id64 *m, int64 k)
{
	uint32		id = intids_id(&m->ix, k, m->n + 1);

	if (id > m->n)
		id64_new(m, k);
	return id;
}

uint32
id64_nan(Id64 *m)
{
	if (m->nan_id == 0)
		m->nan_id = id64_new(m, 0);
	return m->nan_id;
}

/* a stored value of a 64-bit leaf (a float: its bits) as the Datum of the leaf's type; a numeric is the integer times 10^-scale, with that many digits */
static Datum
leaf64_datum(const Id64 *m, const SiftSrcLeaf *lf, uint32 id)
{
	int64		v = m->val[id - 1];

	if (lf->vtype == SCV_F64)
		return lf->typid == FLOAT4OID ? Float4GetDatum((float4) float_from_key(v)) : Float8GetDatum(float_from_key(v));
	if (lf->typid != NUMERICOID)
		return Int64GetDatum(v);
	if (id == m->nan_id)
		return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	return NumericGetDatum(lf->scale > 0 ? int64_div_fast_to_numeric(v, lf->scale) : int64_to_numeric(v));
}

/* the id of the value of column refcol[p] in output row `out` of the earlier lookup, made on its first sight; the row is kept to bind it */
static uint32
prior_vid_new(AggExec *ex, int ci, int p, uint32 out)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	ComboSt    *cs = &ex->cs[ci];
	const ComboSt *prior = &ex->cs[cb->refcombo[p]];
	PriorVal   *pv = &cs->pv[p];
	uint32		row,
				id = 0;

	if (out == COMBO_NONE)
		return 0;
	if (out >= pv->nmemo)
	{
		uint32		cap = Max(Max(pv->nmemo * 2, out + 1), 256);

		SIFT_REGROW_IN(cs->cxt, pv->memo, cap * sizeof(uint32));
		memset(pv->memo + pv->nmemo, 0xFF, (cap - pv->nmemo) * sizeof(uint32));
		pv->nmemo = cap;
	}
	row = prior->maprow[out];
	if (row != UINT32_MAX)
	{
		const MapCol *mc = &prior->map->mcol[cb->refcol[p]];
		bool		isnull;
		Datum		d = map_col(prior->map, row, cb->refcol[p], &isnull);

		if (!isnull)
		{
			if (!mc->byval)
				id = mc->id[row];
			else
			{
				MemoryContext old = MemoryContextSwitchTo(cs->cxt);

				if (cs->id64[p].val == NULL)
					id64_init(&cs->id64[p]);
				MemoryContextSwitchTo(old);
				id = id64_id(&cs->id64[p], (int64) d);
			}
		}
	}
	if (id > pv->nrep)
	{
		uint32		cap = Max(Max(pv->nrep * 2, id), 64);

		SIFT_REGROW_IN(cs->cxt, pv->rep, cap * sizeof(uint32));
		memset(pv->rep + pv->nrep, 0xFF, (cap - pv->nrep) * sizeof(uint32));
		pv->nrep = cap;
	}
	if (id > 0 && pv->rep[id - 1] == UINT32_MAX)
		pv->rep[id - 1] = out;
	pv->memo[out] = id;
	return id;
}

/*
 * The id (0 NULL) of the value of a column of an earlier lookup at its output row `out`, which stands for the leaf in the entries of the combo:
 * the rows of that table with the same value share one entry (a few hundred types, not the thousands of entities that hold them).
 */
uint64
combo_prior_vid(AggExec *ex, int ci, int p, uint32 out)
{
	const PriorVal *pv = &ex->cs[ci].pv[p];

	if (out < pv->nmemo && pv->memo[out] != COMBO_UNEVAL)
		return pv->memo[out];
	return prior_vid_new(ex, ci, p, out);
}

/* the leaf values of an entry (v: ids) as the Params of the combo's expressions */
void
combo_bind_leaves(AggExec *ex, int ci, const uint64 *v)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	ComboSt    *cs = &ex->cs[ci];

	bind_param(ex, SIFT_AGG_NULL_INPUT, BoolGetDatum(combo_null_input(ex)), false);

	for (int p = 0; p < cb->nleaves; p++)
	{
		const SiftSrcLeaf *lf;

		if (cb->leaf[p] < 0)
		{
			ComboSt    *prior = &ex->cs[cb->refcombo[p]];

			if (v[p] == 0)
				bind_param(ex, p, (Datum) 0, true);
			else
				map_bind(ex, p, prior->map, prior->maprow[cs->pv[p].rep[v[p] - 1]], cb->refcol[p]);
			continue;
		}
		lf = &ex->lay->leaf[cb->leaf[p]];

		if (v[p] == 0)
			bind_param(ex, p, (Datum) 0, true);
		else if (lf->vtype == SCV_CODE)
			bind_param(ex, p, text_datum(cb->ptype[p], cb->ptypmod[p], cs->in[p].ptr[v[p] - 1], cs->in[p].len[v[p] - 1]), false);
		else if (leaf_wide(lf))
			bind_param(ex, p, leaf64_datum(&cs->id64[p], lf, (uint32) v[p]), false);
		else
			bind_param(ex, p, leaf_datum(cb->ptype[p], (int32) (v[p] - 1)), false);
	}
}

bool		sift_child_warm_test = false;	/* sift.test_child_warm */
#ifdef SIFT_TESTING
uint64		sift_test_warm_calls = 0;		/* the blocks the probe of a child asked ahead of their fetch (sift._test_segment_reads) */
#endif

