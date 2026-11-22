/*
 * agg/par/star_shm_test.c - test-only SQL function of the dimensions of a star join (agg/par/star_shm.h), compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension. sift._test_star takes a fact table (the item and the region of each row) and the dimensions as arrays: categories, which the items point to (a snowflake),
 * items and regions, which the fact table joins; the leader and 0 to 8 workers build the bundle (the steps of each dimension between barriers), and the leader then scans the
 * fact table as a join would: the zone of each granule against the filters, the batch against them, the rows against the tables. The rows it gives (the row and the payload of
 * each dimension) are compared by the SQL test with PostgreSQL's own join of the same kinds. sift._test_star_stats tells what the last scan did.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/s_lock.h"
#include "storage/shm_toc.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "agg/par/star_shm.h"

PG_FUNCTION_INFO_V1(sift_test_star);
PG_FUNCTION_INFO_V1(sift_test_star_stats);
PGDLLEXPORT void sift_star_test_worker(dsm_segment *seg, shm_toc *toc);

#define KEY_SHARED	UINT64CONST(0x5200000000000005)
#define KEY_BUNDLE	UINT64CONST(0x5200000000000006)
#define N_ARRAYS	7

enum
{
	A_I_ID,
	A_I_CAT,
	A_I_PAY,
	A_C_ID,
	A_C_PAY,
	A_R_ID,
	A_R_PAY
};

typedef struct Shared
{
	int			n_dims;
	uint32		len[N_ARRAYS];
	SiftStarDimSpec specs[3];
	pg_atomic_uint32 arrived pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 go pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	int64		data[FLEXIBLE_ARRAY_MEMBER];
} Shared;

typedef struct DimData
{
	const int64 *key;
	const int64 *pkey;
	const int64 *pay;
} DimData;

static struct
{
	int32		items_raw,
				items_surviving,
				cats_surviving;
	bool		any_empty;
	int64		fact_rows_read,
				rows_probed;
	int32		granules_total,
				granules_pruned,
				auto_disables,
				elided,
				launched;
	int64		bundle_bytes;
}			stats;

static const int64 *
array_at(const Shared *s, int a)
{
	Size		at = 0;

	for (int i = 0; i < a; i++)
		at += s->len[i];
	return s->data + at;
}

static void
dim_rows(void *arg, uint32 first, uint32 n, int64 *key, int64 *pkey, int64 **pay)
{
	const DimData *d = arg;

	memcpy(key, d->key + first, sizeof(int64) * n);
	if (pkey)
		memcpy(pkey, d->pkey + first, sizeof(int64) * n);
	if (d->pay)
		memcpy(pay[0], d->pay + first, sizeof(int64) * n);
}

/* The sources of the dimensions in the order of the specs: categories, items, regions */
static void
make_sources(const Shared *s, DimData *dd, SiftStarSource *src)
{
	dd[0] = (DimData) {array_at(s, A_C_ID), NULL, s->specs[0].n_pay_cols ? array_at(s, A_C_PAY) : NULL};
	dd[1] = (DimData) {array_at(s, A_I_ID), array_at(s, A_I_CAT), s->specs[1].n_pay_cols ? array_at(s, A_I_PAY) : NULL};
	if (s->n_dims > 2)
		dd[2] = (DimData) {array_at(s, A_R_ID), NULL, s->specs[2].n_pay_cols ? array_at(s, A_R_PAY) : NULL};
	for (int d = 0; d < s->n_dims; d++)
		src[d] = (SiftStarSource) {dim_rows, &dd[d]};
}

static SiftStarDimBundle *
bundle_of(shm_toc *toc)
{
	return shm_toc_lookup(toc, KEY_BUNDLE, false);
}

void
sift_star_test_worker(dsm_segment *seg, shm_toc *toc)
{
	Shared	   *s = shm_toc_lookup(toc, KEY_SHARED, false);
	DimData		dd[3];
	SiftStarSource src[3];

	make_sources(s, dd, src);
	pg_atomic_fetch_add_u32(&s->arrived, 1);
	while (pg_atomic_read_u32(&s->go) == 0)
	{
		CHECK_FOR_INTERRUPTS();
		SPIN_DELAY();
	}
	sift_star_build((SiftStarDimBundle *) TYPEALIGN(PG_CACHE_LINE_SIZE, bundle_of(toc)), src);
}

static int64 *
take_array(ArrayType *a, uint32 *len)
{
	Datum	   *d;
	bool	   *n;
	int			cnt;
	int64	   *out;

	deconstruct_array(a, INT8OID, 8, true, 'd', &d, &n, &cnt);
	out = palloc(sizeof(int64) * Max(cnt, 1));
	for (int i = 0; i < cnt; i++)
	{
		if (n[i])
			ereport(ERROR, (errmsg("the arrays of the star test have no NULL")));
		out[i] = DatumGetInt64(d[i]);
	}
	*len = cnt;
	return out;
}

typedef struct StarScan
{
	SiftStarDimBundle *b;
	ReturnSetInfo *rsi;
	SiftSipProbeState ps[3];
	int			item_dim;
	int			region_dim;
} StarScan;

static void
put_row(StarScan *sc, int32 ord, const int64 *vals, const bool *valid)
{
	Datum		row[4];
	bool		nulls[4];

	row[0] = Int32GetDatum(ord);
	nulls[0] = false;
	for (int c = 0; c < 3; c++)
	{
		row[1 + c] = Int64GetDatum(vals[c]);
		nulls[1 + c] = !valid[c];
	}
	tuplestore_putvalues(sc->rsi->setResult, sc->rsi->setDesc, row, nulls);
}

/* The rows of a dimension that a key finds, as the compact rows of its table, or the filter alone for the one that is nothing else (a match then); *n rows */
static int
matches(SiftStarDimBundle *b, int d, int64 key, uint32 *rows, int cap)
{
	SiftDimShm *tab = sift_star_table(b, d);
	SiftDimIter it;
	int			n = 0;
	uint32		row;

	if (tab == NULL)
	{
		rows[0] = 0;
		return 1;
	}
	sift_dim_shm_find(tab, key, &it);
	while (n < cap && sift_dim_shm_next(&it, &row))
		rows[n++] = row;
	return n;
}

/* A dimension joined by kind: the rows it gives to a fact row: -1 drops the fact row; 0 keeps it with no row (NULL payload); else the rows */
static int
join_rows(SiftStarDimBundle *b, int d, int64 key, uint32 *rows, int cap)
{
	const SiftDimJoinKind kind = b->dims[d].join_kind;
	const int	n = matches(b, d, key, rows, cap);

	switch (kind)
	{
		case SDIM_INNER:
			return n > 0 ? n : -1;
		case SDIM_SEMI:
			return n > 0 ? 0 : -1;
		case SDIM_LEFT:
			return n;
		default:
			return n > 0 ? -1 : 0;
	}
}

static void
scan_granule(StarScan *sc, const int64 *fi, const int64 *fr, int32 from, int n, int64 *kv_i, int64 *kv_r)
{
	SiftStarDimBundle *b = sc->b;
	uint64		mask[SIFT_VEC_WORDS] = {0};
	SiftVec		vi = {.type = SVEC_I64,.dense = true,.all_valid = true,.data = kv_i,.len = n},
				vr = {.type = SVEC_I64,.dense = true,.all_valid = true,.data = kv_r,.len = n};

	memcpy(kv_i, fi + from, sizeof(int64) * n);
	if (sc->region_dim >= 0)
		memcpy(kv_r, fr + from, sizeof(int64) * n);
	for (int i = 0; i < n; i++)
		mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	if (b->dims[sc->item_dim].push_sip)
		sift_sip_probe_vec_adapt(sift_star_sip(b, sc->item_dim), &sc->ps[sc->item_dim], &vi, mask);
	if (sc->region_dim >= 0 && b->dims[sc->region_dim].push_sip)
		sift_sip_probe_vec_adapt(sift_star_sip(b, sc->region_dim), &sc->ps[sc->region_dim], &vr, mask);
	for (int i = 0; i < n; i++)
	{
		uint32		irows[64],
					rrows[64];
		int			ni,
					nr = 0;

		if (!((mask[i >> 6] >> (i & 63)) & 1))
			continue;
		ni = join_rows(b, sc->item_dim, fi[from + i], irows, 64);
		if (ni < 0)
			continue;
		if (sc->region_dim >= 0)
		{
			nr = join_rows(b, sc->region_dim, fr[from + i], rrows, 64);
			if (nr < 0)
				continue;
		}
		for (int x = 0; x < Max(ni, 1); x++)
			for (int y = 0; y < Max(nr, 1); y++)
			{
				int64		vals[3] = {0, 0, 0};
				bool		valid[3] = {false, false, false};
				const SiftDimPayloadShm *ip = sift_star_payload(b, sc->item_dim);

				if (ni > 0)
				{
					const int	own = b->dims[sc->item_dim].n_own_cols;

					if (own)
						valid[0] = sift_star_pay(b, sc->item_dim, 0, irows[x], &vals[0]);
					if (ip->n_cols > own)
						valid[1] = sift_star_pay(b, sc->item_dim, own, irows[x], &vals[1]);
				}
				if (nr > 0 && sift_star_payload(b, sc->region_dim)->n_cols > 0)
					valid[2] = sift_star_pay(b, sc->region_dim, 0, rrows[y], &vals[2]);
				put_row(sc, from + i + 1, vals, valid);
			}
	}
}

/*
 * sift._test_star(f_item int8[], f_region int8[], i_id int8[], i_cat int8[], i_pay int8[], c_id int8[], c_pay int8[], r_id int8[], r_pay int8[], kind_cat int4, kind_item int4,
 * kind_region int4, cats_unique bool, items_unique bool, with_regions bool, workers int4, granule int4)
 * RETURNS TABLE(ord int4, item_pay int8, cat_pay int8, region_pay int8)
 */
Datum
sift_test_star(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	const int	workers = PG_GETARG_INT32(15),
				granule = Min(Max(PG_GETARG_INT32(16), 64), SIFT_VEC_LEN);
	const bool	with_regions = PG_GETARG_BOOL(14);
	const SiftDimJoinKind kc = PG_GETARG_INT32(9),
				ki = PG_GETARG_INT32(10),
				kr = PG_GETARG_INT32(11);
	uint32		len[N_ARRAYS + 2] = {0};
	int64	   *arrays[N_ARRAYS + 2];
	Size		data_len = 0;
	SiftStarDimSpec specs[3];
	int			n_dims = with_regions ? 3 : 2;
	Size		shared_size,
				bundle_size;
	ParallelContext *pcxt = NULL;
	Shared	   *s;
	SiftStarDimBundle *b;
	void	   *raw_bundle;
	int			launched = 0;
	DimData		dd[3];
	SiftStarSource src[3];
	StarScan		sc = {0};
	int64	   *fi,
			   *fr;
	uint32		nf;

	InitMaterializedSRF(fcinfo, 0);
	sift_aggkern6_init();
	memset(&stats, 0, sizeof(stats));
	fi = take_array(PG_GETARG_ARRAYTYPE_P(0), &nf);
	fr = take_array(PG_GETARG_ARRAYTYPE_P(1), &len[8]);
	for (int a = 0; a < N_ARRAYS; a++)
		arrays[a] = take_array(PG_GETARG_ARRAYTYPE_P(2 + a), &len[a]);
	for (int a = 0; a < N_ARRAYS; a++)
		data_len += len[a];
	shared_size = add_size(offsetof(Shared, data), mul_size(sizeof(int64), Max(data_len, 1)));
	specs[0] = (SiftStarDimSpec) {kc, -1, false, PG_GETARG_BOOL(12), 0, len[A_C_ID], (kc == SDIM_INNER || kc == SDIM_LEFT) ? 1 : 0};
	specs[1] = (SiftStarDimSpec) {ki, 0, true, PG_GETARG_BOOL(13), 1, len[A_I_ID], (ki == SDIM_INNER || ki == SDIM_LEFT) ? 1 : 0};
	specs[2] = (SiftStarDimSpec) {kr, -1, true, true, 2, len[A_R_ID], (kr == SDIM_INNER || kr == SDIM_LEFT) ? 1 : 0};
	bundle_size = sift_star_size(specs, n_dims, workers + 1);
	if (workers == 0)
	{
		s = palloc0(shared_size);
		raw_bundle = palloc(bundle_size);
	}
	else
	{
		EnterParallelMode();
		pcxt = CreateParallelContext("pg_sift", "sift_star_test_worker", workers);
		shm_toc_estimate_chunk(&pcxt->estimator, shared_size);
		shm_toc_estimate_chunk(&pcxt->estimator, bundle_size);
		shm_toc_estimate_keys(&pcxt->estimator, 2);
		InitializeParallelDSM(pcxt);
		if (pcxt->seg == NULL)
			elog(ERROR, "no shared memory for the parallel test");
		s = shm_toc_allocate(pcxt->toc, shared_size);
		raw_bundle = shm_toc_allocate(pcxt->toc, bundle_size);
		memset(s, 0, shared_size);
	}
	s->n_dims = n_dims;
	memcpy(s->len, len, sizeof(s->len));
	memcpy(s->specs, specs, sizeof(specs));
	for (int a = 0, at = 0; a < N_ARRAYS; at += len[a], a++)
		memcpy(s->data + at, arrays[a], sizeof(int64) * len[a]);
	pg_atomic_init_u32(&s->arrived, 0);
	pg_atomic_init_u32(&s->go, 0);
	b = sift_star_init(raw_bundle, specs, n_dims, workers + 1);
	if (pcxt)
	{
		shm_toc_insert(pcxt->toc, KEY_SHARED, s);
		shm_toc_insert(pcxt->toc, KEY_BUNDLE, raw_bundle);
		LaunchParallelWorkers(pcxt);
		launched = pcxt->nworkers_launched;
		while (pg_atomic_read_u32(&s->arrived) < launched)
		{
			CHECK_FOR_INTERRUPTS();
			SPIN_DELAY();
		}
		pg_atomic_write_u32(&s->go, 1);
	}
	make_sources(s, dd, src);
	sift_star_build(b, src);
	if (pcxt)
		WaitForParallelWorkersToFinish(pcxt);
	stats.launched = launched;
	stats.bundle_bytes = (int64) bundle_size;
	stats.items_raw = (int32) b->dims[1].raw_rows;
	stats.items_surviving = (int32) b->dims[1].surviving_rows;
	stats.cats_surviving = (int32) b->dims[0].surviving_rows;
	stats.any_empty = sift_star_empty(b);
	for (int d = 0; d < n_dims; d++)
		stats.elided += b->dims[d].elided;

	if (!sift_star_empty(b))
	{
		int64	   *kv_i = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO),
				   *kv_r = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);

		sc.b = b;
		sc.rsi = rsi;
		sc.item_dim = 1;
		sc.region_dim = with_regions ? 2 : -1;
		for (uint32 from = 0; from < nf; from += granule)
		{
			const int	n = (int) Min(granule, nf - from);
			int64		zi_min = INT64_MAX,
						zi_max = INT64_MIN,
						zr_min = INT64_MAX,
						zr_max = INT64_MIN;
			bool		keep = true;

			for (int i = 0; i < n; i++)
			{
				zi_min = Min(zi_min, fi[from + i]);
				zi_max = Max(zi_max, fi[from + i]);
				if (with_regions)
				{
					zr_min = Min(zr_min, fr[from + i]);
					zr_max = Max(zr_max, fr[from + i]);
				}
			}
			stats.granules_total++;
			if (b->dims[1].push_sip && !sift_sip_probe_granule(sift_star_sip(b, 1), zi_min, zi_max, NULL))
				keep = false;
			if (keep && with_regions && b->dims[2].push_sip && !sift_sip_probe_granule(sift_star_sip(b, 2), zr_min, zr_max, NULL))
				keep = false;
			if (!keep)
			{
				stats.granules_pruned++;
				continue;
			}
			stats.fact_rows_read += n;
			scan_granule(&sc, fi, fr, (int32) from, n, kv_i, kv_r);
			for (int d = 1; d < n_dims; d++)
				sift_sip_adapt_granule_end(&sc.ps[d]);
		}
		for (int d = 1; d < n_dims; d++)
		{
			stats.rows_probed += sc.ps[d].stats.rows_in;
			stats.auto_disables += sc.ps[d].auto_disables;
		}
	}
	if (pcxt)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
	}
	return (Datum) 0;
}

/* sift._test_star_stats(): what the last sift._test_star did */
Datum
sift_test_star_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[12];
	bool		nulls[12] = {false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = Int32GetDatum(stats.items_raw);
	values[1] = Int32GetDatum(stats.items_surviving);
	values[2] = Int32GetDatum(stats.cats_surviving);
	values[3] = BoolGetDatum(stats.any_empty);
	values[4] = Int64GetDatum(stats.fact_rows_read);
	values[5] = Int32GetDatum(stats.granules_total);
	values[6] = Int32GetDatum(stats.granules_pruned);
	values[7] = Int64GetDatum(stats.rows_probed);
	values[8] = Int32GetDatum(stats.auto_disables);
	values[9] = Int32GetDatum(stats.elided);
	values[10] = Int32GetDatum(stats.launched);
	values[11] = Int64GetDatum(stats.bundle_bytes);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

#endif							/* SIFT_TESTING */
