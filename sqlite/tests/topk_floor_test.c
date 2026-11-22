/*
 * tests/topk_floor_test.c - the floor of bm25/kernels.c's top-k: the K-th score of a search that read fewer terms bounds the K-th score of one that reads more,
 * and a search that starts at that floor returns the hits of one that starts at none. A program of its own on the portable host (no database, so ASan and UBSan run it).
 *
 * Scores are small multiples of an eighth, so that ties are everywhere and the tie-break on the key is exercised. Pass one scores a random subset of the documents; pass two
 * scores every document at least as high as pass one did (a document pass one did not read scores 0 there).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/host.h"
#include "bm25/kernels.h"

#define N		3000
#define ROUNDS	400

static int64_t checks,
			failed;

#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t rng = 88172645463325252ULL;

static uint64_t
rnd(void)
{
	uint64_t	z = (rng += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static float4 s1[N],
			s2[N];

static int64
search(const float4 *score, float4 floor, SiftHit *buf, int64 k, SiftTopK *t)
{
	sift_kern_topk_init(t, buf, k, k);
	t->floor = floor;
	for (int64 i = 0; i < N; i++)
		if (score[i] > 0 && score[i] > sift_kern_topk_theta(t) && sift_kern_topk_admits(t, score[i], i))
			sift_kern_topk_push(t, score[i], i);
	return t->n;
}

static void
test_theta(void)
{
	SiftHit		buf[4];
	SiftTopK	t;

	sift_kern_topk_init(&t, buf, 4, 4);
	CHECK(sift_kern_topk_theta(&t) == -INFINITY);
	t.floor = 2.5f;
	CHECK(sift_kern_topk_theta(&t) == 2.5f);
	for (int i = 0; i < 4; i++)
		sift_kern_topk_push(&t, 1.0f + i, i);
	CHECK(sift_kern_topk_theta(&t) == 2.5f);
	t.floor = 0.5f;
	CHECK(sift_kern_topk_theta(&t) == nextafterf(1.0f, -INFINITY));
	t.floor = -INFINITY;
	CHECK(sift_kern_topk_theta(&t) == nextafterf(1.0f, -INFINITY));
}

static void
test_floor_keeps_the_hits(void)
{
	static SiftHit a[64],
				b[64];

	for (int round = 0; round < ROUNDS; round++)
	{
		int64		k = 1 + rnd() % 40,
					read = 0;
		SiftTopK	t1,
					t2,
					t3;
		float4		floor = -INFINITY;

		for (int64 i = 0; i < N; i++)
		{
			s1[i] = rnd() % 5 == 0 ? (float4) (1 + rnd() % 24) / 8.0f : 0.0f;
			s2[i] = s1[i] + (rnd() % 3 == 0 ? (float4) (1 + rnd() % 24) / 8.0f : 0.0f);
			read += s1[i] > 0;
		}
		if (search(s1, -INFINITY, a, k, &t1) == k)
			floor = sift_kern_topk_theta(&t1);
		CHECK(read < k || floor > -INFINITY);
		CHECK(search(s2, -INFINITY, a, k, &t2) == search(s2, floor, b, k, &t3));
		CHECK(t2.n == t3.n);
		sift_kern_topk_sort(&t2);
		sift_kern_topk_sort(&t3);
		for (int64 i = 0; i < t2.n && i < t3.n; i++)
			CHECK(a[i].key == b[i].key && a[i].score == b[i].score);
		if (floor > -INFINITY && t2.n == k)
			CHECK(floor < a[k - 1].score);
	}
}

int
main(void)
{
	test_theta();
	test_floor_keeps_the_hits();
	printf("topk_floor_test: %lld checks, %lld failed\n", (long long) checks, (long long) failed);
	return failed ? 1 : 0;
}
