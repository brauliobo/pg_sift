/*
 * check-memory: allow-allocs the arrays are sized by the rows of one test case, a few hundred thousand at most
 * agg/par/dim_shm_test.c - test-only SQL function of the shared dimension table (agg/par/dim_shm.c), compiled only with SIFT_TESTING
 * and registered by the pg_sift_test extension: the leader and 0, 1, 2 or 4 workers build the table of a dimension of deterministic
 * int64 keys (a key in several rows when the domain is smaller than the rows) in the parallel context, then probe it, each the
 * probes it claims: hits, misses and keys of the domain that may or may not be there. A worker writes its answers, the number of rows
 * of the key and their sum, to a file of the SharedFileSet of the context; the leader checks every answer, every probe once, and the
 * whole table against a sorted reference of the same rows: every key has exactly the rows of the reference. A table over the share of
 * work_mem the join may take is not built.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"

#include "access/parallel.h"
#include "access/xact.h"
#include "agg/par/dim_shm.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/buffile.h"
#include "storage/s_lock.h"
#include "storage/shm_toc.h"
#include "storage/sharedfileset.h"

PG_FUNCTION_INFO_V1(sift_test_dim_shm);
PGDLLEXPORT void sift_dim_shm_test_worker(dsm_segment *seg, shm_toc *toc);

#define KEY_DIM		UINT64CONST(0x5200000000000004)
#define KEY_PARAMS	UINT64CONST(0x5200000000000005)
#define KEY_FILES	UINT64CONST(0x5200000000000006)
#define KEY_PROBES	UINT64CONST(0x5200000000000007)

#define PROBE_BLOCK	64

typedef struct Params
{
	uint64		seed;
	uint32		nrows;
	uint32		domain;
	uint32		probes;
	pg_atomic_uint32 arrived pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 go pg_attribute_aligned(PG_CACHE_LINE_SIZE);
} Params;

typedef struct Answer
{
	int64		key;
	uint32		probe;
	uint32		nmatch;
	uint64		sum_rows;
} Answer;

typedef struct Ref
{
	int64		key;
	uint32		row;
} Ref;

static uint64
splitmix(uint64 x)
{
	x += UINT64CONST(0x9E3779B97F4A7C15);
	x = (x ^ (x >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	x = (x ^ (x >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return x ^ (x >> 31);
}

static int64
key_of(const Params *p, uint32 row)
{
	return (int64) (splitmix(p->seed + row * UINT64CONST(0x9E3779B97F4A7C15)) % p->domain) - (int64) (p->domain / 2);
}

static void
gen_keys(void *arg, uint32 first, uint32 n, int64 *keys)
{
	for (uint32 i = 0; i < n; i++)
		keys[i] = key_of(arg, first + i);
}

/* The key of probe i: a key of a row, a key past the domain, a key of the domain */
static int64
probe_key(const Params *p, uint32 i)
{
	const uint64 x = splitmix(p->seed ^ ((uint64) i << 20));

	switch (i % 3)
	{
		case 0:
			return key_of(p, (uint32) (x % p->nrows));
		case 1:
			return (int64) p->domain + i;
		default:
			return (int64) (x % p->domain) - (int64) (p->domain / 2);
	}
}

static SiftMorselQueue *
align_queue(void *raw)
{
	return (SiftMorselQueue *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
}

/* The build, then the probes the participant claims, answered into file; the microseconds of the build */
static int64
participant(SiftDimShm *d, Params *p, SiftMorselQueue *pq, BufFile *file)
{
	SiftMorsel	m;
	instr_time	t0,
				t1;

	INSTR_TIME_SET_CURRENT(t0);
	sift_dim_shm_build(d, gen_keys, p);
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_SUBTRACT(t1, t0);
	while (sift_morsel_claim(pq, (uint32) (ParallelWorkerNumber + 1), &m))
		for (uint32 i = m.first * PROBE_BLOCK; i < Min((m.first + m.n) * PROBE_BLOCK, p->probes); i++)
		{
			Answer		a = {.key = probe_key(p, i), .probe = i};
			SiftDimIter it;
			uint32		row;

			sift_dim_shm_find(d, a.key, &it);
			while (sift_dim_shm_next(&it, &row))
			{
				a.nmatch++;
				a.sum_rows += row;
			}
			BufFileWrite(file, &a, sizeof(a));
		}
	return (int64) INSTR_TIME_GET_MICROSEC(t1);
}

static char *
file_name(char *buf, size_t len, int id)
{
	snprintf(buf, len, "dim_probes_%d", id);
	return buf;
}

void
sift_dim_shm_test_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftDimShm *d = shm_toc_lookup(toc, KEY_DIM, false);
	Params	   *p = shm_toc_lookup(toc, KEY_PARAMS, false);
	SharedFileSet *fs = shm_toc_lookup(toc, KEY_FILES, false);
	SiftMorselQueue *pq = align_queue(shm_toc_lookup(toc, KEY_PROBES, false));
	char		name[32];
	BufFile    *file;

	SharedFileSetAttach(fs, seg);
	file = BufFileCreateFileSet(&fs->fs, file_name(name, sizeof(name), ParallelWorkerNumber + 1));
	pg_atomic_fetch_add_u32(&p->arrived, 1);
	while (pg_atomic_read_u32(&p->go) == 0)
	{
		CHECK_FOR_INTERRUPTS();
		SPIN_DELAY();
	}
	participant(d, p, pq, file);
	BufFileClose(file);
}

static int
ref_cmp(const void *a, const void *b)
{
	const Ref  *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->row < y->row ? -1 : x->row > y->row;
}

static int
row_cmp(const void *a, const void *b)
{
	const uint32 x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

/* The rows of the reference [lo, hi) of one key against the rows the table gives for it */
static bool
key_ok(SiftDimShm *d, const Ref *ref, uint32 lo, uint32 hi, uint32 *rows)
{
	SiftDimIter it;
	uint32		n = 0,
				row;

	sift_dim_shm_find(d, ref[lo].key, &it);
	while (sift_dim_shm_next(&it, &row))
	{
		if (n == hi - lo)
			return false;
		rows[n++] = row;
	}
	qsort(rows, n, sizeof(uint32), row_cmp);
	for (uint32 i = 0; i < n; i++)
		if (rows[i] != ref[lo + i].row)
			return false;
	return n == hi - lo;
}

/* The wrong answers of the file, marking its probes as seen */
static int64
check_answers(BufFile *file, const Params *p, const Ref *ref, uint8 *seen)
{
	Answer		a;
	int64		wrong = 0;

	BufFileSeek(file, 0, 0, SEEK_SET);
	while (BufFileReadMaybeEOF(file, &a, sizeof(a), true) != 0)
	{
		uint32		lo = 0,
					hi = p->nrows,
					n = 0;
		uint64		sum = 0;

		if (a.probe >= p->probes || seen[a.probe]++ || a.key != probe_key(p, a.probe))
		{
			wrong++;
			continue;
		}
		while (lo < hi)
		{
			const uint32 mid = lo + (hi - lo) / 2;

			if (ref[mid].key < a.key)
				lo = mid + 1;
			else
				hi = mid;
		}
		for (; lo + n < p->nrows && ref[lo + n].key == a.key; n++)
			sum += ref[lo + n].row;
		wrong += a.nmatch != n || a.sum_rows != sum;
	}
	return wrong;
}

static int64
check_table(SiftDimShm *d, const Params *p, const Ref *ref)
{
	uint32	   *rows = palloc(sizeof(uint32) * p->nrows);
	int64		wrong = 0;

	for (uint32 lo = 0, hi; lo < p->nrows; lo = hi)
	{
		for (hi = lo; hi < p->nrows && ref[hi].key == ref[lo].key; hi++)
			;
		wrong += !key_ok(d, ref, lo, hi, rows);
	}
	pfree(rows);
	return wrong;
}

static void
wait_for(pg_atomic_uint32 *counter, uint32 n)
{
	while (pg_atomic_read_u32(counter) < n)
	{
		CHECK_FOR_INTERRUPTS();
		SPIN_DELAY();
	}
}

/*
 * (seed, workers, nrows, domain, probes): the launched workers, the buckets, the probes answered, the wrong answers and keys, whether
 * the table fits the budget (when it does not nothing is built), its bytes and the microseconds the leader spent in the build.
 */
Datum
sift_test_dim_shm(PG_FUNCTION_ARGS)
{
	const int	workers = PG_GETARG_INT32(1);
	Params		p = {.seed = PG_GETARG_INT64(0), .nrows = PG_GETARG_INT32(2), .domain = PG_GETARG_INT32(3),
					 .probes = PG_GETARG_INT32(4)};
	const uint32 participants = workers + 1;
	const bool	fits = sift_dim_shm_fits(p.nrows, participants);
	const uint32 probe_granules = (p.probes + PROBE_BLOCK - 1) / PROBE_BLOCK;
	const Size	dim_size = sift_dim_shm_size(p.nrows, participants),
				pq_size = sift_morsel_size(1) + PG_CACHE_LINE_SIZE;
	ParallelContext *pcxt = NULL;
	SiftDimShm *d;
	Params	   *shared = &p;
	SiftMorselQueue *pq;
	SharedFileSet *fs = NULL;
	BufFile    *own;
	Ref		   *ref;
	uint8	   *seen;
	int			launched = 0;
	int64		wrong = 0,
				answered = 0;
	TupleDesc	tupdesc;
	int64		build_us;
	Datum		values[7];
	bool		nulls[7] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[4] = BoolGetDatum(fits);
	values[5] = Int64GetDatum((int64) dim_size);
	if (!fits)
	{
		values[0] = values[1] = Int32GetDatum(0);
		values[2] = values[3] = values[6] = Int64GetDatum(0);
		PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
	}

	if (workers == 0)
	{
		d = sift_dim_shm_init(palloc(dim_size), p.nrows, 1);
		pq = align_queue(palloc(pq_size));
	}
	else
	{
		EnterParallelMode();
		pcxt = CreateParallelContext("pg_sift", "sift_dim_shm_test_worker", workers);
		shm_toc_estimate_chunk(&pcxt->estimator, dim_size);
		shm_toc_estimate_chunk(&pcxt->estimator, sizeof(Params));
		shm_toc_estimate_chunk(&pcxt->estimator, sizeof(SharedFileSet));
		shm_toc_estimate_chunk(&pcxt->estimator, pq_size);
		shm_toc_estimate_keys(&pcxt->estimator, 4);
		InitializeParallelDSM(pcxt);
		if (pcxt->seg == NULL)
			elog(ERROR, "no shared memory for the parallel test");
		d = sift_dim_shm_init(shm_toc_allocate(pcxt->toc, dim_size), p.nrows, workers + 1);
		shared = shm_toc_allocate(pcxt->toc, sizeof(Params));
		fs = shm_toc_allocate(pcxt->toc, sizeof(SharedFileSet));
		pq = align_queue(shm_toc_allocate(pcxt->toc, pq_size));
	}
	if (shared != &p)
		memcpy(shared, &p, offsetof(Params, arrived));
	pg_atomic_init_u32(&shared->arrived, 0);
	pg_atomic_init_u32(&shared->go, 0);
	sift_morsel_init(pq, 1, &probe_granules, workers + 1);
	if (pcxt)
	{
		SharedFileSetInit(fs, pcxt->seg);
		shm_toc_insert(pcxt->toc, KEY_DIM, d);
		shm_toc_insert(pcxt->toc, KEY_PARAMS, shared);
		shm_toc_insert(pcxt->toc, KEY_FILES, fs);
		shm_toc_insert(pcxt->toc, KEY_PROBES, pq);
		LaunchParallelWorkers(pcxt);
		launched = pcxt->nworkers_launched;
		wait_for(&shared->arrived, launched);
		pg_atomic_write_u32(&shared->go, 1);
	}
	own = BufFileCreateTemp(false);
	build_us = participant(d, shared, pq, own);
	if (pcxt)
		WaitForParallelWorkersToFinish(pcxt);

	ref = palloc(sizeof(Ref) * p.nrows);
	for (uint32 r = 0; r < p.nrows; r++)
		ref[r] = (Ref) {key_of(&p, r), r};
	qsort(ref, p.nrows, sizeof(Ref), ref_cmp);
	seen = palloc0(p.probes);
	wrong += check_answers(own, &p, ref, seen);
	BufFileClose(own);
	for (int w = 1; w <= launched; w++)
	{
		char		name[32];
		BufFile    *f = BufFileOpenFileSet(&fs->fs, file_name(name, sizeof(name), w), O_RDONLY, false);

		wrong += check_answers(f, &p, ref, seen);
		BufFileClose(f);
	}
	for (uint32 i = 0; i < p.probes; i++)
	{
		answered += seen[i] == 1;
		wrong += seen[i] == 0;
	}
	wrong += check_table(d, &p, ref);
	values[0] = Int32GetDatum(launched);
	values[1] = Int32GetDatum((int32) d->nbuckets);
	values[2] = Int64GetDatum(answered);
	values[3] = Int64GetDatum(wrong);
	values[6] = Int64GetDatum(build_us);
	if (pcxt)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
	}
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#endif							/* SIFT_TESTING */
