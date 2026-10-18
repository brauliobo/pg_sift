/*
 * sift_shred_test.c - test-only SQL functions sift._test_shred* (compiled only with SIFT_TESTING, registered by the pg_sift_test
 * extension): the shredder output as rows, a throughput entry point, and the SIMD kernels checked against the scalar ones.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "common/pg_prng.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/jsonb.h"

#include "sift_shred_pg.h"

PG_FUNCTION_INFO_V1(sift_test_shred_spec);
PG_FUNCTION_INFO_V1(sift_test_shred);
PG_FUNCTION_INFO_V1(sift_test_shred_count);
PG_FUNCTION_INFO_V1(sift_test_shred_summary);
PG_FUNCTION_INFO_V1(sift_test_shred_kern_level);
PG_FUNCTION_INFO_V1(sift_test_shred_kern_digest);
PG_FUNCTION_INFO_V1(sift_test_shred_kern_equiv);
PG_FUNCTION_INFO_V1(sift_test_shred_kern_rate);

static const char *const state_names[] = {"absent", "null", "value", "overflow"};

/* ---------- the spec ---------- */

/* The canonical form of a spec (errors of the parser); the fingerprint is the second column of sift._test_shred_summary's first row */
Datum
sift_test_shred_spec(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(sift_shred_spec_deparse(sift_shred_spec_parse(text_to_cstring(PG_GETARG_TEXT_PP(0))))));
}

/* ---------- the output as rows ---------- */

static void
render_value(StringInfo out, const SiftShred *sh, const SiftShredColDef *cd, const SiftShredVal *v)
{
	char		buf[32];

	switch (cd->type)
	{
		case SIFT_SHRED_BOOL:
			appendStringInfoString(out, v->i ? "true" : "false");
			break;
		case SIFT_SHRED_FLOAT4:
			float_to_shortest_decimal_buf((float4) v->f, buf);
			appendStringInfoString(out, buf);
			break;
		case SIFT_SHRED_FLOAT8:
			double_to_shortest_decimal_buf(v->f, buf);
			appendStringInfoString(out, buf);
			break;
		case SIFT_SHRED_TEXT:
			appendBinaryStringInfo(out, sh->doc.text + v->s.off, v->s.len);
			break;
		default:
			appendStringInfo(out, INT64_FORMAT, v->i);
	}
}

static void
put_row(ReturnSetInfo *rsi, const char *kind, const char *path, int parent, int ord, const char *key, const char *state,
		const char *val)
{
	Datum		d[7];
	bool		n[7] = {false, false, parent < 0, ord < 0, key == NULL, false, val == NULL};

	d[0] = CStringGetTextDatum(kind);
	d[1] = CStringGetTextDatum(path);
	d[2] = Int32GetDatum(parent);
	d[3] = Int32GetDatum(ord);
	d[4] = key ? CStringGetTextDatum(key) : (Datum) 0;
	d[5] = CStringGetTextDatum(state);
	d[6] = val ? CStringGetTextDatum(val) : (Datum) 0;
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, d, n);
}

/*
 * sift._test_shred(doc jsonb, spec text, max_elems int4, max_bytes int4): one row per slot of every column ('col': ord = element
 * ordinal in its group, parent = ordinal of the parent element (NULL in the root group), key = member name of a map) and one per
 * list under every parent element ('list': ord = first element, val = number of elements, state = the list's state).
 */
Datum
sift_test_shred(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	SiftShredSpec *spec = sift_shred_spec_parse(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	SiftShredLimits lim = {PG_GETARG_INT32(2), PG_GETARG_INT32(3), 0};
	SiftShred  *sh = sift_shred_create(spec, &lim);
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	StringInfoData val;

	InitMaterializedSRF(fcinfo, 0);
	sift_shred_jsonb(sh, jb);
	initStringInfo(&val);
	for (int c = 0; c < spec->ncols; c++)
	{
		const SiftShredColDef *cd = &spec->cols[c];
		const SiftShredGroup *gr = &sh->doc.grp[cd->group];
		const SiftShredCol *col = &sh->doc.col[c];
		uint32	   *parent = palloc(gr->n * sizeof(uint32) + 1);

		if (cd->group != 0)
			sift_shred_expand_parents(&sh->doc, cd->group, parent);
		for (uint32 i = 0; i < gr->n; i++)
		{
			const char *key = NULL;
			uint32		klen;

			if (spec->groups[cd->group].kind == SIFT_SHRED_MAP && gr->keyid[i] != SIFT_SHRED_NO_KEY)
			{
				const char *k = sift_shred_dict_key(sh->dict, gr->keyid[i], &klen);

				key = pnstrdup(k, klen);
			}
			resetStringInfo(&val);
			if (col->state[i] == SIFT_SHRED_VALUE)
				render_value(&val, sh, cd, &col->val[i]);
			put_row(rsi, "col", cd->name, cd->group ? (int) parent[i] : -1, i, key, state_names[col->state[i]],
					col->state[i] == SIFT_SHRED_VALUE ? val.data : NULL);
		}
		pfree(parent);
	}
	for (int g = 1; g < spec->ngroups; g++)
	{
		const SiftShredGroup *gr = &sh->doc.grp[g];

		for (uint32 p = 0; p < gr->pn; p++)
		{
			uint32		lo,
						hi;
			char		cnt[16];

			sift_shred_group_range(&sh->doc, g, p, &lo, &hi);
			snprintf(cnt, sizeof(cnt), "%u", hi - lo);
			put_row(rsi, "list", spec->groups[g].name, p, lo, NULL, state_names[gr->lstate[p]], cnt);
		}
	}
	/* the overflow reasons of each column, after the rows */
	for (int c = 0; c < spec->ncols; c++)
		if (sh->doc.col[c].ovf)
		{
			char		r[16];

			snprintf(r, sizeof(r), "%u", sh->doc.col[c].ovf);
			put_row(rsi, "ovf", spec->cols[c].name, -1, -1, NULL, "overflow", r);
		}
	sift_shred_free(sh);
	return (Datum) 0;
}

/* ---------- throughput and column summaries (through the kernels) ---------- */

typedef struct CountCache
{
	char	   *spec_text;
	SiftShred  *sh;
} CountCache;

/* The number of values of one document; the shredder is kept between calls for the same spec text */
Datum
sift_test_shred_count(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	char	   *spec_text = text_to_cstring(PG_GETARG_TEXT_PP(1));
	CountCache *cc = (CountCache *) fcinfo->flinfo->fn_extra;
	int64		total = 0;

	if (cc == NULL || strcmp(cc->spec_text, spec_text) != 0)
	{
		MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		if (cc)
			sift_shred_free(cc->sh);
		else
			cc = palloc(sizeof(CountCache));
		cc->spec_text = pstrdup(spec_text);
		cc->sh = sift_shred_create(sift_shred_spec_parse(spec_text), NULL);
		fcinfo->flinfo->fn_extra = cc;
		MemoryContextSwitchTo(old);
	}
	sift_shred_jsonb(cc->sh, jb);
	for (int c = 0; c < cc->sh->spec->ncols; c++)
	{
		uint32		counts[4];

		sift_shred_kern->state_counts(cc->sh->doc.col[c].state, cc->sh->doc.grp[cc->sh->spec->cols[c].group].n, counts);
		total += counts[SIFT_SHRED_VALUE];
	}
	PG_RETURN_INT64(total);
}

/*
 * sift._test_shred_summary(doc, spec): per column the counts of the four states and the minimum and maximum of the values,
 * from the SIMD kernels of the active level (the differential test compares them with SQL aggregates).
 */
Datum
sift_test_shred_summary(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	SiftShredSpec *spec = sift_shred_spec_parse(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	SiftShred  *sh = sift_shred_create(spec, NULL);
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	sift_shred_jsonb(sh, jb);
	for (int c = 0; c < spec->ncols; c++)
	{
		const SiftShredColDef *cd = &spec->cols[c];
		const SiftShredCol *col = &sh->doc.col[c];
		uint32		n = sh->doc.grp[cd->group].n,
					counts[4];
		Datum		d[7];
		bool		nul[7] = {false, false, false, false, false, true, true};
		char		lo[40],
					hi[40];
		bool		found = false;

		sift_shred_kern->state_counts(col->state, n, counts);
		if (cd->type == SIFT_SHRED_FLOAT4 || cd->type == SIFT_SHRED_FLOAT8)
		{
			double		mn,
						mx;

			if ((found = sift_shred_kern->minmax_f64(col->state, (const double *) col->val, n, &mn, &mx)))
			{
				double_to_shortest_decimal_buf(mn, lo);
				double_to_shortest_decimal_buf(mx, hi);
			}
		}
		else if (cd->type != SIFT_SHRED_TEXT)
		{
			int64		mn,
						mx;

			if ((found = sift_shred_kern->minmax_i64(col->state, (const int64 *) col->val, n, &mn, &mx)))
			{
				snprintf(lo, sizeof(lo), INT64_FORMAT, mn);
				snprintf(hi, sizeof(hi), INT64_FORMAT, mx);
			}
		}
		d[0] = CStringGetTextDatum(cd->name);
		for (int s = 0; s < 4; s++)
			d[1 + s] = Int32GetDatum(counts[s]);
		if (found)
		{
			d[5] = CStringGetTextDatum(lo);
			d[6] = CStringGetTextDatum(hi);
			nul[5] = nul[6] = false;
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, d, nul);
	}
	sift_shred_free(sh);
	return (Datum) 0;
}

/* ---------- kernels: scalar is the specification ---------- */

#define KN_MAX	1200
#define KN_P	48

typedef struct KCase
{
	uint32		n;
	uint8		state[KN_MAX + 64];
	int64		ival[KN_MAX + 8];
	double		fval[KN_MAX + 8];
	uint32		np;
	uint32		end[KN_P];
} KCase;

typedef struct KOut
{
	uint32		counts[4];
	int64		imin,
				imax;
	double		fmin,
				fmax;
	bool		ifound,
				ffound;
	uint32		parent[KN_MAX + 64];
	uint64		nulls[KN_MAX / 64 + 2];
	uint64		ovf[KN_MAX / 64 + 2];
} KOut;

static void
gen_kcase(pg_prng_state *rs, KCase *c)
{
	int			shape = pg_prng_uint32(rs) % 4;
	uint32		lim = shape == 0 ? 20 : shape == 1 ? 100 : shape == 2 ? 300 : KN_MAX;
	uint32		e = 0;

	memset(c, 0, sizeof(*c));
	c->n = pg_prng_uint32(rs) % (lim + 1);
	for (uint32 i = 0; i < c->n; i++)
	{
		c->state[i] = shape == 0 ? pg_prng_uint32(rs) % 4 : (pg_prng_uint32(rs) % 8 ? SIFT_SHRED_VALUE : pg_prng_uint32(rs) % 4);
		switch (pg_prng_uint32(rs) % 5)
		{
			case 0:
				c->ival[i] = (int64) pg_prng_uint64(rs);
				break;
			case 1:
				c->ival[i] = pg_prng_uint32(rs) % 2 ? PG_INT64_MAX : PG_INT64_MIN;
				break;
			default:
				c->ival[i] = (int64) (pg_prng_uint32(rs) % 2001) - 1000;
		}
		c->fval[i] = (pg_prng_double(rs) - 0.5) * (pg_prng_uint32(rs) % 3 == 0 ? 1e300 : 1000.0);
		if (c->fval[i] == 0)
			c->fval[i] = 0.0;
	}
	c->np = pg_prng_uint32(rs) % (KN_P + 1);
	for (uint32 p = 0; p < c->np; p++)
	{
		e += pg_prng_uint32(rs) % 3 == 0 ? 0 : pg_prng_uint32(rs) % (shape == 3 ? 40 : 12);
		c->end[p] = Min(e, KN_MAX);
		e = c->end[p];
	}
}

static void
run_kcase(const SiftShredKernels *k, const KCase *c, KOut *o)
{
	memset(o, 0, sizeof(*o));
	k->state_counts(c->state, c->n, o->counts);
	o->ifound = k->minmax_i64(c->state, c->ival, c->n, &o->imin, &o->imax);
	o->ffound = k->minmax_f64(c->state, c->fval, c->n, &o->fmin, &o->fmax);
	if (c->np > 0)
		k->expand_parents(c->end, c->np, o->parent);
	k->state_bits(c->state, c->n, o->nulls, o->ovf);
}

/* Only what the kernels promise to write is compared: the parents up to the last end */
static bool
kout_equal(const KCase *c, const KOut *a, const KOut *b)
{
	uint32		np = c->np > 0 ? c->end[c->np - 1] : 0;

	return memcmp(a->counts, b->counts, sizeof(a->counts)) == 0 && a->ifound == b->ifound && a->ffound == b->ffound &&
		(!a->ifound || (a->imin == b->imin && a->imax == b->imax)) &&
		(!a->ffound || (memcmp(&a->fmin, &b->fmin, 8) == 0 && memcmp(&a->fmax, &b->fmax, 8) == 0)) &&
		memcmp(a->parent, b->parent, np * sizeof(uint32)) == 0 &&
		memcmp(a->nulls, b->nulls, (c->n + 63) / 64 * sizeof(uint64)) == 0 && memcmp(a->ovf, b->ovf, (c->n + 63) / 64 * sizeof(uint64)) == 0;
}

static uint64
kout_hash(const KCase *c, const KOut *o, uint64 h)
{
	uint32		np = c->np > 0 ? c->end[c->np - 1] : 0;
	const uint8 *p[] = {(const uint8 *) o->counts, (const uint8 *) &o->imin, (const uint8 *) &o->imax,
	(const uint8 *) &o->fmin, (const uint8 *) &o->fmax, (const uint8 *) o->parent, (const uint8 *) o->nulls, (const uint8 *) o->ovf};
	size_t		len[] = {sizeof(o->counts), 8 * o->ifound, 8 * o->ifound, 8 * o->ffound, 8 * o->ffound, np * sizeof(uint32),
	(c->n + 63) / 64 * sizeof(uint64), (c->n + 63) / 64 * sizeof(uint64)};

	for (int k = 0; k < 8; k++)
		for (size_t j = 0; j < len[k]; j++)
			h = (h ^ p[k][j]) * UINT64CONST(0x100000001B3);
	return (h ^ o->ifound ^ (o->ffound << 1)) * UINT64CONST(0x100000001B3);
}

/* The level of the kernels in use: it follows sift.simd (capped by the CPU) */
Datum
sift_test_shred_kern_level(PG_FUNCTION_ARGS)
{
	sift_shred_init();
	for (int level = SIFT_SIMD_AVX512; level >= SIFT_SIMD_SCALAR; level--)
		if (sift_shred_kernels_for((SiftSimdLevel) level) == sift_shred_kern)
			PG_RETURN_TEXT_P(cstring_to_text(sift_simd_level_name((SiftSimdLevel) level)));
	elog(ERROR, "sift shred: the active kernel table is none of the levels");
}

/* A digest of the kernels of the active level (sift.simd) over random cases: equal at every level */
Datum
sift_test_shred_kern_digest(PG_FUNCTION_ARGS)
{
	pg_prng_state rs;
	KCase	   *c = palloc(sizeof(KCase));
	KOut	   *o = palloc(sizeof(KOut));
	uint64		h = UINT64CONST(0xCBF29CE484222325);

	sift_shred_init();
	pg_prng_seed(&rs, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		gen_kcase(&rs, c);
		run_kcase(sift_shred_kern, c, o);
		h = kout_hash(c, o, h);
	}
	PG_RETURN_INT64((int64) h);
}

/* Cases where a SIMD level the CPU supports differs from scalar */
Datum
sift_test_shred_kern_equiv(PG_FUNCTION_ARGS)
{
	pg_prng_state rs;
	KCase	   *c = palloc(sizeof(KCase));
	KOut	   *ref = palloc(sizeof(KOut)),
			   *o = palloc(sizeof(KOut));
	const SiftShredKernels *tabs[] = {sift_shred_kernels_for(SIFT_SIMD_AVX2), sift_shred_kernels_for(SIFT_SIMD_AVX512)};
	int64		bad = 0;

	pg_prng_seed(&rs, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		gen_kcase(&rs, c);
		run_kcase(&sift_shred_kernels_scalar, c, ref);
		for (int t = 0; t < 2; t++)
		{
			if (tabs[t] == NULL)
				continue;
			run_kcase(tabs[t], c, o);
			bad += !kout_equal(c, ref, o);
		}
	}
	PG_RETURN_INT64(bad);
}

/* Elements per second of one kernel (state_counts, state_bits, minmax_i64, minmax_f64, expand_parents) at the active level over n-element columns */
Datum
sift_test_shred_kern_rate(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	uint32		n = PG_GETARG_INT32(1);
	int			iters = PG_GETARG_INT32(2);
	uint8	   *state = palloc(n + 64);
	int64	   *val = palloc(n * 8 + 64);
	uint32	   *end = palloc((n / 4 + 1) * 4),
			   *parent = palloc(n * 4 + 64);
	uint64	   *nulls = palloc(n / 8 + 16),
			   *ovf = palloc(n / 8 + 16);
	pg_prng_state rs;
	instr_time	t0,
				t1;
	uint32		counts[4],
				np = n / 4;
	int64		mn,
				mx,
				sink = 0;

	sift_shred_init();
	pg_prng_seed(&rs, 7);
	for (uint32 i = 0; i < n; i++)
	{
		state[i] = pg_prng_uint32(&rs) % 8 ? SIFT_SHRED_VALUE : pg_prng_uint32(&rs) % 4;
		val[i] = (int64) pg_prng_uint32(&rs);
	}
	for (uint32 p = 0; p < np; p++)
		end[p] = (p + 1) * 4;
	INSTR_TIME_SET_CURRENT(t0);
	for (int it = 0; it < iters; it++)
	{
		if (strcmp(name, "state_counts") == 0)
			sift_shred_kern->state_counts(state, n, counts), sink += counts[2];
		else if (strcmp(name, "state_bits") == 0)
			sift_shred_kern->state_bits(state, n, nulls, ovf), sink += nulls[0] & 1;
		else if (strcmp(name, "minmax_i64") == 0)
			sink += sift_shred_kern->minmax_i64(state, val, n, &mn, &mx);
		else if (strcmp(name, "minmax_f64") == 0)
			sink += sift_shred_kern->minmax_f64(state, (const double *) val, n, (double *) &mn, (double *) &mx);
		else if (strcmp(name, "expand_parents") == 0)
			sift_shred_kern->expand_parents(end, np, parent), sink += parent[0];
		else
			elog(ERROR, "unknown kernel \"%s\"", name);
	}
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_SUBTRACT(t1, t0);
	if (sink == -1)
		elog(NOTICE, "unreachable");
	PG_RETURN_FLOAT8((double) n * iters / Max(INSTR_TIME_GET_DOUBLE(t1), 1e-9));
}

#endif							/* SIFT_TESTING */
