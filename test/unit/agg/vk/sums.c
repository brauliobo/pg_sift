/*
 * test_vk_sums - the sums of floats the device hands back (agg/vk/fin.c): a sum that is not finite may be the overflow of finite values, which PostgreSQL raises, or a sum of an infinity
 * the table holds, which it returns, and the device cannot tell them apart, so the CPU runs the scan; the sums that are finite, and the words of the other aggregates, are taken as they are.
 */
#include "postgres.h"

#include <math.h>

#include "agg/vk/job.h"
#include "test/common/check.h"

static int64_t
bits(double d)
{
	int64_t		b;

	memcpy(&b, &d, sizeof(b));
	return b;
}

static SiftVkJob
job_of(SiftVkMode mode, uint32_t nkeys, uint32_t naggs, const uint8_t *kinds)
{
	SiftVkJob	j = {.mode = mode, .nkeys = nkeys, .naggs = naggs};

	for (uint32_t i = 0; i < naggs; i++)
		j.agg[i].kind = kinds[i];
	return j;
}

static const uint8_t count_and_sum[] = {SIFT_VK_AK_COUNT_STAR, SIFT_VK_AK_SUM_F};
static const uint8_t sum_i_and_min[] = {SIFT_VK_AK_SUM_I, SIFT_VK_AK_MIN_F};

static void
test_ungrouped(void)
{
	SiftVkJob	j = job_of(SIFT_VK_UNGROUPED, 0, 2, count_and_sum);
	int64_t		w[7] = {2, 2, 0, 0, bits(3.5), 0, 2};
	SiftVkResult res = {.acc = w, .acc_words = 7, .ngroups = 1};

	SIFT_CHECK(sift_vk_sums_finite(&j, &res));
	w[4] = bits(INFINITY);
	SIFT_CHECK(!sift_vk_sums_finite(&j, &res));
	w[4] = bits(-INFINITY);
	SIFT_CHECK(!sift_vk_sums_finite(&j, &res));
	w[4] = bits(NAN);
	SIFT_CHECK(!sift_vk_sums_finite(&j, &res));
}

static void
test_other_aggregates_are_not_sums(void)
{
	SiftVkJob	j = job_of(SIFT_VK_UNGROUPED, 0, 2, sum_i_and_min);
	int64_t		w[7] = {1, bits(INFINITY), 0, 1, bits(INFINITY), 0, 1};
	SiftVkResult res = {.acc = w, .acc_words = 7, .ngroups = 1};

	SIFT_CHECK(sift_vk_sums_finite(&j, &res));
}

static void
test_dense(void)
{
	SiftVkJob	j = job_of(SIFT_VK_DENSE, 1, 2, count_and_sum);
	int64_t		w[3 * 7] = {0};
	SiftVkResult res = {.acc = w, .acc_words = 3 * 7, .ngroups = 3};

	for (int g = 0; g < 3; g++)
		w[g * 7 + 4] = bits(1e308);
	SIFT_CHECK(sift_vk_sums_finite(&j, &res));
	w[2 * 7 + 4] = bits(INFINITY);
	SIFT_CHECK(!sift_vk_sums_finite(&j, &res));
}

static void
test_hash_records(void)
{
	SiftVkJob	j = job_of(SIFT_VK_HASH, 2, 2, count_and_sum);
	const uint32_t rec = 2 + 2 + 3 * 2;
	int64_t		w[3 * 10] = {0};
	SiftVkResult res = {.acc = w, .acc_words = 3 * rec, .ngroups = 3, .rec_words = rec};

	j.finish = SIFT_VK_FIN_GROUPS;
	for (int g = 0; g < 3; g++)
	{
		w[g * rec] = 7;
		w[g * rec + 2 + 2 + 3 + 0] = bits(-1e308);
	}
	SIFT_CHECK(sift_vk_sums_finite(&j, &res));
	w[1 * rec + 2 + 2 + 3 + 0] = bits(INFINITY);
	SIFT_CHECK(!sift_vk_sums_finite(&j, &res));
	w[1 * rec + 2 + 2 + 3 + 0] = bits(0);
	w[1 * rec + 0] = bits(INFINITY);
	SIFT_CHECK(sift_vk_sums_finite(&j, &res));
}

static void
test_results_that_hold_no_sums(void)
{
	SiftVkJob	j = job_of(SIFT_VK_HASH, 1, 2, count_and_sum);
	int64_t		w[4] = {bits(INFINITY), bits(INFINITY), bits(INFINITY), bits(INFINITY)};
	SiftVkResult res = {.acc = w, .acc_words = 4, .ngroups = 4};

	j.finish = SIFT_VK_FIN_DENSE_COUNT;
	SIFT_CHECK(sift_vk_sums_finite(&j, &res));
	j.finish = SIFT_VK_FIN_COUNT;
	res.acc = NULL;
	SIFT_CHECK(sift_vk_sums_finite(&j, &res));
	j = job_of(SIFT_VK_TOPN, 0, 0, NULL);
	res = (SiftVkResult) {.ncand = 2};
	SIFT_CHECK(sift_vk_sums_finite(&j, &res));
}

int
main(void)
{
	test_ungrouped();
	test_other_aggregates_are_not_sums();
	test_dense();
	test_hash_records();
	test_results_that_hold_no_sums();
	puts("test_vk_sums: ok");
	return 0;
}
