/*
 * agg/win/spill.c - the sorted spill of a partition of v6_win_spill.h.
 *
 * A row is a tuple of: for each order key its state (NULL first or last, a NaN before or after the values, as the key says) and its value (an int8, a float8 or the bytes of the cell, whose
 * order is the order of the cell), the number the row came in and the bytes of the row. The sort gives the rows as sort_rows() of v6_win_partsort.c does.
 */
#include "postgres.h"

#include <math.h>

#include "access/tupdesc.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"

#include "agg/batch.h"
#include "agg/win/spill.h"
#include "core/compat.h"
#include "core/memtrack.h"

#define INT2_LT			95
#define INT8_GT			413
#define FLOAT8_GT		674
#define BYTEA_GT		1959
#define INLINE_STR		12
#define CODE_NULL_FIRST	(-2)
#define CODE_NULL_LAST	2

typedef enum KeyKind
{
	KEY_INT8,
	KEY_FLOAT8,
	KEY_BYTEA
} KeyKind;

struct SiftWinSpill
{
	MemoryContext cx;
	MemoryContext row_cx;
	MemoryContext prev_cx;
	SiftWinPsSpec spec;
	TupleDesc	desc;
	Tuplesortstate *ts;
	TupleTableSlot *in;
	TupleTableSlot *out;
	int64		seq;
	size_t		esz[SIFT_V6_MAX_COLS];
	bool		have_prev;
	int16		prev_code[SIFT_WIN_MAX_KEYS];
	Datum		prev_val[SIFT_WIN_MAX_KEYS];
	int64		changes[SIFT_WIN_MAX_KEYS];
};

static KeyKind
key_kind(SiftVecType t)
{
	switch (t)
	{
		case SVEC_I32:
		case SVEC_I64:
		case SVEC_NUM64:
			return KEY_INT8;
		case SVEC_F64:
			return KEY_FLOAT8;
		default:
			return KEY_BYTEA;
	}
}

static KeyKind
ord_kind(const SiftWinSpill *sp, int k)
{
	return key_kind(sp->spec.type[sp->spec.ord[k].slot]);
}

static int16
value_code(const SiftWinPsKey *key)
{
	return key->desc ? 1 : 0;
}

static int16
state_code(const SiftWinPsKey *key, uint8 state)
{
	if (state == SIFT_CELL_STATE_NULL)
		return key->nulls_first ? CODE_NULL_FIRST : CODE_NULL_LAST;
	return (state == SIFT_CELL_STATE_NAN) != key->desc ? 1 : 0;
}

static Datum
bytes_datum(const void *p, size_t len)
{
	bytea	   *b = palloc(VARHDRSZ + len);

	SET_VARSIZE(b, VARHDRSZ + len);
	memcpy(VARDATA(b), p, len);
	return PointerGetDatum(b);
}

static Datum
int128_datum(const char *cell)
{
	SiftI128	v;
	uint8		be[16];

	memcpy(&v, cell, sizeof(v));
	for (int i = 0; i < 8; i++)
	{
		be[i] = (uint8) (((uint64) v.hi ^ (UINT64CONST(1) << 63)) >> (56 - 8 * i));
		be[8 + i] = (uint8) (((uint64) v.lo) >> (56 - 8 * i));
	}
	return bytes_datum(be, sizeof(be));
}

static const char *
str_bytes(const SiftStrView *sv)
{
	return sv->len > INLINE_STR ? sv->u.ptr : (const char *) sv->prefix;
}

/* the value of a cell as its key: what the sort compares */
static Datum
key_datum(SiftVecType t, const char *cell)
{
	switch (t)
	{
		case SVEC_I32:
		{
			int32		v;

			memcpy(&v, cell, sizeof(v));
			return Int64GetDatum(v);
		}
		case SVEC_F64:
		{
			double		v;

			memcpy(&v, cell, sizeof(v));
			return Float8GetDatum(v);
		}
		case SVEC_NUM128:
			return int128_datum(cell);
		case SVEC_STRVIEW:
		{
			SiftStrView sv;

			memcpy(&sv, cell, sizeof(sv));
			return bytes_datum(str_bytes(&sv), sv.len);
		}
		default:
		{
			int64		v;

			memcpy(&v, cell, sizeof(v));
			return Int64GetDatum(v);
		}
	}
}

static Datum
empty_key(KeyKind kind)
{
	return kind == KEY_BYTEA ? bytes_datum("", 0) : Int64GetDatum(0);
}

static bool
key_same(KeyKind kind, Datum a, Datum b)
{
	switch (kind)
	{
		case KEY_INT8:
			return DatumGetInt64(a) == DatumGetInt64(b);
		case KEY_FLOAT8:
			return DatumGetFloat8(a) == DatumGetFloat8(b) || (isnan(DatumGetFloat8(a)) && isnan(DatumGetFloat8(b)));
		default:
			return VARSIZE_ANY_EXHDR(DatumGetPointer(a)) == VARSIZE_ANY_EXHDR(DatumGetPointer(b))
				&& memcmp(VARDATA_ANY(DatumGetPointer(a)), VARDATA_ANY(DatumGetPointer(b)), VARSIZE_ANY_EXHDR(DatumGetPointer(a))) == 0;
	}
}

static Oid
value_type(KeyKind kind)
{
	return kind == KEY_INT8 ? INT8OID : kind == KEY_FLOAT8 ? FLOAT8OID : BYTEAOID;
}

static Oid
value_op(KeyKind kind, bool desc)
{
	switch (kind)
	{
		case KEY_INT8:
			return desc ? INT8_GT : Int8LessOperator;
		case KEY_FLOAT8:
			return desc ? FLOAT8_GT : Float8LessOperator;
		default:
			return desc ? BYTEA_GT : ByteaLessOperator;
	}
}

static TupleDesc
spill_desc(const SiftWinSpill *sp)
{
	const int	n = sp->spec.n_ord;
	TupleDesc	desc = CreateTemplateTupleDesc(2 * n + 2);

	for (int k = 0; k < n; k++)
	{
		TupleDescInitEntry(desc, 2 * k + 1, NULL, INT2OID, -1, 0);
		TupleDescInitEntry(desc, 2 * k + 2, NULL, value_type(ord_kind(sp, k)), -1, 0);
	}
	TupleDescInitEntry(desc, 2 * n + 1, NULL, INT8OID, -1, 0);
	TupleDescInitEntry(desc, 2 * n + 2, NULL, BYTEAOID, -1, 0);
	sift_tupdesc_finalize(desc);
	return desc;
}

SiftWinSpill *
sift_win_spill_begin(MemoryContext parent, const SiftWinPsSpec *spec, uint64 bytes)
{
	MemoryContext cx = sift_mem_allocset_create(parent, "sift window spill");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftWinSpill *sp = palloc0(sizeof(SiftWinSpill));
	const int	nkeys = 2 * spec->n_ord + 1;
	AttrNumber	att[2 * SIFT_WIN_MAX_KEYS + 1];
	Oid			op[2 * SIFT_WIN_MAX_KEYS + 1];
	Oid			coll[2 * SIFT_WIN_MAX_KEYS + 1] = {InvalidOid};
	bool		nulls_first[2 * SIFT_WIN_MAX_KEYS + 1] = {false};

	sp->cx = cx;
	sp->spec = *spec;
	sp->row_cx = AllocSetContextCreate(cx, "sift window spill row", ALLOCSET_SMALL_SIZES);
	sp->prev_cx = AllocSetContextCreate(cx, "sift window spill peer", ALLOCSET_SMALL_SIZES);
	for (int c = 0; c < spec->ncols; c++)
		sp->esz[c] = sift_win_ps_elem_size(spec->type[c]);
	sp->desc = spill_desc(sp);
	for (int k = 0; k < spec->n_ord; k++)
	{
		att[2 * k] = 2 * k + 1;
		op[2 * k] = INT2_LT;
		att[2 * k + 1] = 2 * k + 2;
		op[2 * k + 1] = value_op(ord_kind(sp, k), spec->ord[k].desc);
	}
	att[nkeys - 1] = 2 * spec->n_ord + 1;
	op[nkeys - 1] = Int8LessOperator;
	sp->ts = tuplesort_begin_heap(sp->desc, nkeys, att, op, coll, nulls_first, (int) Max(64, bytes / 1024), NULL,
								  spec->keep > 0 ? TUPLESORT_ALLOWBOUNDED : TUPLESORT_NONE);
	if (spec->keep > 0)
		tuplesort_set_bound(sp->ts, (int64) spec->keep);
	sp->in = MakeSingleTupleTableSlot(sp->desc, &TTSOpsVirtual);
	sp->out = MakeSingleTupleTableSlot(sp->desc, &TTSOpsMinimalTuple);
	MemoryContextSwitchTo(old);
	return sp;
}

static size_t
long_len(SiftVecType t, const char *cell)
{
	SiftStrView sv;

	if (t != SVEC_STRVIEW)
		return 0;
	memcpy(&sv, cell, sizeof(sv));
	return sv.len > INLINE_STR ? sv.len : 0;
}

static Datum
payload_datum(const SiftWinSpill *sp, uint64 tag, const uint8 *state, const char *const *cell)
{
	size_t		len = sizeof(tag);
	bytea	   *b;
	char	   *p;

	for (int c = 0; c < sp->spec.ncols; c++)
		len += 1 + (state[c] == SIFT_CELL_STATE_VALUE ? sp->esz[c] + long_len(sp->spec.type[c], cell[c]) : 0);
	b = palloc(sift_alloc_grow(0, VARHDRSZ + len, "a row of a window that spills"));
	SET_VARSIZE(b, VARHDRSZ + len);
	p = VARDATA(b);
	memcpy(p, &tag, sizeof(tag));
	p += sizeof(tag);
	for (int c = 0; c < sp->spec.ncols; c++)
	{
		const size_t extra = state[c] == SIFT_CELL_STATE_VALUE ? long_len(sp->spec.type[c], cell[c]) : 0;

		*p++ = (char) state[c];
		if (state[c] != SIFT_CELL_STATE_VALUE)
			continue;
		memcpy(p, cell[c], sp->esz[c]);
		p += sp->esz[c];
		if (extra > 0)
		{
			SiftStrView sv;

			memcpy(&sv, cell[c], sizeof(sv));
			memcpy(p, sv.u.ptr, extra);
			p += extra;
		}
	}
	return PointerGetDatum(b);
}

void
sift_win_spill_put(SiftWinSpill *sp, uint64 tag, const uint8 *state, const char *const *cell)
{
	const int	n = sp->spec.n_ord;
	MemoryContext old = MemoryContextSwitchTo(sp->row_cx);

	CHECK_FOR_INTERRUPTS();
	ExecClearTuple(sp->in);
	for (int k = 0; k < n; k++)
	{
		const SiftWinPsKey *key = &sp->spec.ord[k];
		const uint8	st = state[key->slot];

		sp->in->tts_values[2 * k] = Int16GetDatum(state_code(key, st));
		sp->in->tts_values[2 * k + 1] = st == SIFT_CELL_STATE_VALUE ? key_datum(sp->spec.type[key->slot], cell[key->slot]) : empty_key(ord_kind(sp, k));
	}
	sp->in->tts_values[2 * n] = Int64GetDatum(sp->seq++);
	sp->in->tts_values[2 * n + 1] = payload_datum(sp, tag, state, cell);
	memset(sp->in->tts_isnull, 0, sizeof(bool) * (2 * n + 2));
	ExecStoreVirtualTuple(sp->in);
	tuplesort_puttupleslot(sp->ts, sp->in);
	MemoryContextSwitchTo(old);
	MemoryContextReset(sp->row_cx);
}

void
sift_win_spill_sort(SiftWinSpill *sp)
{
	tuplesort_performsort(sp->ts);
}

static void
decode_row(const SiftWinSpill *sp, const bytea *payload, uint16 i, SiftWinSpillOut *out)
{
	const char *p = VARDATA_ANY(payload);

	memcpy(&out->tag[i], p, sizeof(uint64));
	p += sizeof(uint64);
	for (int c = 0; c < sp->spec.ncols; c++)
	{
		char	   *cell = out->col[c] + (size_t) i * sp->esz[c];
		const uint8	st = (uint8) *p++;

		out->state[c][i] = st;
		memset(cell, 0, sp->esz[c]);
		if (st != SIFT_CELL_STATE_VALUE)
			continue;
		memcpy(cell, p, sp->esz[c]);
		p += sp->esz[c];
		if (long_len(sp->spec.type[c], cell) > 0)
		{
			SiftStrView *sv = (SiftStrView *) cell;
			char	   *s = MemoryContextAlloc(out->cx, sv->len);

			memcpy(s, p, sv->len);
			sv->u.ptr = s;
			p += sv->len;
		}
	}
}

static void
count_changes(SiftWinSpill *sp, uint16 i, SiftWinSpillOut *out)
{
	const int	n = sp->spec.n_ord;
	int16		code[SIFT_WIN_MAX_KEYS];
	Datum		val[SIFT_WIN_MAX_KEYS];
	MemoryContext old;

	for (int k = 0; k < n; k++)
	{
		const KeyKind kind = ord_kind(sp, k);

		code[k] = DatumGetInt16(sp->out->tts_values[2 * k]);
		val[k] = sp->out->tts_values[2 * k + 1];
		if (sp->have_prev && (code[k] != sp->prev_code[k] || (code[k] == value_code(&sp->spec.ord[k]) && !key_same(kind, val[k], sp->prev_val[k]))))
			sp->changes[k]++;
		out->peer[k][i] = sp->changes[k];
	}
	MemoryContextReset(sp->prev_cx);
	old = MemoryContextSwitchTo(sp->prev_cx);
	for (int k = 0; k < n; k++)
	{
		sp->prev_code[k] = code[k];
		sp->prev_val[k] = ord_kind(sp, k) == KEY_BYTEA ? datumCopy(val[k], false, -1) : val[k];
	}
	MemoryContextSwitchTo(old);
	sp->have_prev = true;
}

uint16
sift_win_spill_read(SiftWinSpill *sp, uint16 m, SiftWinSpillOut *out)
{
	uint16		i = 0;

	while (i < m && tuplesort_gettupleslot(sp->ts, true, false, sp->out, NULL))
	{
		slot_getallattrs(sp->out);
		count_changes(sp, i, out);
		decode_row(sp, DatumGetByteaPP(sp->out->tts_values[2 * sp->spec.n_ord + 1]), i, out);
		i++;
	}
	return i;
}

void
sift_win_spill_end(SiftWinSpill *sp)
{
	ExecDropSingleTupleTableSlot(sp->in);
	ExecDropSingleTupleTableSlot(sp->out);
	tuplesort_end(sp->ts);
	MemoryContextDelete(sp->cx);
}
