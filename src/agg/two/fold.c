/*
 * agg/two/fold.c - the cells of a two-level aggregate into the vectors of a batch (agg/two/fold.h). A value goes into the vector of its class as a scan decodes it (int2, int4 and
 * date as int32, float4 widened to float8, a text as the view of its bytes); a type no native vector holds is a datum. The bytes a batch points to are copied into its memory,
 * which is reset when the batch is folded.
 */
/* check-memory: allow-allocs the vectors of one batch of cells and the state of one fold */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/datum.h"
#include "agg/two/fold.h"
#include "core/memtrack.h"

typedef struct TwoIn
{
	ExprState  *expr;
	Oid			typid;
	SiftVecType type;
	int16		typlen;
	bool		typbyval;
	void	   *data;
	uint64		valid[SIFT_VEC_WORDS];
	bool		all_valid;
} TwoIn;

struct TwoFold
{
	SiftV6Group *group;
	ExprContext *econtext;
	MemoryContext bcx;			/* the bytes of the values of the batch */
	int			ninputs;
	TwoIn	   *in;
	SiftVec    *vec;
	SiftVec   **keyvec;
	SiftVec   **argvec;
	SiftBatchSel *bs;
	uint16		n;
	uint64		cells;
};

static SiftVecType
vec_type(Oid typid)
{
	if (!sift_v6_type_native(typid))
		return SVEC_DATUM;
	switch (sift_v6_class(typid))
	{
		case SV6C_I32:
			return SVEC_I32;
		case SV6C_F64:
			return SVEC_F64;
		case SV6C_BOOL:
			return SVEC_BOOL;
		case SV6C_TEXT:
			return SVEC_STRVIEW;
		default:
			return SVEC_I64;
	}
}

static Size
elem_size(SiftVecType t)
{
	switch (t)
	{
		case SVEC_I32:
			return sizeof(int32);
		case SVEC_BOOL:
			return sizeof(uint8);
		case SVEC_STRVIEW:
			return sizeof(SiftStrView);
		case SVEC_DATUM:
			return sizeof(Datum);
		default:
			return sizeof(int64);
	}
}

static void
input_init(TwoIn *in, Expr *e, MemoryContext cx)
{
	in->typid = getBaseType(exprType((Node *) e));
	in->type = vec_type(in->typid);
	get_typlenbyval(in->typid, &in->typlen, &in->typbyval);
	in->expr = ExecInitExpr(e, NULL);
	in->data = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * elem_size(in->type) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
}

TwoFold *
sift_two_fold_new(const SiftV6Spec *spec, List *inputs, EState *estate, MemoryContext cx, TupleDesc desc)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	TwoFold    *f = palloc0(sizeof(TwoFold));

	f->group = sift_v6_group_new(spec, cx, desc);
	f->econtext = CreateExprContext(estate);
	f->bcx = sift_mem_allocset_create(cx, "sift agg two cells");
	f->ninputs = list_length(inputs);
	f->in = palloc0(Max(f->ninputs, 1) * sizeof(TwoIn));
	f->vec = palloc0(Max(f->ninputs, 1) * sizeof(SiftVec));
	foreach_ptr(Expr, e, inputs)
		input_init(&f->in[foreach_current_index(e)], e, cx);
	f->keyvec = palloc0(Max(spec->nkeys, 1) * sizeof(SiftVec *));
	for (int i = 0; i < spec->nkeys; i++)
		f->keyvec[i] = &f->vec[spec->key[i].attno - 1];
	f->argvec = palloc0(Max(sift_v6_spec_nvec(spec), 1) * sizeof(SiftVec *));
	for (int j = 0; j < spec->naggs; j++)
		if (sift_v6_kind_reads_column(spec->agg[j].kind))
			f->argvec[j] = &f->vec[spec->agg[j].arg.attno - 1];
	f->bs = palloc_aligned(sizeof(SiftBatchSel), 64, MCXT_ALLOC_ZERO);
	MemoryContextSwitchTo(old);
	return f;
}

static void
put(TwoIn *in, uint16 r, Datum d)
{
	switch (in->type)
	{
		case SVEC_I32:
			((int32 *) in->data)[r] = in->typid == INT2OID ? DatumGetInt16(d) : DatumGetInt32(d);
			break;
		case SVEC_F64:
			((double *) in->data)[r] = in->typid == FLOAT4OID ? DatumGetFloat4(d) : DatumGetFloat8(d);
			break;
		case SVEC_BOOL:
			((uint8 *) in->data)[r] = DatumGetBool(d);
			break;
		case SVEC_STRVIEW:
			sift_v6_result_put(TEXTOID, in->data, r, datumCopy(d, false, -1));
			break;
		case SVEC_DATUM:
			((Datum *) in->data)[r] = datumCopy(d, in->typbyval, in->typlen);
			break;
		default:
			((int64 *) in->data)[r] = DatumGetInt64(d);
			break;
	}
}

static void
flush(TwoFold *f)
{
	SiftBatchSel *bs = f->bs;
	const uint16 n = f->n;

	if (n == 0)
		return;
	memset(bs->mask, 0, sizeof(bs->mask));
	for (uint16 w = 0; w < n / 64; w++)
		bs->mask[w] = ~UINT64CONST(0);
	if (n % 64 != 0)
		bs->mask[n / 64] = (UINT64CONST(1) << (n % 64)) - 1;
	bs->n_raw = bs->n_active = n;
	bs->all_active = true;
	bs->has_sel = false;
	for (int i = 0; i < f->ninputs; i++)
	{
		TwoIn	   *in = &f->in[i];
		const bool	datum = in->type == SVEC_DATUM;

		f->vec[i] = (SiftVec) {.data = in->data,.valid = in->all_valid ? NULL : in->valid,.len = n,.all_valid = in->all_valid,.dense = true,.type = in->type,
		.typid = datum ? in->typid : InvalidOid,.typlen = datum ? in->typlen : 0,.typbyval = datum && in->typbyval};
	}
	sift_v6_group_update(f->group, bs, f->keyvec, f->argvec);
	f->n = 0;
	MemoryContextReset(f->bcx);
}

void
sift_two_fold_cell(TwoFold *f, TupleTableSlot *cell, TupleTableSlot *spine)
{
	const uint16 r = f->n;
	MemoryContext old = MemoryContextSwitchTo(f->bcx);

	f->econtext->ecxt_scantuple = cell;
	f->econtext->ecxt_outertuple = spine;
	for (int i = 0; i < f->ninputs; i++)
	{
		TwoIn	   *in = &f->in[i];
		bool		isnull;
		const Datum d = ExecEvalExpr(in->expr, f->econtext, &isnull);

		if (r == 0)
		{
			memset(in->valid, 0, sizeof(in->valid));
			in->all_valid = true;
		}
		in->all_valid &= !isnull;
		if (isnull)
			continue;
		in->valid[r >> 6] |= UINT64CONST(1) << (r & 63);
		put(in, r, d);
	}
	MemoryContextSwitchTo(old);
	f->cells++;
	if (++f->n == SIFT_VEC_LEN)
		flush(f);
}

SiftV6Group *
sift_two_fold_finish(TwoFold *f)
{
	flush(f);
	sift_v6_group_finish(f->group);
	return f->group;
}

uint64
sift_two_fold_cells(const TwoFold *f)
{
	return f->cells;
}
