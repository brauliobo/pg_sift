/*
 * check-memory: allow-allocs the arrays are sized by the rows of one test case, a few hundred thousand at most
 * agg/stream_test.c - test-only SQL functions of the exception stream evaluator (agg/variant_eval.c) and of the sorted streaming
 * GROUP BY and Top-N (agg/stream_exec.c), compiled only with SIFT_TESTING and registered by the pg_sift_test extension. Each returns
 * the number of failed cases.
 *
 * Variant: a granule of a float8 or int8 path with exceptions of every kind (numbers that are not of the type, strings, booleans,
 * documents, NULLs, values whose cast raises), a random set of surviving rows; the answer of the typed column and the exception
 * stream (the scan path, with the kernels over the mask the evaluator makes), and of the granule's pre-aggregates and the stream
 * (the pre-aggregate path), against a loop over every row with the cast of each exception written out in a table, and the error
 * raised exactly when a surviving row's cast raises. No relation is opened: nothing could touch a heap.
 * Stream: sorted rows split among 1 to 4 parts in batches with masks, ascending and descending, one and two key columns, NULL keys
 * and values, offset and limit, against groups and rows computed from the sorted rows.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"

#include <math.h>

#include "access/xact.h"
#include "agg/cost.h"
#include "agg/group/state.h"
#include "agg/kern6.h"
#include "agg/stream_exec.h"
#include "agg/variant_eval.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"
#include "utils/resowner.h"
#include "core/memtrack.h"

PG_FUNCTION_INFO_V1(sift_test_variant_eval);
PG_FUNCTION_INFO_V1(sift_test_stream_exec);

typedef __int128 i128;

static uint64 rng_state;

static uint64
rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state * UINT64CONST(0x2545F4914F6CDD1D);
}

enum
{
	OUT_VALUE,
	OUT_NULL,
	OUT_ERROR
};

typedef struct ExcSpec
{
	SiftVariantKind kind;
	const char *text;			/* numeric or jsonb text, or the text of the value itself */
	int			f_out;
	double		f;
	int			i_out;
	int64		i;
} ExcSpec;

/* The cast of each exception to float8 and to int8, written out: what the SQL cast of the text gives */
static const ExcSpec specs[] = {
	{SVAR_NUM_SPECIAL, "NaN", OUT_VALUE, NAN, OUT_ERROR, 0},
	{SVAR_NUM_SPECIAL, "Infinity", OUT_VALUE, INFINITY, OUT_ERROR, 0},
	{SVAR_NUM_SPECIAL, "-Infinity", OUT_VALUE, -INFINITY, OUT_ERROR, 0},
	{SVAR_NUM_SPECIAL, "1496577676626844588240573268701473812127674924007424", OUT_VALUE, 0x1p170, OUT_ERROR, 0},
	{SVAR_NUM_SPECIAL, "2.5", OUT_VALUE, 2.5, OUT_VALUE, 3},
	{SVAR_NUM_SPECIAL, "-3", OUT_VALUE, -3, OUT_VALUE, -3},
	{SVAR_JSONB_SCALAR, "\"7\"", OUT_VALUE, 7, OUT_VALUE, 7},
	{SVAR_JSONB_SCALAR, "\"abc\"", OUT_ERROR, 0, OUT_ERROR, 0},
	{SVAR_JSONB_SCALAR, "true", OUT_ERROR, 0, OUT_ERROR, 0},
	{SVAR_JSONB_SCALAR, "null", OUT_NULL, 0, OUT_NULL, 0},
	{SVAR_JSONB_SCALAR, "2.5", OUT_VALUE, 2.5, OUT_VALUE, 3},
	{SVAR_JSONB_SCALAR, "\"1.5\"", OUT_VALUE, 1.5, OUT_ERROR, 0},
	{SVAR_JSONB_SCALAR, "\"  8 \"", OUT_VALUE, 8, OUT_VALUE, 8},
	{SVAR_JSONB_SCALAR, "1e400", OUT_ERROR, 0, OUT_ERROR, 0},
	{SVAR_JSONB_DOC, "{\"a\": 1}", OUT_ERROR, 0, OUT_ERROR, 0},
	{SVAR_JSONB_DOC, "[1, 2]", OUT_ERROR, 0, OUT_ERROR, 0},
	{SVAR_CAST_ERROR, "xyz", OUT_ERROR, 0, OUT_ERROR, 0},
	{SVAR_CAST_ERROR, "41", OUT_VALUE, 41, OUT_VALUE, 41},
};

#define N_SPECS	((int) lengthof(specs))

/* The payload of a spec: the image of its numeric or jsonb, or its text */
static uint8 *
payload_of(const ExcSpec *sp, uint32 *len)
{
	if (sp->kind == SVAR_CAST_ERROR)
	{
		*len = strlen(sp->text);
		return (uint8 *) pstrdup(sp->text);
	}
	else
	{
		Datum		d = sp->kind == SVAR_NUM_SPECIAL
			? DirectFunctionCall3(numeric_in, CStringGetDatum(sp->text), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1))
			: DirectFunctionCall1(jsonb_in, CStringGetDatum(sp->text));
		uint8	   *p = (uint8 *) DatumGetPointer(d);

		*len = VARSIZE_ANY(p);
		return p;
	}
}

static bool
f8_same(double a, double b)
{
	return (isnan(a) && isnan(b)) || a == b;
}

/* The order of float8: a NaN above every number */
static bool
f8_less(double a, double b)
{
	return !isnan(a) && (isnan(b) || a < b);
}

/* One granule of the path: the answers of the engine and of the loop; true when they agree (an error raised or expected counted) */
static bool
variant_case(void)
{
	const SiftVecType type = rnd() % 2 ? SVEC_F64 : SVEC_I64;
	const uint32 n_rows = 1 + rnd() % SIFT_GRANULE_ROWS,
				mode_active = rnd() % 4;
	const bool	preagg = rnd() % 2;
	const uint32 n_exc = rnd() % 4 == 0 ? 0 : (rnd() % 8 == 0 ? rnd() % 600 : rnd() % 20);
	int64	   *prim = MemoryContextAllocAligned(CurrentMemoryContext, sizeof(int64) * (SIFT_GRANULE_ROWS + 8), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	uint64	   *exc_mask = palloc0(sizeof(uint64) * 128),
			   *active = palloc0(sizeof(uint64) * 128);
	int		   *spec_of = palloc(sizeof(int) * n_rows);
	SiftVariantExcEntry *entries = palloc(sizeof(SiftVariantExcEntry) * (n_exc + 1));
	uint8	   *payload = palloc(1);
	uint32		payload_bytes = 0,
				k = 0;
	SiftVariantExcStream stream;
	SiftVarAgg got,
				want;
	bool		want_err = false,
				got_err = false,
				ok = true;

	for (uint32 r = 0; r < n_rows; r++)
	{
		spec_of[r] = -1;
		if (type == SVEC_F64)
			((double *) prim)[r] = (double) ((int64) (rnd() % 2001) - 1000);
		else
			prim[r] = (int64) (rnd() % 2001) - 1000;
	}
	for (uint32 r = 0; r < n_rows && k < n_exc; r++)
		if (rnd() % n_rows < n_exc)
		{
			const int	si = rnd() % N_SPECS;
			uint32		len;
			uint8	   *p = payload_of(&specs[si], &len);

			spec_of[r] = si;
			exc_mask[r >> 6] |= UINT64CONST(1) << (r & 63);
			entries[k] = (SiftVariantExcEntry) {.row_idx = r, .kind = specs[si].kind, .payload_off = payload_bytes};
			payload = repalloc(payload, payload_bytes + len + 1);
			memcpy(payload + payload_bytes, p, len);
			payload_bytes += len;
			if (type == SVEC_F64)
				((double *) prim)[r] = 1e9;
			else
				prim[r] = 1000000000;
			k++;
		}
	entries[k] = (SiftVariantExcEntry) {.row_idx = 0, .payload_off = payload_bytes};
	stream = (SiftVariantExcStream) {.n_entries = k, .payload_bytes = payload_bytes, .exc_mask = exc_mask, .entries = entries,
									 .payload_buf = payload};
	for (uint32 r = 0; r < n_rows; r++)
		if (preagg || mode_active == 0 || (mode_active == 1 && rnd() % 2) || (mode_active == 2 && rnd() % 20 == 0) ||
			(mode_active == 3 && rnd() % 10 != 0))
			active[r >> 6] |= UINT64CONST(1) << (r & 63);

	/* the loop over every surviving row */
	sift_var_agg_init(&want, type);
	for (uint32 r = 0; r < n_rows; r++)
	{
		const bool	on = (active[r >> 6] >> (r & 63)) & 1;
		const ExcSpec *sp = spec_of[r] >= 0 ? &specs[spec_of[r]] : NULL;
		int			out;
		double		f;
		int64		i;

		if (!on)
			continue;
		want.rows++;
		if (sp)
		{
			out = type == SVEC_F64 ? sp->f_out : sp->i_out;
			f = sp->f;
			i = sp->i;
		}
		else
		{
			out = OUT_VALUE;
			f = ((double *) prim)[r];
			i = prim[r];
		}
		if (out == OUT_ERROR)
			want_err = true;
		if (out != OUT_VALUE)
			continue;
		if (type == SVEC_F64)
		{
			want.fsum += f;
			if (want.count == 0 || f8_less(f, want.min.f))
				want.min.f = f;
			if (want.count == 0 || f8_less(want.max.f, f))
				want.max.f = f;
		}
		else
		{
			sift_i128_add(&want.isum, sift_i128_of_i64(i));
			if (want.count == 0 || i < want.min.i)
				want.min.i = i;
			if (want.count == 0 || i > want.max.i)
				want.max.i = i;
		}
		want.count++;
	}

	/* the engine, an error caught as a subtransaction would */
	{
		MemoryContext oldcxt = CurrentMemoryContext;
		ResourceOwner oldowner = CurrentResourceOwner;

		sift_var_agg_init(&got, type);
		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(oldcxt);
		PG_TRY();
		{
			if (preagg)
			{
				SiftVarAgg footer;
				SiftVec		col = {.data = prim, .len = 0, .all_valid = true, .dense = true, .type = type};
				int64	   *typed = palloc(sizeof(int64) * n_rows);

				sift_var_agg_init(&footer, type);
				for (uint32 r = 0; r < n_rows; r++)
					if (spec_of[r] < 0)
						typed[col.len++] = prim[r];
				col.data = typed;
				sift_var_agg_add_vec(&footer, &col);
				footer.rows = footer.count;
				sift_var_agg_merge(&got, &footer);
				sift_variant_fold_granule(&got, k ? &stream : NULL, NULL, n_rows);
			}
			else
				for (uint32 slice = 0; slice * SIFT_VEC_LEN < n_rows; slice++)
				{
					const uint16 n = (uint16) Min(SIFT_VEC_LEN, n_rows - slice * SIFT_VEC_LEN);
					uint64		mask[SIFT_VEC_WORDS] __attribute__((aligned(64)));
					SiftVariantVals *vals = sift_variant_vals_new();
					uint16		left;
					const int64 *col = prim + (size_t) slice * SIFT_VEC_LEN;
					bool		first = got.count == 0;

					memcpy(mask, active + (size_t) slice * SIFT_VEC_WORDS, sizeof(mask));
					left = sift_variant_primary_mask(mask, k ? &stream : NULL, slice, n);
					got.rows += left;
					if (left)
					{
						if (type == SVEC_F64)
						{
							const double *fc = (const double *) col;

							got.fsum += sift_aggk6->sum_f64_fast(fc, mask, n, 0.0);
							for (uint16 i = 0; i < n; i++)
								if ((mask[i >> 6] >> (i & 63)) & 1)
								{
									if (first || f8_less(fc[i], got.min.f))
										got.min.f = fc[i];
									if (first || f8_less(got.max.f, fc[i]))
										got.max.f = fc[i];
									first = false;
								}
						}
						else
						{
							got.isum = sift_aggk6->sum_i64_to_i128(col, mask, n, got.isum);
							for (uint16 i = 0; i < n; i++)
								if ((mask[i >> 6] >> (i & 63)) & 1)
								{
									if (first || col[i] < got.min.i)
										got.min.i = col[i];
									if (first || col[i] > got.max.i)
										got.max.i = col[i];
									first = false;
								}
						}
						got.count += left;
					}
					sift_variant_eval(vals, k ? &stream : NULL, slice, active + (size_t) slice * SIFT_VEC_WORDS, n, type);
					got.rows += vals->n_active;
					{
						SiftVarAgg ex;

						sift_var_agg_init(&ex, type);
						sift_var_agg_add_vec(&ex, &vals->vec);
						sift_var_agg_merge(&got, &ex);
					}
					pfree(vals);
				}
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcxt);
			CurrentResourceOwner = oldowner;
		}
		PG_CATCH();
		{
			ErrorData  *ed;

			MemoryContextSwitchTo(oldcxt);
			ed = CopyErrorData();
			FlushErrorState();
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcxt);
			CurrentResourceOwner = oldowner;
			got_err = true;
			FreeErrorData(ed);
		}
		PG_END_TRY();
	}
	if (got_err || want_err)
		ok = got_err == want_err;
	else if (type == SVEC_F64)
		ok = got.rows == want.rows && got.count == want.count && f8_same(got.fsum, want.fsum) &&
			(want.count == 0 || (f8_same(got.min.f, want.min.f) && f8_same(got.max.f, want.max.f)));
	else
		ok = got.rows == want.rows && got.count == want.count && got.isum.lo == want.isum.lo && got.isum.hi == want.isum.hi &&
			(want.count == 0 || (got.min.i == want.min.i && got.max.i == want.max.i));
	return ok;
}

/* Number of failed cases */
Datum
sift_test_variant_eval(PG_FUNCTION_ARGS)
{
	int64		bad = 0;

	rng_state = PG_GETARG_INT64(0) * UINT64CONST(0x9E3779B97F4A7C15) | 1;
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		MemoryContext cxt = sift_mem_allocset_create(CurrentMemoryContext, "variant case");
		MemoryContext old = MemoryContextSwitchTo(cxt);

		CHECK_FOR_INTERRUPTS();
		bad += !variant_case();
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
	}
	PG_RETURN_INT64(bad);
}

/* ---- the streaming GROUP BY and Top-N ---- */

typedef struct TRow
{
	int64		k[SIFT_STREAM_MAX_KEYS];
	bool		knull[SIFT_STREAM_MAX_KEYS];
	int64		val;
	bool		vnull;
	bool		active;
} TRow;

static uint8 test_nkeys;

static int
trow_cmp(const void *a, const void *b)
{
	const TRow *x = a,
			   *y = b;

	for (uint8 i = 0; i < test_nkeys; i++)
	{
		if (x->knull[i] != y->knull[i])
			return x->knull[i] ? 1 : -1;
		if (!x->knull[i] && x->k[i] != y->k[i])
			return x->k[i] < y->k[i] ? -1 : 1;
	}
	return 0;
}

typedef struct TBatch
{
	SiftVec		keys[SIFT_STREAM_MAX_KEYS];
	SiftVec		val;
	uint64		mask[SIFT_VEC_WORDS];
	bool		all_active;
	uint16		n;
} TBatch;

typedef struct TSrc
{
	TBatch	   *batches;
	uint32		n,
				i;
} TSrc;

static bool
tsrc_next(void *src, SiftStreamBatch *out)
{
	TSrc	   *s = src;
	TBatch	   *b;

	if (s->i >= s->n)
		return false;
	b = &s->batches[s->i++];
	out->n = b->n;
	for (int i = 0; i < SIFT_STREAM_MAX_KEYS; i++)
		out->keys[i] = b->keys[i].data ? &b->keys[i] : NULL;
	out->nvals = 1;
	out->val[0] = &b->val;
	out->filter[0] = NULL;
	out->mask = b->all_active ? NULL : b->mask;
	return true;
}

static void
fill_col(SiftVec *v, SiftVecType type, const TRow *rows, uint32 n, bool is_key, uint8 col)
{
	bool		any_null = false;

	v->type = type;
	v->len = n;
	v->dense = true;
	v->data = type == SVEC_I32 ? palloc(sizeof(int32) * (n + 16)) : palloc(sizeof(int64) * (n + 8));
	v->valid = palloc0(sizeof(uint64) * SIFT_VEC_WORDS);
	for (uint32 r = 0; r < n; r++)
	{
		const bool	isnull = is_key ? rows[r].knull[col] : rows[r].vnull;
		const int64 x = is_key ? rows[r].k[col] : rows[r].val;

		any_null |= isnull;
		if (type == SVEC_I32)
			((int32 *) v->data)[r] = isnull ? 0 : (int32) x;
		else
			((int64 *) v->data)[r] = isnull ? 0 : x;
		if (!isnull)
			v->valid[r >> 6] |= UINT64CONST(1) << (r & 63);
	}
	v->all_valid = !any_null;
	if (v->all_valid)
		v->valid = NULL;
}

typedef struct Collect
{
	SiftStreamGroup *groups;
	SiftStreamRow *rows;
	uint32		n;
} Collect;

static void
collect_group(void *arg, const SiftStreamGroup *g)
{
	Collect    *c = arg;

	c->groups[c->n++] = *g;
}

static void
collect_row(void *arg, const SiftStreamRow *r)
{
	Collect    *c = arg;

	c->rows[c->n++] = *r;
}

static bool
stream_case(void)
{
	const uint8 nkeys = 1 + rnd() % 2;
	const bool	desc = rnd() % 2,
				rows_mode = rnd() % 3 == 0;
	const uint32 n = 1 + rnd() % (rnd() % 8 == 0 ? 30000 : 3000),
				nparts = 1 + rnd() % 4;
	const int64 kr = 1 + rnd() % (rnd() % 2 ? 6 : 60);
	const SiftVecType ktype[SIFT_STREAM_MAX_KEYS] = {rnd() % 2 ? SVEC_I32 : SVEC_I64, rnd() % 2 ? SVEC_I32 : SVEC_I64, SVEC_I64, SVEC_I64},
				vtype = rnd() % 2 ? SVEC_I32 : SVEC_I64;
	TRow	   *rows = palloc(sizeof(TRow) * n);
	uint32		nr = n,
				first = 0,
				last;
	TSrc	   *srcs = palloc0(sizeof(TSrc) * nparts);
	uint32	   *np = palloc0(sizeof(uint32) * nparts);
	TRow	  **per = palloc(sizeof(TRow *) * nparts);
	SiftStreamGroup *want_groups = palloc_aligned(sizeof(SiftStreamGroup) * n, _Alignof(SiftStreamGroup), 0);
	SiftStreamRow *want_rows = palloc(sizeof(SiftStreamRow) * n);
	uint32		nwant = 0;
	SiftStreamExec se;
	Collect		got;
	uint64		offset,
				limit;
	bool		ok = true;

	test_nkeys = nkeys;
	for (uint32 i = 0; i < n; i++)
	{
		memset(&rows[i], 0, sizeof(TRow));
		for (uint8 c = 0; c < nkeys; c++)
		{
			rows[i].knull[c] = rnd() % 10 == 0;
			rows[i].k[c] = rows[i].knull[c] ? 0 : (int64) (rnd() % (2 * kr + 1)) - kr;
		}
		rows[i].vnull = rnd() % 10 == 0;
		rows[i].val = rows[i].vnull ? 0 : (vtype == SVEC_I32 ? (int64) (rnd() % 2000001) - 1000000 : (int64) rnd() >> (rnd() % 64));
		rows[i].active = rnd() % 7 != 0;
	}
	qsort(rows, n, sizeof(TRow), trow_cmp);
	if (rows_mode)
	{
		uint32		w = 0;

		for (uint32 i = 0; i < n; i++)
			if (i == 0 || trow_cmp(&rows[i], &rows[w - 1]))
				rows[w++] = rows[i];
		nr = w;
	}
	if (desc)
		for (uint32 i = 0; i < nr / 2; i++)
		{
			const TRow	t = rows[i];

			rows[i] = rows[nr - 1 - i];
			rows[nr - 1 - i] = t;
		}

	/* the expected groups or rows, in the order of the sorted rows */
	for (uint32 i = 0; i < nr; i++)
	{
		if (!rows[i].active)
			continue;
		if (rows_mode)
		{
			SiftStreamRow *r = &want_rows[nwant++];

			memset(r, 0, sizeof(*r));
			for (uint8 c = 0; c < nkeys; c++)
			{
				r->key[c] = rows[i].k[c];
				r->key_nulls |= (uint32) rows[i].knull[c] << c;
			}
			r->val = rows[i].val;
			r->val_null = rows[i].vnull;
			continue;
		}
		if (nwant == 0 || trow_cmp(&rows[i], &rows[first]))
		{
			SiftStreamGroup *g = &want_groups[nwant++];

			memset(g, 0, sizeof(*g));
			for (uint8 c = 0; c < nkeys; c++)
			{
				g->key[c] = rows[i].k[c];
				g->key_nulls |= (uint32) rows[i].knull[c] << c;
			}
			first = i;
		}
		{
			SiftStreamGroup *g = &want_groups[nwant - 1];
			SiftStreamVal *v = &g->v[0];
			i128		sum = ((i128) v->sum.hi << 64) | v->sum.lo;

			g->rows++;
			if (!rows[i].vnull)
			{
				v->min = v->n ? Min(v->min, rows[i].val) : rows[i].val;
				v->max = v->n ? Max(v->max, rows[i].val) : rows[i].val;
				v->n++;
				sum += rows[i].val;
				v->sum.lo = (uint64) sum;
				v->sum.hi = (int64) (sum >> 64);
			}
		}
	}

	/* the parts: a random subsequence each, in batches of random sizes */
	for (uint32 p = 0; p < nparts; p++)
		per[p] = palloc(sizeof(TRow) * (nr + 1));
	for (uint32 i = 0; i < nr; i++)
	{
		const uint32 p = rnd() % nparts;

		per[p][np[p]++] = rows[i];
	}
	for (uint32 p = 0; p < nparts; p++)
	{
		srcs[p].batches = palloc(sizeof(TBatch) * (np[p] + 1));
		for (uint32 at = 0; at < np[p];)
		{
			const uint32 want = 1 + rnd() % (rnd() % 2 ? 50 : SIFT_VEC_LEN),
						len = Min(np[p] - at, want);
			TBatch	   *b = &srcs[p].batches[srcs[p].n++];

			memset(b, 0, sizeof(*b));
			b->n = len;
			b->all_active = true;
			for (uint32 r = 0; r < len; r++)
				if (per[p][at + r].active)
					b->mask[r >> 6] |= UINT64CONST(1) << (r & 63);
				else
					b->all_active = false;
			for (uint8 c = 0; c < nkeys; c++)
				fill_col(&b->keys[c], ktype[c], per[p] + at, len, true, c);
			fill_col(&b->val, vtype, per[p] + at, len, false, 0);
			at += len;
		}
	}

	offset = rnd() % 3 == 0 ? 0 : rnd() % (nwant + 2);
	limit = rnd() % 3 == 0 ? SIFT_STREAM_NO_LIMIT : rnd() % (nwant + 2);
	got.groups = palloc_aligned(sizeof(SiftStreamGroup) * (nwant + 1), _Alignof(SiftStreamGroup), 0);
	got.rows = palloc(sizeof(SiftStreamRow) * (nwant + 1));
	got.n = 0;
	sift_stream_init(&se, nkeys, desc, rows_mode, offset, limit, collect_group, collect_row, &got);
	for (uint32 p = 0; p < nparts; p++)
		sift_stream_add_source(&se, tsrc_next, &srcs[p]);
	sift_stream_run(&se);
	sift_stream_free(&se);

	first = Min(offset, nwant);
	last = limit == SIFT_STREAM_NO_LIMIT ? nwant : (uint32) Min((uint64) nwant, offset + limit);
	ok = got.n == last - first;
	for (uint32 i = 0; ok && i < got.n; i++)
		if (rows_mode)
			ok = !memcmp(&got.rows[i].key, &want_rows[first + i].key, sizeof(int64) * nkeys) &&
				got.rows[i].key_nulls == want_rows[first + i].key_nulls && got.rows[i].val_null == want_rows[first + i].val_null &&
				got.rows[i].val == want_rows[first + i].val;
		else
			ok = !memcmp(got.groups[i].key, want_groups[first + i].key, sizeof(int64) * nkeys) &&
				got.groups[i].key_nulls == want_groups[first + i].key_nulls && got.groups[i].rows == want_groups[first + i].rows &&
				got.groups[i].v[0].n == want_groups[first + i].v[0].n && got.groups[i].v[0].sum.lo == want_groups[first + i].v[0].sum.lo &&
				got.groups[i].v[0].sum.hi == want_groups[first + i].v[0].sum.hi &&
				(got.groups[i].v[0].n == 0 || (got.groups[i].v[0].min == want_groups[first + i].v[0].min && got.groups[i].v[0].max == want_groups[first + i].v[0].max));
	return ok;
}

/* A part of 100 batches of unique keys and a limit of 10 rows: the stream stops after the first batch */
static bool
stream_stops_early(void)
{
	TRow	   *rows = palloc(sizeof(TRow) * SIFT_VEC_LEN);
	TSrc		src = {palloc(sizeof(TBatch) * 100), 100, 0};
	SiftStreamExec se;
	Collect		got = {palloc(1), palloc(sizeof(SiftStreamRow) * 10), 0};

	for (uint32 b = 0; b < 100; b++)
	{
		for (uint32 r = 0; r < SIFT_VEC_LEN; r++)
		{
			memset(&rows[r], 0, sizeof(TRow));
			rows[r].k[0] = (int64) b * SIFT_VEC_LEN + r;
			rows[r].val = r;
			rows[r].active = true;
		}
		memset(&src.batches[b], 0, sizeof(TBatch));
		src.batches[b].n = SIFT_VEC_LEN;
		src.batches[b].all_active = true;
		fill_col(&src.batches[b].keys[0], SVEC_I64, rows, SIFT_VEC_LEN, true, 0);
		fill_col(&src.batches[b].val, SVEC_I64, rows, SIFT_VEC_LEN, false, 0);
	}
	sift_stream_init(&se, 1, false, true, 0, 10, NULL, collect_row, &got);
	sift_stream_add_source(&se, tsrc_next, &src);
	sift_stream_run(&se);
	sift_stream_free(&se);
	return got.n == 10 && src.i == 1 && got.rows[9].key[0] == 9;
}

/* Two groups of one key column: INT64_MAX three times and INT64_MIN once (a sum beyond 64 bits, both ways), and NULL values only (no sum, no minimum) */
static bool
stream_sum_edges(void)
{
	static const int64 key[] = {1, 1, 1, 1, 2, 2, 3, 3, 3, 3};
	static const int64 val[] = {PG_INT64_MAX, PG_INT64_MAX, PG_INT64_MAX, 0, 0, 0, PG_INT64_MIN, PG_INT64_MIN, PG_INT64_MIN, 5};
	static const bool vnull[] = {false, false, false, true, true, true, false, false, false, false};
	TRow		rows[lengthof(key)];
	TSrc		src = {palloc(sizeof(TBatch)), 1, 0};
	SiftStreamExec se;
	Collect		got = {palloc_aligned(sizeof(SiftStreamGroup) * 3, _Alignof(SiftStreamGroup), 0), palloc(1), 0};
	const SiftStreamGroup *g = got.groups;

	for (uint32 r = 0; r < lengthof(key); r++)
	{
		memset(&rows[r], 0, sizeof(TRow));
		rows[r].k[0] = key[r];
		rows[r].val = val[r];
		rows[r].vnull = vnull[r];
		rows[r].active = true;
	}
	memset(src.batches, 0, sizeof(TBatch));
	src.batches[0].n = lengthof(key);
	src.batches[0].all_active = true;
	fill_col(&src.batches[0].keys[0], SVEC_I64, rows, lengthof(key), true, 0);
	fill_col(&src.batches[0].val, SVEC_I64, rows, lengthof(key), false, 0);
	sift_stream_init(&se, 1, false, false, 0, SIFT_STREAM_NO_LIMIT, collect_group, NULL, &got);
	sift_stream_add_source(&se, tsrc_next, &src);
	sift_stream_run(&se);
	sift_stream_free(&se);
	return got.n == 3 &&
		g[0].rows == 4 && g[0].v[0].n == 3 && g[0].v[0].sum.hi == 1 && g[0].v[0].sum.lo == UINT64CONST(0x7FFFFFFFFFFFFFFD) && g[0].v[0].min == PG_INT64_MAX &&
		g[1].rows == 2 && g[1].v[0].n == 0 &&
		g[2].rows == 4 && g[2].v[0].n == 4 && g[2].v[0].sum.hi == -2 && g[2].v[0].sum.lo == UINT64CONST(0x8000000000000005) && g[2].v[0].min == PG_INT64_MIN && g[2].v[0].max == 5;
}

/* Number of failed cases */
Datum
sift_test_stream_exec(PG_FUNCTION_ARGS)
{
	int64		bad = 0;

	rng_state = PG_GETARG_INT64(0) * UINT64CONST(0x9E3779B97F4A7C15) | 1;
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		MemoryContext cxt = sift_mem_allocset_create(CurrentMemoryContext, "stream case");
		MemoryContext old = MemoryContextSwitchTo(cxt);

		CHECK_FOR_INTERRUPTS();
		bad += !stream_case();
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
	}
	bad += !stream_stops_early();
	bad += !stream_sum_edges();
	PG_RETURN_INT64(bad);
}

#endif							/* SIFT_TESTING */
