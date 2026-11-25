/*
 * test_inset.c - agg/v6/inset/filter.c: the set of a column = ANY (array) is tested per row by its bloom filter and table when they fit work_mem and by a binary search of the values when
 * they do not, and the two answer alike.
 */
#include "postgres.h"

#include "agg/v6/inset/inset.h"
#include "core/hash.h"
#include "pgshim.h"
#include "unit.h"

int			work_mem = 4096;

#define BATCH	SIFT_VEC_LEN

static int64 *
ascending(int n, int64 step)
{
	int64	   *v = palloc(n * sizeof(int64));

	for (int i = 0; i < n; i++)
		v[i] = (int64) i * step - 1000;
	return v;
}

static uint64
member_bits(const SiftV6InSet *s, const int64 *probe)
{
	SiftVec		col = {.data = (void *) probe,.len = BATCH,.all_valid = true,.dense = true,.type = SVEC_I64};
	uint64		out[SIFT_VEC_WORDS];
	uint16		left;
	uint64		sum = 0;

	memset(out, 0xff, sizeof(out));
	left = sift_v6_inset_filter(s, &col, BATCH, out, BATCH);
	for (int w = 0; w < SIFT_VEC_WORDS; w++)
		sum = sum * 31 + out[w];
	return sum * 31 + left;
}

static void
probes(int64 *p, int64 step)
{
	for (int i = 0; i < BATCH; i++)
		p[i] = i % 2 == 0 ? (int64) (sift_fmix64(i) % 400000) * step - 1000 : (int64) (sift_fmix64(i) % 400000) * step - 999;
}

static void
test_budget(void)
{
	const int	n = 300000;
	const int64 step = 3;
	int64	   *vals = ascending(n, step),
				probe[BATCH];
	SiftV6InSet hashed = {.n = n,.vals = vals},
				searched = {.n = n,.vals = vals};

	probes(probe, step);
	work_mem = 1 << 20;
	sift_v6_inset_index(CurrentMemoryContext, &hashed);
	CHECK(hashed.table != NULL && hashed.bloom != NULL);
	work_mem = 1024;
	sift_v6_inset_index(CurrentMemoryContext, &searched);
	CHECK(searched.table == NULL && searched.bloom == NULL);
	CHECK(member_bits(&hashed, probe) == member_bits(&searched, probe));
	work_mem = 4096;
}

static void
test_small(void)
{
	int64	   *vals = ascending(10, 7);
	SiftV6InSet s = {.n = 10,.vals = vals};

	sift_v6_inset_index(CurrentMemoryContext, &s);
	CHECK(s.table == NULL && s.vmin == vals[0] && s.vmax == vals[9]);
}

int
main(void)
{
	pgshim_keep();
	test_small();
	test_budget();
	puts("test_inset: ok");
	return 0;
}
