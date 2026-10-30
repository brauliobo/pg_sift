/*
 * agg/topn/key.c - the order of the entries and the keys of a row
 * The comparison of two keys (bytes, blobs, enums, floats), the heap of the best rows, the
 * keys an expression gives a unit, the result entries and the final sort.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/tuplesort.h"
#include "utils/uuid.h"
#include "utils/varlena.h"

#include "agg/topn/int.h"

#define ST_SORT sort_ents
#define ST_ELEMENT_TYPE TopEnt
#define ST_COMPARE_ARG_TYPE SiftTopN
#define ST_COMPARE(a, b, t) ent_cmp(t, a, b)
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

/* < 0, 0, > 0: the entry a sorts before, with or after b, ascending, as the type's btree comparison of the query's operator class says */
int
bytes_cmp(const TopKey *k, const char *a, uint32 la, const char *b, uint32 lb)
{
	switch (k->bkind)
	{
		case BK_TEXT:
			return varstr_cmp(a, la, b, lb, k->collid);
		case BK_UUID:
			return memcmp(a, b, 16);
		case BK_BYTEA:
			{
				int			c = memcmp(a + VARHDRSZ, b + VARHDRSZ, Min(la, lb) - VARHDRSZ);

				return c != 0 ? c : SIFT_CMP3(la, lb);
			}
		default:
			{
				union
				{
					char		c[64];
					int64		align;
				}			x,
							y;
				char	   *pa = la <= sizeof(x.c) ? x.c : palloc(la),
						   *pb = lb <= sizeof(y.c) ? y.c : palloc(lb);
				int			c;

				memcpy(pa, a, la);
				memcpy(pb, b, lb);
				c = DatumGetInt32(FunctionCall2Coll(k->cmp, k->collid, PointerGetDatum(pa), PointerGetDatum(pb)));
				if (pa != x.c)
					pfree(pa);
				if (pb != y.c)
					pfree(pb);
				return c;
			}
	}
}

/* < 0: a sorts before b by the first nk keys */
int
key_cmp_n(const SiftTopN *t, const TopEnt *a, const TopEnt *b, int nk)
{
	for (int j = 0; j < nk; j++)
	{
		bool		an = (a->nul >> j) & 1,
					bn = (b->nul >> j) & 1;

		if (an != bn)
			return an == t->key[j].nulls_first ? -1 : 1;
		if (an || a->k[j] == b->k[j])
			continue;
		if (t->key[j].blob)
		{
			const KeyBlob *x = KEY_BLOB(a->k[j]),
					   *y = KEY_BLOB(b->k[j]);
			int			c = bytes_cmp(&t->key[j], x->data, x->len, y->data, y->len);

			if (c == 0)
				continue;
			return (c < 0) != t->key[j].desc ? -1 : 1;
		}
		return (a->k[j] < b->k[j]) != t->key[j].desc ? -1 : 1;
	}
	return 0;
}

/* the heap keeps its worst entry at h[0]: a parent sorts after its children */
static void
heap_down(const SiftTopN *t, TopEnt *h, int n, int i)
{
	TopEnt		e = h[i];

	for (;;)
	{
		int			c = 2 * i + 1;

		if (c >= n)
			break;
		if (c + 1 < n && ent_cmp(t, &h[c + 1], &h[c]) > 0)
			c++;
		if (ent_cmp(t, &h[c], &e) <= 0)
			break;
		h[i] = h[c];
		i = c;
	}
	h[i] = e;
}

void
heap_push(const SiftTopN *t, TopEnt *h, int *n, const TopEnt *e)
{
	int			i;

	if (*n == t->limit)
	{
		if (ent_cmp(t, e, &h[0]) >= 0)
			return;
		h[0] = *e;
		heap_down(t, h, *n, 0);
		return;
	}
	i = (*n)++;
	while (i > 0 && ent_cmp(t, e, &h[(i - 1) / 2]) > 0)
	{
		h[i] = h[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	h[i] = *e;
}

/* the string through lower() or upper() as the call of the query does it: a copy in the arena of the unit */
KeyBlob *
xform_blob(SiftTopN *t, const TopKey *k, const char *p, uint32 len)
{
	MemoryContext old = MemoryContextSwitchTo(t->ucxt);
	struct varlena *in = palloc(VARHDRSZ + len),
			   *out;
	KeyBlob    *b;

	SET_VARSIZE(in, VARHDRSZ + len);
	memcpy(VARDATA(in), p, len);
	out = (struct varlena *) DatumGetPointer(FunctionCall1Coll(k->xfn, k->xcoll, PointerGetDatum(in)));
	b = blob_new(t->ucxt, VARDATA_ANY(out), VARSIZE_ANY_EXHDR(out));
	MemoryContextSwitchTo(old);
	return b;
}

/* the key j of row i of v, of any key type: a byte string is a copy in the arena of the unit */
bool
row_val(SiftTopN *t, int j, const SiftSrcVec *v, int i, int64 *out)
{
	const char *p;
	uint32		len;

	if (!t->key[j].blob)
		return row_key(&t->key[j], v, i, out);
	if (v->isnull && v->isnull[i])
		return false;
	key_entry(t, j, v->dict, ((const uint32 *) v->data)[i], &p, &len);
	*out = (int64) (uintptr_t) blob_new(t->ucxt, p, len);
	return true;
}

/* The vector a key of this result type is kept in: the integer types by their width, floats widened, anything else as byte strings of a dictionary */
SiftSrcVecType
expr_vtype(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return SCV_I32;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return SCV_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return SCV_F64;
		default:
			return SCV_CODE;
	}
}

/* xs: the expression over Vars numbered 1.. and the leaves they stand for, in that order */
TopExpr *
expr_open(List *xs, const SiftSrcLayout *lay, Oid rtype, ExprContext *outer, MemoryContext cxt)
{
	MemoryContext old = MemoryContextSwitchTo(cxt);
	TopExpr    *x = palloc0(sizeof(TopExpr));
	List	   *leaves = lsecond(xs);
	TupleDesc	desc;

	x->nin = list_length(leaves);
	x->in = palloc0(Max(x->nin, 1) * sizeof(TopIn));
	desc = CreateTemplateTupleDesc(Max(x->nin, 1));
	for (int a = 0; a < x->nin; a++)
	{
		const SiftSrcLeaf *lf = &lay->leaf[list_nth_int(leaves, a)];

		x->in[a] = (TopIn) {.leaf = lf->id, .typid = lf->typid, .vtype = lf->vtype, .scale = lf->scale};
		TupleDescInitEntry(desc, a + 1, NULL, lf->typid, -1, 0);
	}
	sift_tupdesc_finalize(desc);
	x->slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	x->econtext = CreateStandaloneExprContext();
	x->econtext->ecxt_scantuple = x->slot;
	/* the parameters of the statement (the leader only: the planner does not give a sort expression with one to workers) */
	x->econtext->ecxt_param_list_info = outer->ecxt_param_list_info;
	/* no parent: the Vars are of the slot above whatever the scan's own tuple is */
	x->state = ExecInitExpr((Expr *) linitial(xs), NULL);
	x->rtype = rtype;
	MemoryContextSwitchTo(old);
	return x;
}

/* The datum of the entry c of the string dictionary of an input, built once for the unit */
static Datum
in_entry(SiftTopN *t, TopIn *in, uint32 c)
{
	const SiftSrcDict *d = in->v->dict;
	const char *p = d->bytes + d->off[c];
	uint32		len = d->off[c + 1] - d->off[c];
	char	   *r;

	if (in->built[c])
		return in->memo[c];
	switch (in->typid)
	{
		case TEXTOID:
		case VARCHAROID:
			r = MemoryContextAlloc(t->ucxt, VARHDRSZ + len);
			SET_VARSIZE(r, VARHDRSZ + len);
			memcpy(VARDATA(r), p, len);
			break;
		case UUIDOID:
			r = MemoryContextAlloc(t->ucxt, len);
			memcpy(r, p, len);
			break;
		default:				/* bytea, inet: the image of the datum, its header says the length */
			{
				varattrib_4b h;

				memcpy(&h, p, sizeof(uint32));
				len = VARSIZE_4B(&h);
				r = MemoryContextAlloc(t->ucxt, len);
				memcpy(r, p, len);
			}
	}
	in->built[c] = true;
	return in->memo[c] = PointerGetDatum(r);
}

static Datum
in_datum(SiftTopN *t, TopIn *in, int i, bool *isnull)
{
	const SiftSrcVec *v = in->v;

	*isnull = v->isnull && v->isnull[i];
	if (*isnull)
		return (Datum) 0;
	switch (in->vtype)
	{
		case SCV_I32:
			return Int32GetDatum(((const int32 *) v->data)[i]);
		case SCV_I64:
			if (in->typid != NUMERICOID)
				return Int64GetDatum(((const int64 *) v->data)[i]);
			if (v->ovf && ((v->ovf[i / 64] >> (i % 64)) & 1))
				return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
			return NumericGetDatum(int64_div_fast_to_numeric(((const int64 *) v->data)[i], in->scale));
		case SCV_F64:
			return in->typid == FLOAT4OID ? Float4GetDatum((float4) ((const double *) v->data)[i]) : Float8GetDatum(((const double *) v->data)[i]);
		default:
			return in_entry(t, in, ((const uint32 *) v->data)[i]);
	}
}

/*
 * The key j of every row of the unit that passes, from PostgreSQL's evaluation of its expression over those rows' leaves: one result for each
 * (the normal plan computes the sort expression of every row that passes the WHERE, and raises its errors for them). The result is the vector a
 * key of the leaf of that type would be, and the rest of the scan reads it as such.
 */
void
expr_eval_unit(SiftTopN *t, TopKey *k, SiftTopLeaf leaf, void *ctx, const uint64 *rmask, int nrow)
{
	TopExpr    *x = k->x;
	int			nw = (nrow + 63) / 64,
				npass = 0,
				at = 0,
				esz = k->vtype == SCV_I32 ? sizeof(int32) : k->vtype == SCV_CODE ? sizeof(uint32) : sizeof(int64);
	uint8	   *nul = MemoryContextAllocZero(t->ucxt, Max(nrow, 1));
	char	   *data = MemoryContextAllocZero(t->ucxt, Max(nrow, 1) * (Size) esz);
	SiftSrcDict *dict = NULL;
	uint32	   *off = NULL;
	StringInfoData bytes = {0};
	MemoryContext tup = x->econtext->ecxt_per_tuple_memory;

	for (int w = 0; w < nw; w++)
		npass += pg_popcount64(rmask[w]);
	for (int a = 0; a < x->nin; a++)
	{
		TopIn	   *in = &x->in[a];

		in->v = leaf(ctx, in->leaf);
		if (in->vtype == SCV_CODE)
		{
			in->memo = MemoryContextAllocZero(t->ucxt, Max(in->v->dict->n, 1) * sizeof(Datum));
			in->built = MemoryContextAllocZero(t->ucxt, Max(in->v->dict->n, 1));
		}
	}
	if (k->vtype == SCV_CODE)
	{
		MemoryContext old = MemoryContextSwitchTo(t->ucxt);

		dict = palloc0(sizeof(SiftSrcDict));
		off = palloc((npass + 1) * sizeof(uint32));
		initStringInfo(&bytes);
		off[0] = 0;
		MemoryContextSwitchTo(old);
	}
	for (int w = 0; w < nw; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			bool		isnull;
			Datum		d;
			MemoryContext old = MemoryContextSwitchTo(tup);

			ExecClearTuple(x->slot);
			for (int a = 0; a < x->nin; a++)
				x->slot->tts_values[a] = in_datum(t, &x->in[a], i, &x->slot->tts_isnull[a]);
			ExecStoreVirtualTuple(x->slot);
			MemoryContextSwitchTo(old);
			d = ExecEvalExprSwitchContext(x->state, x->econtext, &isnull);
			if (isnull)
				nul[i] = 1;
			switch (k->vtype)
			{
				case SCV_I32:
					((int32 *) data)[i] = x->rtype == INT2OID ? DatumGetInt16(d) : x->rtype == BOOLOID ? DatumGetBool(d) : DatumGetInt32(d);
					break;
				case SCV_I64:
					((int64 *) data)[i] = DatumGetInt64(d);
					break;
				case SCV_F64:
					((double *) data)[i] = x->rtype == FLOAT4OID ? DatumGetFloat4(d) : DatumGetFloat8(d);
					break;
				default:
					if (!isnull)
					{
						MemoryContext o2 = MemoryContextSwitchTo(t->ucxt);

						if (k->bkind == BK_UUID)
							appendBinaryStringInfo(&bytes, DatumGetPointer(d), UUID_LEN);
						else
						{
							struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
							uint32		len = VARSIZE_ANY_EXHDR(v);

							if (k->bkind == BK_TEXT)
								appendBinaryStringInfo(&bytes, VARDATA_ANY(v), len);
							else
							{
								/* the image of the datum, with the four byte header the entries of a dictionary have */
								varattrib_4b hdr;

								SET_VARSIZE(&hdr, VARHDRSZ + len);
								appendBinaryStringInfo(&bytes, (char *) &hdr, VARHDRSZ);
								appendBinaryStringInfo(&bytes, VARDATA_ANY(v), len);
							}
						}
						MemoryContextSwitchTo(o2);
					}
					((uint32 *) data)[i] = at;
					off[++at] = bytes.len;
			}
			ResetExprContext(x->econtext);
			if ((i & 4095) == 4095)
				CHECK_FOR_INTERRUPTS();
		}
	k->xv = (SiftSrcVec) {.data = data, .isnull = nul, .n = nrow};
	if (dict)
	{
		*dict = (SiftSrcDict) {.id = dict, .n = at, .off = off, .bytes = bytes.data};
		k->xv.dict = dict;
	}
}

/* The key j of the rows t->cand[0..n): read from the leaf, or taken from the vector of the expression */
void
key_rows(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int j, int n, SiftSrcVec *v)
{
	const TopKey *k = &t->key[j];
	int			esz = k->vtype == SCV_I32 ? sizeof(int32) : k->vtype == SCV_CODE ? sizeof(uint32) : sizeof(int64);
	char	   *data;
	uint8	   *nul;

	if (k->x == NULL)
	{
		sift_colsrc_read_rows(src, unit, k->leaf, t->cand, n, v);
		return;
	}
	data = MemoryContextAlloc(t->ucxt, Max(n, 1) * (Size) esz);
	nul = MemoryContextAlloc(t->ucxt, Max(n, 1));
	for (int x = 0; x < n; x++)
	{
		memcpy(data + (Size) x * esz, (const char *) k->xv.data + (Size) t->cand[x] * esz, esz);
		nul[x] = k->xv.isnull[t->cand[x]];
	}
	*v = (SiftSrcVec) {.data = data, .isnull = nul, .dict = k->xv.dict, .n = n};
}

/* The heap of the result keeps its own copies of the byte strings of the entries it takes, and frees them when it drops the row */
static void
res_own(SiftTopN *t, TopEnt *e)
{
	MemoryContext cxt = GetMemoryChunkContext(t);

	for (int j = 0; j < t->nkey; j++)
		if (t->key[j].blob && !((e->nul >> j) & 1))
		{
			const KeyBlob *b = KEY_BLOB(e->k[j]);

			e->k[j] = (int64) (uintptr_t) blob_new(cxt, b->data, b->len);
		}
}

static void
res_free(const SiftTopN *t, const TopEnt *e)
{
	for (int j = 0; j < t->nkey; j++)
		if (t->key[j].blob && !((e->nul >> j) & 1))
			pfree((void *) KEY_BLOB(e->k[j]));
}

/* the place of the entry's output columns and the columns themselves (v: nov values and flags; NULL: the row is fetched from the heap) */
static inline void
res_values(SiftTopN *t, TopEnt *c, uint32 oi, const int64 *v, const bool *vn)
{
	c->oi = oi;
	c->hasv = v != NULL;
	if (v != NULL)
	{
		memcpy(&t->ov[(Size) oi * t->nov], v, t->nov * sizeof(int64));
		memcpy(&t->ovn[(Size) oi * t->nov], vn, t->nov * sizeof(bool));
	}
}

/* An entry takes the place of the one it pushes out, or the next place while the heap fills */
void
res_push(SiftTopN *t, const TopEnt *e, const int64 *v, const bool *vn)
{
	TopEnt		c = *e,
				old;

	if (t->nblob == 0)
	{
		/* the entries are kept as they come until there are n of them, then made a heap: the fill costs one comparison an entry less log n */
		if (t->n < t->limit)
		{
			res_values(t, &c, t->n, v, vn);
			t->heap[t->n++] = c;
			if (t->n == t->limit)
				for (int i = t->n / 2 - 1; i >= 0; i--)
					heap_down(t, t->heap, t->n, i);
			return;
		}
		if (ent_cmp(t, e, &t->heap[0]) >= 0)
			return;
		res_values(t, &c, t->heap[0].oi, v, vn);
		t->heap[0] = c;
		heap_down(t, t->heap, t->n, 0);
		return;
	}
	if (t->n == t->limit)
	{
		if (ent_cmp(t, e, &t->heap[0]) >= 0)
			return;
		old = t->heap[0];
		res_own(t, &c);
		res_values(t, &c, old.oi, v, vn);
		heap_push(t, t->heap, &t->n, &c);
		res_free(t, &old);
		return;
	}
	res_own(t, &c);
	res_values(t, &c, t->n, v, vn);
	heap_push(t, t->heap, &t->n, &c);
}

void
sift_topn_finish(SiftTopN *t)
{
	t->pos = 0;
	if (t->sort)
		tuplesort_performsort(t->sort);
	else if (t->all)
		qsort_arg(t->mem, t->nmem, t->stride * sizeof(int64), mem_cmp, t);
	else
	{
		t->res = t->heap;
		sort_ents(t->res, t->n, t);
		if (t->want > 0 && t->n >= t->want)
		{
			const TopEnt *w = &t->res[t->want - 1];

			if (t->n == t->limit && key_cmp(t, w, &t->res[t->n - 1]) == 0)
				t->incomplete = true;
			else
				while (t->n > t->want && key_cmp(t, w, &t->res[t->n - 1]) != 0)
					t->n--;
		}
	}
}
