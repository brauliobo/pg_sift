/*
 * agg/window_test.c - test-only SQL functions of the streaming window executor (agg/window.h), compiled only with SIFT_TESTING and registered by the
 * pg_sift_test extension. sift._test_window runs the executor over arrays that the SQL has sorted (the partition key, the order key, an int8 and a float8
 * argument, a mask of the rows a WHERE keeps), with the functions named in a list, and returns each row with its results as text, so that the SQL test
 * can hold them to PostgreSQL's own window functions over the same table. The source can deliver granules of a fixed number of rows and pass over the
 * granules of a partition that is not wanted, as a source with zone maps does; sift._test_window_stats gives the counters of the last run.
 *
 * A function is written name[:column][:offset[:default]][:rows]: row_number, rank, dense_rank, lag:arg:2:-1, lead:argf, first_value:arg, count:*, count:arg,
 * sum:arg, avg:argf:rows, min:arg, max:argf. The column is arg (int8) or argf (float8); an aggregate has the RANGE frame unless it says rows.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"

#include "agg/window.h"

PG_FUNCTION_INFO_V1(sift_test_window);
PG_FUNCTION_INFO_V1(sift_test_window_stats);
PG_FUNCTION_INFO_V1(sift_test_window_tail_tie);

typedef struct WStats
{
	SiftWinStats st;
	int64		granules_read;
	int64		granules_total;
} WStats;

static WStats last_stats;

typedef struct WSrc
{
	int			n;
	int			pos;			/* the next row to deliver */
	int			batch;
	int			granule;		/* 0: batches of `batch` rows; else the rows of a granule, the unit that is skipped */
	const int64 *part,
			   *ord,
			   *arg;
	const double *argf;
	const bool *pn,
			   *on,
			   *an,
			   *afn,
			   *act;
	bool		keys_i32;
	bool		use[SIFT_WIN_MAX_FUNCS][2];	/* the function reads arg, argf */
	int64	   *pk_min,
			   *pk_max;			/* the zone of the part key of each granule */
	bool	   *pk_has_null;
	int			granules_read;
	/* the batch */
	SiftVec		pv,
				ov,
				av,
				afv;
	int32	   *p32,
			   *o32;
	int64	   *p64,
			   *o64,
			   *a64;
	double	   *f64;
	uint64		pvalid[SIFT_VEC_WORDS],
				ovalid[SIFT_VEC_WORDS],
				avalid[SIFT_VEC_WORDS],
				afvalid[SIFT_VEC_WORDS],
				tag[SIFT_VEC_LEN],
				mask[SIFT_VEC_WORDS];
} WSrc;

static void
col_init(SiftVec *v, void *data, uint64 *valid, SiftVecType type)
{
	memset(v, 0, sizeof(*v));
	v->data = data;
	v->valid = valid;
	v->dense = true;
	v->type = type;
}

static void *
buf(size_t bytes)
{
	return palloc_aligned(bytes + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
}

static bool
wsrc_next(void *p, SiftWinBatch *out)
{
	WSrc	   *s = p;
	const int	len = s->granule ? s->granule : s->batch;
	int			n;

	if (s->pos >= s->n)
		return false;
	n = Min(len, s->n - s->pos);
	memset(s->pvalid, 0, sizeof(s->pvalid));
	memset(s->ovalid, 0, sizeof(s->ovalid));
	memset(s->avalid, 0, sizeof(s->avalid));
	memset(s->afvalid, 0, sizeof(s->afvalid));
	memset(s->mask, 0, sizeof(s->mask));
	for (int i = 0; i < n; i++)
	{
		const int	r = s->pos + i;

		s->p64[i] = s->p32[i] = (int32) s->part[r];
		s->o64[i] = s->o32[i] = (int32) s->ord[r];
		if (!s->keys_i32)
		{
			s->p64[i] = s->part[r];
			s->o64[i] = s->ord[r];
		}
		s->a64[i] = s->arg[r];
		s->f64[i] = s->argf[r];
		s->pvalid[i >> 6] |= (uint64) !s->pn[r] << (i & 63);
		s->ovalid[i >> 6] |= (uint64) !s->on[r] << (i & 63);
		s->avalid[i >> 6] |= (uint64) !s->an[r] << (i & 63);
		s->afvalid[i >> 6] |= (uint64) !s->afn[r] << (i & 63);
		s->mask[i >> 6] |= (uint64) (!s->act || s->act[r]) << (i & 63);
		s->tag[i] = (uint64) r;
	}
	memset(out, 0, sizeof(*out));
	out->n = (uint16) n;
	out->part[0] = &s->pv;
	out->ord[0] = &s->ov;
	for (int f = 0; f < SIFT_WIN_MAX_FUNCS; f++)
		out->arg[f] = s->use[f][0] ? &s->av : s->use[f][1] ? &s->afv : NULL;
	out->tag = s->tag;
	out->mask = s->mask;
	s->pv.len = s->ov.len = s->av.len = s->afv.len = (uint16) n;
	s->pos += n;
	s->granules_read += s->granule != 0;
	return true;
}

/* a partition that is not wanted: the granules that are all of that partition are not read */
static void
wsrc_skip(void *p, const int64 *part_key, uint8 part_nulls)
{
	WSrc	   *s = p;

	if (!s->granule || part_nulls)
		return;
	while (s->pos < s->n)
	{
		const int	g = s->pos / s->granule;

		if (s->pk_has_null[g] || s->pk_min[g] != part_key[0] || s->pk_max[g] != part_key[0])
			break;
		s->pos = Min(s->n, (g + 1) * s->granule);
	}
}

static SiftWinFuncKind
func_kind(const char *name)
{
	static const struct
	{
		const char *name;
		SiftWinFuncKind kind;
	}			kinds[] = {
		{"row_number", SWIN_ROW_NUMBER}, {"rank", SWIN_RANK}, {"dense_rank", SWIN_DENSE_RANK}, {"lag", SWIN_LAG}, {"lead", SWIN_LEAD},
		{"first_value", SWIN_FIRST_VALUE}, {"count", SWIN_COUNT}, {"sum", SWIN_SUM}, {"avg", SWIN_AVG}, {"min", SWIN_MIN}, {"max", SWIN_MAX}
	};

	for (int i = 0; i < lengthof(kinds); i++)
		if (!strcmp(name, kinds[i].name))
			return kinds[i].kind;
	ereport(ERROR, (errmsg("unknown function \"%s\"", name)));
	return SWIN_RANK;
}

static void
parse_func(const char *text, SiftWinFuncSpec *f, bool *use_arg, bool *use_argf)
{
	char	   *copy = pstrdup(text),
			   *tok[6];
	int			nt = 0;
	bool		rows = false;
	int			nnum = 0;

	for (char *c = strtok(copy, ":"); c && nt < 6; c = strtok(NULL, ":"))
		tok[nt++] = c;
	memset(f, 0, sizeof(*f));
	*use_arg = *use_argf = false;
	f->arg_type = SVEC_I64;
	f->offset = 1;
	f->range_frame = true;
	f->kind = func_kind(tok[0]);
	for (int i = 1; i < nt; i++)
	{
		if (!strcmp(tok[i], "*"))
			f->kind = SWIN_COUNT_STAR;
		else if (!strcmp(tok[i], "arg"))
			*use_arg = true;
		else if (!strcmp(tok[i], "argf"))
		{
			*use_argf = true;
			f->arg_type = SVEC_F64;
		}
		else if (!strcmp(tok[i], "rows"))
			rows = true;
		else if (nnum++ == 0)
			f->offset = (uint32) atoi(tok[i]);
		else
		{
			f->has_default = true;
			if (f->arg_type == SVEC_F64)
				f->default_val.f = strtod(tok[i], NULL);
			else
				f->default_val.i = strtoll(tok[i], NULL, 10);
		}
	}
	f->range_frame = !rows;
}

static char *
i128_text(SiftI128 v)
{
	char		digits[48];
	int			n = 0;
	const bool	neg = v.hi < 0;
	unsigned __int128 x = ((unsigned __int128) (uint64) v.hi << 64) | v.lo;

	if (neg)
		x = -x;
	do
	{
		digits[n++] = (char) ('0' + (int) (x % 10));
		x /= 10;
	} while (x);
	{
		char	   *out = palloc(n + 2),
				   *o = out;

		if (neg)
			*o++ = '-';
		while (n)
			*o++ = digits[--n];
		*o = 0;
		return out;
	}
}

/* coef * 10^-scale as the text PostgreSQL gives the numeric */
static char *
num_text(const SiftWinNum *n)
{
	char	   *digits = i128_text(sift_win_num_coef(n));
	const bool	neg = digits[0] == '-';
	const int	len = (int) strlen(digits) - neg,
				total = Max(len, n->scale + 1);
	StringInfoData out;

	initStringInfo(&out);
	if (neg)
		appendStringInfoChar(&out, '-');
	for (int at = 0; at < total; at++)
	{
		if (n->scale > 0 && at == total - n->scale)
			appendStringInfoChar(&out, '.');
		appendStringInfoChar(&out, at < total - len ? '0' : digits[neg + at - (total - len)]);
	}
	return out.data;
}

static char *
val_text(const SiftWinFuncSpec *f, SiftWinVal v)
{
	switch (sift_win_result_type(f))
	{
		case SVEC_F64:
			return float8out_internal(v.f);
		case SVEC_NUM128:
			return f->kind == SWIN_AVG ? num_text(&v.num) : i128_text(v.w);
		default:
			return psprintf("%lld", (long long) v.i);
	}
}

typedef struct Sink
{
	ReturnSetInfo *rsi;
	const SiftWinFuncSpec *funcs;
	int			nf;
} Sink;

static void
sink_row(void *arg, const SiftWinRow *r)
{
	Sink	   *k = arg;
	Datum	   *el = palloc(k->nf * sizeof(Datum));
	bool	   *en = palloc(k->nf);
	Datum		row[3];
	bool		nulls[3] = {false, false, false};
	int			dims[1] = {k->nf},
				lbs[1] = {1};

	for (int f = 0; f < k->nf; f++)
	{
		en[f] = (r->res_nulls >> f) & 1;
		el[f] = en[f] ? (Datum) 0 : CStringGetTextDatum(val_text(&k->funcs[f], r->res[f]));
	}
	row[0] = Int64GetDatum((int64) r->tag);
	row[1] = Int64GetDatum((int64) r->part_idx);
	row[2] = PointerGetDatum(construct_md_array(el, en, 1, dims, lbs, TEXTOID, -1, false, TYPALIGN_INT));
	tuplestore_putvalues(k->rsi->setResult, k->rsi->setDesc, row, nulls);
	pfree(el);
	pfree(en);
}

static void
unpack(ArrayType *a, Oid type, int len, bool byval, char align, Datum **d, bool **n, int *cnt)
{
	deconstruct_array(a, type, len, byval, align, d, n, cnt);
}

/*
 * sift._test_window(part int8[], ord int8[], arg int8[], argf float8[], funcs text[], batch_rows int4, mode text, topk int8, off int8, lim int8, act bool[],
 *					 keys_i32 bool, granule int4) RETURNS TABLE(tag int8, part_idx int8, res text[])
 */
Datum
sift_test_window(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum	   *pd,
			   *od,
			   *ad,
			   *fd,
			   *ft,
			   *xd;
	bool	   *pn,
			   *on,
			   *an,
			   *fn,
			   *fx,
			   *xn;
	int			np,
				no,
				na,
				nfl,
				nft,
				nx;
	WSrc	   *s = palloc0(sizeof(WSrc));
	SiftWindowSpec spec;
	Sink		sink;
	SiftWindowExec *w;
	const char *mode = PG_ARGISNULL(6) ? "window" : text_to_cstring(PG_GETARG_TEXT_PP(6));
	int64	   *part,
			   *ord,
			   *arg;
	double	   *argf;
	bool	   *act = NULL;

	sift_aggkern6_init();
	InitMaterializedSRF(fcinfo, 0);
	unpack(PG_GETARG_ARRAYTYPE_P(0), INT8OID, 8, true, 'd', &pd, &pn, &np);
	unpack(PG_GETARG_ARRAYTYPE_P(1), INT8OID, 8, true, 'd', &od, &on, &no);
	unpack(PG_GETARG_ARRAYTYPE_P(2), INT8OID, 8, true, 'd', &ad, &an, &na);
	unpack(PG_GETARG_ARRAYTYPE_P(3), FLOAT8OID, 8, true, 'd', &fd, &fn, &nfl);
	unpack(PG_GETARG_ARRAYTYPE_P(4), TEXTOID, -1, false, 'i', &ft, &fx, &nft);
	part = palloc(Max(np, 1) * 8);
	ord = palloc(Max(np, 1) * 8);
	arg = palloc(Max(np, 1) * 8);
	argf = palloc(Max(np, 1) * 8);
	for (int i = 0; i < np; i++)
	{
		part[i] = pn[i] ? 0 : DatumGetInt64(pd[i]);
		ord[i] = on[i] ? 0 : DatumGetInt64(od[i]);
		arg[i] = an[i] ? 0 : DatumGetInt64(ad[i]);
		argf[i] = fn[i] ? 0 : DatumGetFloat8(fd[i]);
	}
	if (!PG_ARGISNULL(10))
	{
		unpack(PG_GETARG_ARRAYTYPE_P(10), BOOLOID, 1, true, 'c', &xd, &xn, &nx);
		act = palloc(Max(nx, 1));
		for (int i = 0; i < nx; i++)
			act[i] = !xn[i] && DatumGetBool(xd[i]);
	}
	s->n = np;
	s->batch = PG_ARGISNULL(5) ? SIFT_VEC_LEN : Min(PG_GETARG_INT32(5), SIFT_VEC_LEN);
	s->granule = PG_ARGISNULL(12) ? 0 : Min(PG_GETARG_INT32(12), SIFT_VEC_LEN);
	s->keys_i32 = !PG_ARGISNULL(11) && PG_GETARG_BOOL(11);
	s->part = part;
	s->ord = ord;
	s->arg = arg;
	s->argf = argf;
	s->pn = pn;
	s->on = on;
	s->an = an;
	s->afn = fn;
	s->act = act;
	s->p32 = buf(SIFT_VEC_LEN * 4);
	s->o32 = buf(SIFT_VEC_LEN * 4);
	s->p64 = buf(SIFT_VEC_LEN * 8);
	s->o64 = buf(SIFT_VEC_LEN * 8);
	s->a64 = buf(SIFT_VEC_LEN * 8);
	s->f64 = buf(SIFT_VEC_LEN * 8);
	col_init(&s->pv, s->keys_i32 ? (void *) s->p32 : (void *) s->p64, s->pvalid, s->keys_i32 ? SVEC_I32 : SVEC_I64);
	col_init(&s->ov, s->keys_i32 ? (void *) s->o32 : (void *) s->o64, s->ovalid, s->keys_i32 ? SVEC_I32 : SVEC_I64);
	col_init(&s->av, s->a64, s->avalid, SVEC_I64);
	col_init(&s->afv, s->f64, s->afvalid, SVEC_F64);
	if (s->granule)
	{
		const int	ng = (np + s->granule - 1) / s->granule;

		s->pk_min = palloc(Max(ng, 1) * 8);
		s->pk_max = palloc(Max(ng, 1) * 8);
		s->pk_has_null = palloc0(Max(ng, 1));
		for (int g = 0; g < ng; g++)
		{
			s->pk_min[g] = INT64_MAX;
			s->pk_max[g] = INT64_MIN;
			for (int r = g * s->granule; r < Min(np, (g + 1) * s->granule); r++)
			{
				s->pk_has_null[g] |= pn[r];
				s->pk_min[g] = Min(s->pk_min[g], part[r]);
				s->pk_max[g] = Max(s->pk_max[g], part[r]);
			}
		}
		last_stats.granules_total = ng;
	}
	else
		last_stats.granules_total = 0;

	memset(&spec, 0, sizeof(spec));
	spec.mode = !strcmp(mode, "first") ? SWIN_MODE_DISTINCT_FIRST : !strcmp(mode, "last") ? SWIN_MODE_DISTINCT_LAST : SWIN_MODE_WINDOW;
	spec.n_part = 1;
	spec.n_ord = 1;
	spec.n_funcs = (uint16) nft;
	for (int f = 0; f < nft; f++)
		parse_func(TextDatumGetCString(ft[f]), &spec.funcs[f], &s->use[f][0], &s->use[f][1]);
	if (!PG_ARGISNULL(7) && PG_GETARG_INT64(7) > 0)
	{
		spec.topk.active = true;
		spec.topk.func = 0;
		spec.topk.k = PG_GETARG_INT64(7);
	}
	spec.offset = PG_ARGISNULL(8) ? 0 : (uint64) PG_GETARG_INT64(8);
	spec.limit = PG_ARGISNULL(9) ? SIFT_WIN_NO_LIMIT : (uint64) PG_GETARG_INT64(9);
	sink.rsi = rsi;
	sink.funcs = spec.funcs;
	sink.nf = nft;
	w = sift_window_create(CurrentMemoryContext, &spec, (uint64) work_mem * 1024, wsrc_next, wsrc_skip, s, sink_row, &sink);
	sift_window_run(w);
	last_stats.st = *sift_window_stats(w);
	last_stats.granules_read = s->granules_read;
	sift_window_free(w);
	return (Datum) 0;
}

/* sift._test_window_stats() RETURNS TABLE(rows_in, rows_looked, rows_out, skips, max_pending, granules_read, granules_total) of the last run */
Datum
sift_test_window_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	td;
	Datum		v[7];
	bool		nulls[7] = {false};

	if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	v[0] = Int64GetDatum((int64) last_stats.st.rows_in);
	v[1] = Int64GetDatum((int64) last_stats.st.rows_looked);
	v[2] = Int64GetDatum((int64) last_stats.st.rows_out);
	v[3] = Int64GetDatum((int64) last_stats.st.skips);
	v[4] = Int64GetDatum((int64) last_stats.st.max_pending);
	v[5] = Int64GetDatum(last_stats.granules_read);
	v[6] = Int64GetDatum(last_stats.granules_total);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(td), v, nulls)));
}

/* sift._test_window_tail_tie(vals int8[], granule int4) RETURNS int4: the first granule of the rows (ascending, NULLs last) that can hold the tail tie run */
Datum
sift_test_window_tail_tie(PG_FUNCTION_ARGS)
{
	Datum	   *d;
	bool	   *nl;
	int			n,
				gr = PG_GETARG_INT32(1),
				ng;
	SiftWinZone *z;

	deconstruct_array(PG_GETARG_ARRAYTYPE_P(0), INT8OID, 8, true, 'd', &d, &nl, &n);
	if (n == 0 || gr <= 0)
		ereport(ERROR, (errmsg("at least one row and a granule of at least one row")));
	ng = (n + gr - 1) / gr;
	z = palloc0(sizeof(SiftWinZone) * ng);
	for (int g = 0; g < ng; g++)
	{
		const int	to = Min(n, (g + 1) * gr);

		z[g].min = INT64_MAX;
		z[g].max = INT64_MIN;
		z[g].all_null = true;
		for (int r = g * gr; r < to; r++)
		{
			z[g].has_null |= nl[r];
			z[g].all_null &= nl[r];
			if (!nl[r])
			{
				z[g].min = Min(z[g].min, DatumGetInt64(d[r]));
				z[g].max = Max(z[g].max, DatumGetInt64(d[r]));
			}
		}
	}
	PG_RETURN_INT32((int32) sift_win_tail_tie_first(z, (uint32) ng));
}

#endif							/* SIFT_TESTING */
