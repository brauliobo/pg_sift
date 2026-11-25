/*
 * par_rows.c - agg/v6/par/rows.h: the K best rows of 1 to 4 threads are those of the rows sorted whole (integer and text columns, ascending and descending, NULLs first and last, ties of every
 * column but the address), on threads that the trap shim flags as workers (no palloc, no ereport).
 */
#include "postgres.h"

#include "agg/v6/par/rows.h"
#include "core/hash.h"
#include "core/par/pool.h"
#include "par_host.h"
#include "pgshim.h"
#include "unit.h"

#define ROWS		(1u << 17)
#define BATCH		1024
#define TEXT_MAX	300

typedef struct Data
{
	int64		key[SIFT_PAR_ROWS_MAX_KEYS][ROWS];
	bool		isnull[SIFT_PAR_ROWS_MAX_KEYS][ROWS];
	SiftStrRef	str[SIFT_PAR_ROWS_MAX_KEYS][ROWS];
	uint64		tid[ROWS];
	uint8		bytes[ROWS][TEXT_MAX];
} Data;

static Data *D;

typedef struct Case
{
	SiftParRowsSpec sp;
	uint64		out[SIFT_PAR_ROWS_K_MAX];
	uint32		nout;
} Case;

static uint64
mix(uint64 i, int j)
{
	return sift_fmix64(i * 7 + j + 1);
}

static void
make_data(void)
{
	D = malloc(sizeof(Data));
	CHECK(D != NULL);
	for (uint32 i = 0; i < ROWS; i++)
	{
		D->tid[i] = (uint64) i * 3 + 1;
		for (int j = 0; j < SIFT_PAR_ROWS_MAX_KEYS; j++)
		{
			const uint64 h = mix(i, j);
			const uint32 len = (uint32) (h >> 20) % TEXT_MAX;

			D->key[j][i] = j == 0 ? (int64) (h % 5000) - 2500 : (int64) (h % 40);
			D->isnull[j][i] = h % 97 == 0;
			memset(D->bytes[i], 'a' + (int) (h % 3), len);
			D->bytes[i][len > 0 ? len - 1 : 0] = (uint8) ('a' + h % 7);
			D->str[j][i] = (SiftStrRef) {.p = D->bytes[i],.len = D->isnull[j][i] ? SIFT_STR_NULL : (h >> 8) % 5 == 0 ? 0 : len};
		}
	}
}

static int
cmp_rows(const SiftParRowsSpec *sp, uint32 a, uint32 b)
{
	for (int j = 0; j < sp->nkeys; j++)
	{
		int			c = 0;

		if (D->isnull[j][a] != D->isnull[j][b])
			return D->isnull[j][a] == sp->nulls_first[j] ? -1 : 1;
		if (D->isnull[j][a])
			continue;
		if (sp->str[j])
		{
			const uint32 la = D->str[j][a].len,
						lb = D->str[j][b].len,
						n = la < lb ? la : lb;

			c = n > 0 ? memcmp(D->str[j][a].p, D->str[j][b].p, n) : 0;
			c = c != 0 ? c : la == lb ? 0 : la < lb ? -1 : 1;
		}
		else
			c = D->key[j][a] == D->key[j][b] ? 0 : D->key[j][a] < D->key[j][b] ? -1 : 1;
		if (c != 0)
			return sp->desc[j] ? -c : c;
	}
	return a < b ? -1 : a > b;
}

static const SiftParRowsSpec *sort_spec;

static int
qcmp(const void *a, const void *b)
{
	return cmp_rows(sort_spec, *(const uint32 *) a, *(const uint32 *) b);
}

static bool
test_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParRowsBatch *out, SiftErr *err)
{
	const SiftParRowsSpec *sp = &((const Case *) arg)->sp;

	sift_test_in_worker = worker != 0;
	out->n = (hi - lo) * BATCH;
	out->tid = D->tid + lo * BATCH;
	for (int j = 0; j < sp->nkeys; j++)
	{
		out->key[j] = D->key[j] + lo * BATCH;
		out->isnull[j] = D->isnull[j] + lo * BATCH;
		out->str[j] = D->str[j] + lo * BATCH;
	}
	return true;
}

static void
test_emit(void *arg, const SiftParRowsRow *row)
{
	Case	   *c = arg;
	const uint32 i = c->nout++;

	c->out[i] = row->tid;
	for (int j = 0; j < c->sp.nkeys; j++)
	{
		const uint32 r = (uint32) ((row->tid - 1) / 3);

		CHECK(row->isnull[j] == D->isnull[j][r]);
		if (row->isnull[j])
			continue;
		if (c->sp.str[j])
			CHECK(row->str[j].len == D->str[j][r].len && (row->str[j].len == 0 || memcmp(row->str[j].p, D->str[j][r].p, row->str[j].len) == 0));
		else
			CHECK(row->key[j] == D->key[j][r]);
	}
}

static void
run_case(const SiftParRowsSpec *sp, int nthreads, const uint32 *order)
{
	static Case c;
	SiftParArenas *ar;
	SiftParRows *r;
	SiftErr		err = {0};
	const SiftParRowsHooks hooks = {.fill = test_fill,.emit = test_emit,.arg = &c};

	pgshim_reset();
	memset(&c, 0, sizeof(c));
	c.sp = *sp;
	ar = sift_par_arenas_create(CurrentMemoryContext, nthreads, 64 << 10, 1 << 20, 16 << 20);
	r = sift_par_rows_create(ar, sp, &hooks, &err);
	CHECK(r != NULL);
	CHECK(sift_par_run(&(SiftParCfg) {.nthreads = nthreads,.items = ROWS / BATCH}, &sift_par_rows_ops, r, ar, NULL) && !sift_par_pool_busy());
	CHECK(c.nout == sp->k);
	for (uint32 i = 0; i < c.nout; i++)
		CHECK(c.out[i] == D->tid[order[i]]);
}

static void
test_spec(const SiftParRowsSpec *sp, uint32 *order)
{
	sort_spec = sp;
	for (uint32 i = 0; i < ROWS; i++)
		order[i] = i;
	qsort(order, ROWS, sizeof(uint32), qcmp);
	for (int w = 1; w <= 4; w++)
		run_case(sp, w, order);
}

int
main(void)
{
	uint32	   *order = malloc(sizeof(uint32) * ROWS);
	const uint16 ks[] = {1, 10, 100, 1000};
	int			ncases = 0;

	CHECK(order != NULL);
	pgshim_keep();
	make_data();
	for (int ki = 0; ki < 4; ki++)
		for (int shape = 0; shape < 12; shape++)
		{
			SiftParRowsSpec sp = {.nkeys = 1 + shape % 3,.k = ks[ki]};

			for (int j = 0; j < sp.nkeys; j++)
			{
				sp.str[j] = shape >= 6 && (j == 0 || shape % 2 == 0);
				sp.desc[j] = (shape + j) % 2 == 1;
				sp.nulls_first[j] = (shape / 3 + j) % 2 == 1;
			}
			test_spec(&sp, order);
			ncases++;
		}
	free(order);
	free(D);
	par_host_proc_exit();
	printf("par_rows: ok (%d cases on 1 to 4 threads)\n", ncases);
	return 0;
}
