/*
 * check-memory: allow-allocs the arrays are sized by the rows of one test case, a few hundred thousand at most
 * agg/par/radix_test.c - test-only SQL function of the two-phase parallel aggregation (agg/par/radix.c), compiled only with
 * SIFT_TESTING and registered by the pg_sift_test extension: a leader and 1, 2 or 4 workers aggregate the same deterministic
 * rows, as granules of SIFT_GRANULE_ROWS claimed in morsels, through the local tables, the radix partitions (in the shared segment,
 * or in the files of the SharedFileSet when the budget of chunks is small) and the parallel merge, and the groups that come back
 * through the queues are compared field by field with the groups of a sorted reference of the same rows.
 *
 * Modes: 0 GROUP BY an inline key (count, sum, min, max and an exact 128-bit sum), 1 the same with a wide key (24 bytes and null
 * flags), 2 the same with a count(DISTINCT) per group, 3 an ungrouped count(DISTINCT), which the partitions split by the hash of
 * the value: the values are the keys and every partition gives the number of its groups.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"

#include "access/parallel.h"
#include "agg/v6/cost/model.h"
#include "agg/group/state.h"
#include "agg/par/radix.h"
#include "core/testrng.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(sift_test_radix_par);
PGDLLEXPORT void sift_radix_test_worker(dsm_segment *seg, shm_toc *toc);

#define KEY_COORD	UINT64CONST(0x5200000000000001)
#define KEY_PARAMS	UINT64CONST(0x5200000000000002)

typedef __int128 i128;

enum
{
	MODE_INLINE,
	MODE_WIDE,
	MODE_DISTINCT,
	MODE_UNGROUPED
};

typedef struct Params
{
	uint64		seed;
	uint32		groups;
	uint32		mode;
	uint32		granules;
} Params;

typedef struct Ext
{
	SiftStateSum128 sum;
	SiftDistSet *dist;
	int64		pad;
} Ext;

typedef struct OutRec
{
	uint64		id;
	int64		cnt;
	int64		sum;
	int64		mn;
	int64		mx;
	SiftStateSum128 s;
	uint64		dist;
	bool		key_ok;
} OutRec;

typedef struct Gen
{
	uint64		id;
	int64		v;
	uint64		d;
} Gen;

static Gen
gen_row(const Params *p, uint64 r)
{
	const uint64 x = sift_mix64(p->seed + r * SIFT_FIB64),
				y = sift_mix64(x);
	Gen			g;

	g.id = x % p->groups + 1;
	g.v = (int64) y >> ((x >> 40) % 40);
	g.d = p->mode == MODE_UNGROUPED ? y % ((uint64) p->groups * 4) + 1 : (y >> 7) % 50 + 1;
	return g;
}

static SiftInlineKey16
inline_key(uint64 id)
{
	SiftInlineKey16 k;

	k.u64[0] = id;
	k.u64[1] = id * 7 + 1;
	return k;
}

static void
wide_key(uint64 id, uint8 *buf, uint64 *nulls)
{
	const uint64 b = id * SIFT_FIB64,
				c = id ^ UINT64CONST(0xABCDEF0123456789);

	memcpy(buf, &id, 8);
	memcpy(buf + 8, &b, 8);
	memcpy(buf + 16, &c, 8);
	*nulls = id % 5 == 0 ? 5 : 0;
}

static uint64
wide_hash(const uint8 *buf, uint64 nulls)
{
	return sift_hash_bytes(buf, 24) ^ (nulls * UINT64CONST(0xBF58476D1CE4E5B9));
}

static void
shape_init(const SiftRadixShape *sh, SiftAggSlot64 *s, uint8 *ext)
{
	sift_slot_cmsm_init(s);
}

static void
shape_merge(const SiftRadixShape *sh, SiftAggSlot64 *dst, uint8 *dst_ext, const SiftAggSlot64 *src, const uint8 *src_ext)
{
	sift_slot_cmsm_merge(dst, src);
	sift_state_sum128_merge(&((Ext *) dst_ext)->sum, &((const Ext *) src_ext)->sum);
}

static SiftRadixShape
make_shape(uint32 mode)
{
	SiftRadixShape sh = {.wide_key = mode == MODE_WIDE, .ext_dist_off = SIFT_HT_NO_EXT};

	if (mode != MODE_UNGROUPED)
	{
		sh.ext_stride = sizeof(Ext);
		sh.init = shape_init;
		sh.merge = shape_merge;
	}
	if (mode == MODE_DISTINCT)
		sh.ext_dist_off = offsetof(Ext, dist);
	return sh;
}

/* Phase 1 of a participant: the morsels it claims, row by row into its local table */
static void
scan_morsels(SiftParCoord *c, const SiftRadixShape *sh, const Params *p)
{
	SiftRadixLocal *l = sift_radix_local_new(c, sh);
	uint32		first,
				n;
	uint64		rows = 0;

	while (sift_par_next_morsel(c, &first, &n))
		for (uint64 r = (uint64) first * SIFT_GRANULE_ROWS; r < (uint64) (first + n) * SIFT_GRANULE_ROWS; r++, rows++)
		{
			const Gen	g = gen_row(p, r);
			SiftAggSlot64 *s;
			bool		found;

			if (p->mode == MODE_UNGROUPED)
			{
				const SiftInlineKey16 k = inline_key(g.d);

				sift_radix_local_upsert(l, sift_aggk6_hash1(g.d), &k, &found);
				continue;
			}
			if (p->mode == MODE_WIDE)
			{
				uint8		kb[24];
				uint64		nulls;

				wide_key(g.id, kb, &nulls);
				s = sift_radix_local_upsert_wide(l, wide_hash(kb, nulls), kb, 24, nulls, &found);
			}
			else
			{
				const SiftInlineKey16 k = inline_key(g.id);

				s = sift_radix_local_upsert(l, sift_aggk6_hash1(g.id), &k, &found);
			}
			sift_slot_cmsm_add(s, g.v);
			sift_state_sum128_add(&((Ext *) sift_radix_local_ext(l, s))->sum,
								  (SiftI128) {(uint64) g.v << 40, g.v >> 24});
			if (p->mode == MODE_DISTINCT)
				sift_radix_local_dist_add(l, s, g.d);
		}
	pg_atomic_fetch_add_u64(&c->total_rows_scanned, rows);
	sift_radix_local_finish(l);
}

static void
emit_group(SiftRadixSink *sink, const SiftSaltedHT *ht, const SiftAggSlot64 *s, const Params *p)
{
	OutRec		o;
	const Ext  *e = ht->ext_stride ? (const Ext *) sift_ht_ext(ht, s) : NULL;

	memset(&o, 0, sizeof(o));
	if (p->mode == MODE_WIDE)
	{
		uint8		kb[24];
		uint64		nulls;
		uint32		len;
		const uint8 *key = sift_ht_wide_key(ht, s, &len);

		memcpy(&o.id, key, 8);
		wide_key(o.id, kb, &nulls);
		o.key_ok = len == 24 && !memcmp(key, kb, 24) && s->key.wide.key_nulls == nulls && s->hash64 == wide_hash(kb, nulls);
	}
	else
	{
		o.id = s->key.u64[0];
		o.key_ok = s->key.u64[1] == o.id * 7 + 1 && s->hash64 == sift_aggk6_hash1(o.id);
	}
	o.cnt = s->agg.i64[0];
	o.sum = s->agg.i64[1];
	o.mn = s->agg.i64[2];
	o.mx = s->agg.i64[3];
	o.s = e->sum;
	o.dist = p->mode == MODE_DISTINCT && e->dist ? sift_dist_count(e->dist) : 0;
	sift_radix_sink_put(sink, &o, sizeof(o));
}

/* Phase 2 of a participant: the partitions it claims, merged and emitted */
static void
merge_parts(SiftParCoord *c, const SiftRadixShape *sh, const Params *p, SiftRadixSink *sink)
{
	uint32		part;

	while (sift_par_next_part(c, &part))
	{
		SiftSaltedHT ht;
		SiftAggSlot64 *s;

		sift_radix_merge_part(c, part, sh, &ht);
		if (p->mode == MODE_UNGROUPED)
		{
			OutRec		o;

			memset(&o, 0, sizeof(o));
			o.cnt = ht.count;
			sift_radix_sink_put(sink, &o, sizeof(o));
		}
		else
			SIFT_HT_FOREACH(&ht, s)
				emit_group(sink, &ht, s, p);
		pg_atomic_fetch_add_u64(&c->total_groups_out, ht.count);
		sift_ht_destroy(&ht);
	}
}

void
sift_radix_test_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParCoord *c = shm_toc_lookup(toc, KEY_COORD, false);
	const Params *p = shm_toc_lookup(toc, KEY_PARAMS, false);
	const SiftRadixShape sh = make_shape(p->mode);
	shm_mq_handle *mqh = sift_par_worker_attach(c, seg);
	SiftRadixSink *sink;

	if (sift_radix_join(c) == 0)
	{
		scan_morsels(c, &sh, p);
		sift_radix_scan_done(c);
	}
	sink = sift_radix_sink_new(mqh, NULL, NULL);
	merge_parts(c, &sh, p, sink);
	sift_radix_sink_finish(sink);
	sift_radix_leave(c);
	shm_mq_detach(mqh);
}

typedef struct Collected
{
	OutRec	   *recs;
	uint32		n;
	uint32		cap;
} Collected;

static void
collect(void *arg, const void *rec, uint32 len)
{
	Collected  *c = arg;

	Assert(len == sizeof(OutRec));
	if (c->n == c->cap)
	{
		OutRec	   *grown = palloc_aligned(sizeof(OutRec) * (c->cap ? c->cap * 2 : 1024), _Alignof(OutRec), 0);

		if (c->recs != NULL)
		{
			memcpy(grown, c->recs, sizeof(OutRec) * c->n);
			pfree(c->recs);
		}
		c->recs = grown;
		c->cap = c->cap ? c->cap * 2 : 1024;
	}
	memcpy(&c->recs[c->n++], rec, sizeof(OutRec));
}

static int
out_cmp(const void *a, const void *b)
{
	const OutRec *x = a,
			   *y = b;

	return x->id < y->id ? -1 : x->id > y->id;
}

static int
gen_cmp(const void *a, const void *b)
{
	const Gen  *x = a,
			   *y = b;

	if (x->id != y->id)
		return x->id < y->id ? -1 : 1;
	return x->d < y->d ? -1 : x->d > y->d;
}

/* The groups of the rows of the sorted reference [lo, hi) against a collected one */
static bool
group_ok(const Gen *rows, uint32 lo, uint32 hi, const OutRec *o, bool distinct)
{
	int64		sum = 0,
				mn = INT64_MAX,
				mx = INT64_MIN;
	i128		wide = 0;
	uint64		dist = 0;

	for (uint32 i = lo; i < hi; i++)
	{
		sum = (int64) ((uint64) sum + (uint64) rows[i].v);
		mn = Min(mn, rows[i].v);
		mx = Max(mx, rows[i].v);
		wide += (i128) rows[i].v << 40;
		dist += i == lo || rows[i].d != rows[i - 1].d;
	}
	return o->key_ok && o->cnt == (int64) (hi - lo) && o->sum == sum && o->mn == mn && o->mx == mx &&
		o->s.n == (int64) (hi - lo) && o->s.sum.lo == (uint64) wide && o->s.sum.hi == (int64) (wide >> 64) &&
		(!distinct || o->dist == dist);
}

/* The number of groups of the collected set that differ from the reference, a missing or an extra one counting */
static int64
compare_groups(const Params *p, Gen *rows, uint32 n, Collected *got)
{
	int64		bad = 0;
	uint32		j = 0;

	qsort(rows, n, sizeof(Gen), gen_cmp);
	qsort(got->recs, got->n, sizeof(OutRec), out_cmp);
	for (uint32 lo = 0, hi; lo < n; lo = hi)
	{
		for (hi = lo; hi < n && rows[hi].id == rows[lo].id; hi++)
			;
		if (j < got->n && got->recs[j].id == rows[lo].id)
			bad += !group_ok(rows, lo, hi, &got->recs[j++], p->mode == MODE_DISTINCT);
		else
			bad++;
	}
	return bad + (got->n - j);
}

static int64
compare_distinct_total(Gen *rows, uint32 n, Collected *got, uint32 n_parts)
{
	int64		total = 0,
				expect = 0;

	for (uint32 i = 0; i < got->n; i++)
		total += got->recs[i].cnt;
	qsort(rows, n, sizeof(Gen), gen_cmp);
	for (uint32 i = 0; i < n; i++)
		expect += i == 0 || rows[i].d != rows[i - 1].d;
	return (total != expect) + (got->n != n_parts);
}

/*
 * (seed, workers, granules, groups, mode, local_cap, max_chunks): the launched workers, the groups out, the number of wrong
 * groups, the partitions that went to files, the chunks taken, the microseconds from the launch to the last output and to the end of the scan. max_chunks 0: the budget of work_mem and hash_mem_multiplier.
 */
Datum
sift_test_radix_par(PG_FUNCTION_ARGS)
{
	const int	workers = PG_GETARG_INT32(1);
	const uint32 max_chunks = PG_GETARG_INT32(6) ? (uint32) PG_GETARG_INT32(6) : sift_par_budget_chunks();
	Params		p = {.seed = PG_GETARG_INT64(0), .granules = PG_GETARG_INT32(2), .groups = PG_GETARG_INT32(3),
					 .mode = PG_GETARG_INT32(4)};
	const SiftRadixShape sh = make_shape(p.mode);
	const Size	size = sift_par_coord_size(max_chunks, workers);
	ParallelContext *pcxt;
	SiftParCoord *c;
	Params	   *shared;
	shm_mq_handle **queues;
	Collected	got = {0};
	SiftRadixSink *sink;
	Gen		   *rows;
	const uint32 nrows = p.granules * SIFT_GRANULE_ROWS;
	int			launched;
	int64		bad;
	int			spilled = 0;
	TupleDesc	tupdesc;
	instr_time	t0,
				t1,
				t2;
	Datum		values[7];
	bool		nulls[7] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	EnterParallelMode();
	pcxt = CreateParallelContext("pg_sift", "sift_radix_test_worker", workers);
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(Params) + SIFT_PAR_SHM_ALIGN);
	shm_toc_estimate_keys(&pcxt->estimator, 2);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
		elog(ERROR, "no shared memory for the parallel test");
	shared = (Params *) TYPEALIGN(SIFT_PAR_SHM_ALIGN, shm_toc_allocate(pcxt->toc, sizeof(Params) + SIFT_PAR_SHM_ALIGN));
	*shared = p;
	c = sift_par_coord_init(shm_toc_allocate(pcxt->toc, size), pcxt->seg, max_chunks, workers, p.granules, PG_GETARG_INT32(5));
	shm_toc_insert(pcxt->toc, KEY_COORD, c);
	shm_toc_insert(pcxt->toc, KEY_PARAMS, shared);
	INSTR_TIME_SET_CURRENT(t0);
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;
	queues = sift_par_leader_queues(c, pcxt->seg, pcxt);

	if (sift_radix_join(c) == 0)
	{
		scan_morsels(c, &sh, &p);
		sift_radix_scan_done(c);
	}
	INSTR_TIME_SET_CURRENT(t2);
	INSTR_TIME_SUBTRACT(t2, t0);
	sink = sift_radix_sink_new(NULL, collect, &got);
	merge_parts(c, &sh, &p, sink);
	sift_radix_sink_finish(sink);
	sift_radix_leave(c);
	sift_radix_drain(queues, launched, collect, &got);
	WaitForParallelWorkersToFinish(pcxt);
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_SUBTRACT(t1, t0);

	for (uint32 i = 0; i < c->n_parts; i++)
		spilled += pg_atomic_read_u32(&c->parts[i].spilled);
	rows = palloc(sizeof(Gen) * nrows);
	for (uint32 r = 0; r < nrows; r++)
	{
		rows[r] = gen_row(&p, r);
		if (p.mode == MODE_UNGROUPED)
			rows[r].id = 0;
	}
	bad = p.mode == MODE_UNGROUPED ? compare_distinct_total(rows, nrows, &got, c->n_parts) : compare_groups(&p, rows, nrows, &got);
	bad += pg_atomic_read_u64(&c->total_rows_scanned) != nrows;
	values[0] = Int32GetDatum(launched);
	values[1] = Int64GetDatum((int64) pg_atomic_read_u64(&c->total_groups_out));
	values[2] = Int64GetDatum(bad);
	values[3] = Int32GetDatum(spilled);
	values[4] = Int32GetDatum((int32) Min(pg_atomic_read_u32(&c->next_free_chunk), c->max_chunks));
	values[5] = Int64GetDatum((int64) INSTR_TIME_GET_MICROSEC(t1));
	values[6] = Int64GetDatum((int64) INSTR_TIME_GET_MICROSEC(t2));
	DestroyParallelContext(pcxt);
	ExitParallelMode();
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#endif							/* SIFT_TESTING */
