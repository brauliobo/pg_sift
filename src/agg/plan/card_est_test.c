/*
 * agg/plan/card_est_test.c - test-only SQL function of the cardinality estimator (agg/plan/card_est.h), compiled only with SIFT_TESTING and registered by the
 * pg_sift_test extension. sift._test_card_est folds the statistics of the arrays the way a writer folds those of a part (the range, the NULLs, the common values,
 * the fences of 16 equal-depth buckets, the distinct values, the documents that hold a key, the zones of up to 64 sampled granules, the roots of a scope that is
 * the first column), then estimates the predicates written as text; the SQL test holds the estimate to the rows that the same predicates select in the table.
 *
 * The predicates, separated by ';': `c1 = 5`, `c1 in 1,2,3`, `c1 >= 3`, `c1 <= 9`, `c1 between 3 9`, `c2 isnull`, `c2 notnull`, `doc exists`, `doc absent`,
 * `c0 scope 1,2` and `win 10 20` (the partitions that overlap the values 10 to 20 of c0).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "agg/plan/card_est.h"

PG_FUNCTION_INFO_V1(sift_test_card_est);

#define NCOLS	4

typedef struct Table
{
	int			n;
	Datum	   *v[3];
	bool	   *null[3];
	bool	   *doc_null;
	bool	   *present;
} Table;

static int
cmp_i64(const void *a, const void *b)
{
	const int64 x = *(const int64 *) a,
				y = *(const int64 *) b;

	return (x > y) - (x < y);
}

typedef struct Count
{
	int64		v;
	int64		n;
} Count;

static int
cmp_count(const void *a, const void *b)
{
	const Count *x = a,
			   *y = b;

	return x->n != y->n ? (x->n < y->n) - (x->n > y->n) : (x->v > y->v) - (x->v < y->v);
}

static void
fold_col(const Table *t, int c, SiftColCardStats *cs, double *distinct)
{
	int64	   *vals = palloc(sizeof(int64) * Max(t->n, 1));
	Count	   *counts = palloc(sizeof(Count) * Max(t->n, 1));
	int			nn = 0,
				nd = 0;

	memset(cs, 0, sizeof(*cs));
	cs->col = (uint16) c;
	cs->total_rows = (uint64) t->n;
	for (int i = 0; i < t->n; i++)
		if (t->null[c][i])
			cs->null_rows++;
		else
			vals[nn++] = DatumGetInt64(t->v[c][i]);
	qsort(vals, nn, sizeof(int64), cmp_i64);
	for (int i = 0; i < nn; i++)
		if (nd > 0 && counts[nd - 1].v == vals[i])
			counts[nd - 1].n++;
		else
		{
			counts[nd].v = vals[i];
			counts[nd++].n = 1;
		}
	*distinct = nd;
	cs->avg_comp_bytes_row = 2.0;
	cs->avg_dec_bytes_row = 8.0;
	if (nn == 0)
		return;
	cs->flags = SIFT_CSTAT_HAS_RANGE | SIFT_CSTAT_HAS_NDV | SIFT_CSTAT_HAS_MCV;
	cs->min_i64 = vals[0];
	cs->max_i64 = vals[nn - 1];
	cs->ndv_est = nd;
	qsort(counts, nd, sizeof(Count), cmp_count);
	cs->n_mcv = (uint8) Min(nd, SIFT_CARD_MCV_MAX);
	for (int i = 0; i < cs->n_mcv; i++)
	{
		cs->mcvs[i].val_or_hash = (uint64) counts[i].v;
		cs->mcvs[i].freq = (float4) ((double) counts[i].n / (double) t->n);
	}
	if (nn >= SIFT_CARD_HIST_BUCKETS)
	{
		cs->flags |= SIFT_CSTAT_HAS_HIST;
		cs->n_hist = SIFT_CARD_HIST_BUCKETS;
		for (int b = 0; b < SIFT_CARD_HIST_BUCKETS; b++)
			cs->hist_bounds[b] = vals[(int64) b * nn / SIFT_CARD_HIST_BUCKETS];
		cs->hist_bounds[SIFT_CARD_HIST_BUCKETS] = vals[nn - 1];
	}
}

static void
fold_zone(const Table *t, int c, int from, int to, SiftCardZone *z)
{
	z->min = INT64_MAX;
	z->max = INT64_MIN;
	z->nnull = 0;
	z->flags = 0;
	if (c == 3)
	{
		bool		all = true,
					none = true;

		for (int i = from; i < to; i++)
			if (t->doc_null[i])
				z->nnull++;
			else
			{
				all &= t->present[i];
				none &= !t->present[i];
			}
		z->flags = (none ? SIFT_CZONE_KEY_NONE : 0) | (all && z->nnull == 0 ? SIFT_CZONE_KEY_ALL : 0);
		z->min = z->max = 0;
		return;
	}
	for (int i = from; i < to; i++)
		if (t->null[c][i])
			z->nnull++;
		else
		{
			z->min = Min(z->min, DatumGetInt64(t->v[c][i]));
			z->max = Max(z->max, DatumGetInt64(t->v[c][i]));
		}
}

static int64 *
parse_list(const char *s, uint32 *n)
{
	int64	   *v = palloc(sizeof(int64) * 64);

	*n = 0;
	while (*s && *n < 64)
	{
		char	   *end;

		v[(*n)++] = strtoll(s, &end, 10);
		s = *end == ',' ? end + 1 : end;
	}
	return v;
}

static void
parse_pred(char *text, SiftCardPred *p, SiftCardQuery *q)
{
	char		col[16],
				op[16],
				a[256],
				b[64];
	int			n = sscanf(text, " %15s %15s %255s %63s", col, op, a, b);

	memset(p, 0, sizeof(*p));
	if (!strcmp(col, "win"))
	{
		q->has_window = true;
		q->win_lo = strtoll(op, NULL, 10);
		q->win_hi = strtoll(a, NULL, 10);
		p->kind = (SiftCardPredKind) -1;
		return;
	}
	p->col = !strcmp(col, "doc") ? 3 : (uint16) (col[1] - '0');
	if (!strcmp(op, "isnull"))
		p->kind = SCP_IS_NULL;
	else if (!strcmp(op, "notnull"))
		p->kind = SCP_IS_NOT_NULL;
	else if (!strcmp(op, "exists"))
		p->kind = SCP_KEY_EXISTS;
	else if (!strcmp(op, "absent"))
		p->kind = SCP_KEY_ABSENT;
	else if (n >= 3 && (!strcmp(op, "=") || !strcmp(op, "in") || !strcmp(op, "scope")))
	{
		uint32		nv;
		int64	   *v = parse_list(a, &nv);

		p->kind = !strcmp(op, "=") ? SCP_EQ : !strcmp(op, "in") ? SCP_IN : SCP_SCOPE_IN;
		p->vals = (const uint64 *) v;
		p->nvals = nv;
	}
	else if (n >= 3)
	{
		p->kind = SCP_RANGE;
		if (!strcmp(op, ">="))
		{
			p->has_lo = true;
			p->lo = strtoll(a, NULL, 10);
		}
		else if (!strcmp(op, "<="))
		{
			p->has_hi = true;
			p->hi = strtoll(a, NULL, 10);
		}
		else if (!strcmp(op, "between") && n == 4)
		{
			p->has_lo = p->has_hi = true;
			p->lo = strtoll(a, NULL, 10);
			p->hi = strtoll(b, NULL, 10);
		}
		else
			ereport(ERROR, (errmsg("unknown predicate \"%s\"", text)));
	}
	else
		ereport(ERROR, (errmsg("unknown predicate \"%s\"", text)));
}

static ArrayType *
arg_array(FunctionCallInfo fcinfo, int i)
{
	return PG_GETARG_ARRAYTYPE_P(i);
}

static void
load(Table *t, int c, ArrayType *a, Oid oid, int len, bool byval, char align)
{
	int			n;

	deconstruct_array(a, oid, len, byval, align, &t->v[c], &t->null[c], &n);
	if (n != t->n)
		ereport(ERROR, (errmsg("the column arrays differ in length")));
}

/*
 * sift._test_card_est(c0 int8[], c1 int8[], c2 int8[], doc bool[], present bool[], preds text, granule_rows int4 DEFAULT 1024,
 * granules_per_partition int4 DEFAULT 16, sample int4 DEFAULT 64)
 * RETURNS TABLE(est_rows float8, selectivity float8, g_eff float8, g_total float8, confidence float8, exact_zero bool, used_scope bool, ndv_c1 float8)
 */
Datum
sift_test_card_est(PG_FUNCTION_ARGS)
{
	Table		t;
	SiftColCardStats cs[NCOLS];
	double		distinct[3] = {0, 0, 0};
	const int	gr = PG_GETARG_INT32(6),
				gpp = PG_GETARG_INT32(7),
				want = PG_GETARG_INT32(8);
	int			G,
				ns;
	SiftCardPartition *parts;
	SiftCardZone *zones;
	SiftSampleGranule *sample;
	SiftCardScopeRoot *scope;
	int			nscope = 0;
	int64	   *keys;
	SiftCardStatsView sv;
	SiftCardQuery q;
	SiftCardPred preds[SIFT_CARD_MAX_PREDS];
	SiftCardEst est;
	TupleDesc	tupdesc;
	Datum		v[8];
	bool		nulls[8] = {false};
	char	   *pred_text = text_to_cstring(PG_GETARG_TEXT_PP(5)),
			   *tok,
			   *save = NULL;
	uint32		np = 0;
	int64		sum_rows = 0;
	Datum	   *dd,
			   *pd;
	bool	   *dn,
			   *pn;
	int			n2;

	memset(&t, 0, sizeof(t));
	t.n = ArrayGetNItems(ARR_NDIM(arg_array(fcinfo, 0)), ARR_DIMS(arg_array(fcinfo, 0)));
	for (int c = 0; c < 3; c++)
		load(&t, c, arg_array(fcinfo, c), INT8OID, 8, true, 'd');
	deconstruct_array(arg_array(fcinfo, 3), BOOLOID, 1, true, 'c', &dd, &dn, &n2);
	deconstruct_array(arg_array(fcinfo, 4), BOOLOID, 1, true, 'c', &pd, &pn, &n2);
	t.doc_null = dn;
	t.present = palloc(sizeof(bool) * t.n);
	for (int i = 0; i < t.n; i++)
		t.present[i] = !pn[i] && DatumGetBool(pd[i]);
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	for (int c = 0; c < 3; c++)
		fold_col(&t, c, &cs[c], &distinct[c]);
	memset(&cs[3], 0, sizeof(cs[3]));
	cs[3].col = 3;
	cs[3].total_rows = (uint64) t.n;
	cs[3].flags = SIFT_CSTAT_HAS_KEY_PRESENT;
	for (int i = 0; i < t.n; i++)
	{
		cs[3].null_rows += t.doc_null[i];
		cs[3].key_present_rows += t.present[i] && !t.doc_null[i];
	}

	G = (t.n + gr - 1) / gr;
	parts = palloc0(sizeof(SiftCardPartition) * ((G + gpp - 1) / gpp + 1));
	for (int p = 0; p * gpp < G; p++)
	{
		const int	from = p * gpp * gr,
					to = Min(t.n, (p + 1) * gpp * gr);

		parts[p].rows = (uint64) (to - from);
		parts[p].lo = INT64_MAX;
		parts[p].hi = INT64_MIN;
		for (int i = from; i < to; i++)
			if (!t.null[0][i])
			{
				parts[p].lo = Min(parts[p].lo, DatumGetInt64(t.v[0][i]));
				parts[p].hi = Max(parts[p].hi, DatumGetInt64(t.v[0][i]));
			}
	}
	ns = want <= 0 ? 0 : Min(G, Min(want, SIFT_CARD_SAMPLE_GRANULES));
	sample = palloc0(sizeof(SiftSampleGranule) * Max(ns, 1));
	zones = palloc0(sizeof(SiftCardZone) * NCOLS * Max(ns, 1));
	for (int k = 0; k < ns; k++)
	{
		const int	g = (int) ((int64) (2 * k + 1) * G / (2 * ns)),
					from = g * gr,
					to = Min(t.n, from + gr);

		sample[k].part_idx = (uint32) (g / gpp);
		sample[k].nrows = (uint16) (to - from);
		sum_rows += to - from;
		for (int c = 0; c < NCOLS; c++)
			fold_zone(&t, c, from, to, &zones[(size_t) c * ns + k]);
	}
	for (int k = 0; k < ns; k++)
		sample[k].weight = (float4) ((double) t.n / (double) sum_rows);

	keys = palloc(sizeof(int64) * Max(t.n, 1));
	for (int i = 0; i < t.n; i++)
		keys[i] = t.null[0][i] ? INT64_MIN : DatumGetInt64(t.v[0][i]);
	qsort(keys, t.n, sizeof(int64), cmp_i64);
	scope = palloc(sizeof(SiftCardScopeRoot) * Max(t.n, 1));
	for (int i = 0; i < t.n; i++)
		if (keys[i] != INT64_MIN)
		{
			if (nscope > 0 && scope[nscope - 1].key == keys[i])
				scope[nscope - 1].nrows++;
			else
			{
				scope[nscope].key = keys[i];
				scope[nscope++].nrows = 1;
			}
		}

	memset(&sv, 0, sizeof(sv));
	sv.total_live_rows = (uint64) t.n;
	sv.total_granules = (uint64) G;
	sv.ncols = NCOLS;
	sv.nparts = (uint32) ((G + gpp - 1) / gpp);
	sv.n_sample = (uint32) ns;
	sv.cols = cs;
	sv.parts = parts;
	sv.sample = sample;
	sv.sample_zones = zones;
	sv.scope = scope;
	sv.n_scope = (uint32) nscope;

	memset(&q, 0, sizeof(q));
	q.lead_col = 0;
	for (tok = strtok_r(pred_text, ";", &save); tok && np < SIFT_CARD_MAX_PREDS; tok = strtok_r(NULL, ";", &save))
	{
		parse_pred(tok, &preds[np], &q);
		if (preds[np].kind != (SiftCardPredKind) -1)
			np++;
	}
	q.preds = preds;
	q.npreds = np;
	sift_card_estimate(&sv, &q, &est);

	v[0] = Float8GetDatum(est.est_rows);
	v[1] = Float8GetDatum(est.selectivity);
	v[2] = Float8GetDatum(est.g_eff);
	v[3] = Float8GetDatum(est.g_total);
	v[4] = Float8GetDatum(est.confidence);
	v[5] = BoolGetDatum(est.exact_zero);
	v[6] = BoolGetDatum(est.used_scope_l2);
	v[7] = Float8GetDatum(sift_card_ndv(distinct[1], (double) t.n, est.est_rows));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, v, nulls)));
}

#endif							/* SIFT_TESTING */
