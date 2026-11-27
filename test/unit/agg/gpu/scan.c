/*
 * test/unit/agg/gpu/scan.c - the candidates of a GPU top-N (agg/gpu/scan.c): the batches of a scan each bring their best rows, and the backend keeps twice the k of the LIMIT at most, so a
 * scan of many batches holds as many candidates as one batch does, and the k it keeps are the k best of all of them.
 */
#include "postgres.h"

#include "agg/gpu/scan.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

#define BATCHES		20
#define PER_BATCH	2000
#define K			10

static void
locate(const SiftGpuPlan *p, uint32 i, uint32 *part_id, uint64 *part_gen, uint32 *granule)
{
	*part_id = 0;
	*part_gen = 1;
	*granule = i;
}

static int64
key_of(int b, int i)
{
	return (int64) ((uint64) (b * PER_BATCH + i) * 2654435761u % 1000003);
}

static void
test_candidates_stay_within_twice_k(void)
{
	const SiftGpuOps ops = {.locate = locate};
	SiftGpuPlan plan = {.ops = &ops};
	Exec		x = {.plan = &plan};
	TopnTake	t = {.cap = 1024,.k = K,.desc = true};
	int64	   *cand = palloc(2 * PER_BATCH * sizeof(int64));
	uint32		peak = 0,
				above = 0;

	t.cand = palloc(t.cap * sizeof(SiftGpuCand));
	for (int b = 0; b < BATCHES; b++)
	{
		const SiftVkResult res = {.cand = cand,.ncand = PER_BATCH};

		for (int i = 0; i < PER_BATCH; i++)
		{
			cand[2 * i] = key_of(b, i);
			cand[2 * i + 1] = (int64) b << 32 | i;
		}
		take_cand(&x, &res, &t);
		peak = Max(peak, t.cap);
	}
	SIFT_CHECK(peak == 1024);
	cand_keep(&t);
	sift_nth_sort(t.cand, t.n, sizeof(SiftGpuCand), cand_cmp, &t);
	for (int i = 1; i < K; i++)
		SIFT_CHECK(t.cand[i - 1].key >= t.cand[i].key);
	for (int b = 0; b < BATCHES; b++)
		for (int i = 0; i < PER_BATCH; i++)
			above += key_of(b, i) > t.cand[K - 1].key;
	SIFT_CHECK(t.n == K && above < K);
	pgshim_reset();
}

int
main(void)
{
	test_candidates_stay_within_twice_k();
	puts("test_gpu_scan: ok");
	return 0;
}
