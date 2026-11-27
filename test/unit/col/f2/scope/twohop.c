/*
 * test/unit/col/f2/scope/twohop.c - the pair map a scope makes of pairs that went to a sort (col/f2/scope/twohop.c sorted_map) is the map sift_spm_build makes of the same pairs, with each
 * array allocated once at the size it holds: none asks for twice what it keeps, which put the arrays of 67 million members past one allocation (1 GB), and a sort with no pair makes an
 * empty map (S119-24). The spool is a sorted array here.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"
#include "test/common/pgshim.h"

static Size largest;

static void *
track_palloc(Size n)
{
	largest = Max(largest, n);
	return palloc(n);
}

static void *
track_repalloc(void *p, Size n)
{
	largest = Max(largest, n);
	return repalloc(p, n);
}

#define palloc(n)		track_palloc(n)
#define repalloc(p, n)	track_repalloc(p, n)
#include "col/f2/scope/twohop.c"
#undef palloc
#undef repalloc

volatile sig_atomic_t InterruptPending = false;

struct SiftScopeSpool
{
	SiftScopeMemberKey *v;
	uint32		n;
	uint32		cap;
	uint32		at;
};

SiftScopeSpool *
sift_scope_spool_new(SiftMemTracker *mem)
{
	return calloc(1, sizeof(SiftScopeSpool));
}

void
sift_scope_spool_put(SiftScopeSpool *s, int64 a, int64 b)
{
	if (s->n == s->cap)
		s->v = realloc(s->v, sizeof(*s->v) * (s->cap = Max(64, 2 * s->cap)));
	s->v[s->n++] = (SiftScopeMemberKey) {a, b};
}

static int
row_order(const void *a, const void *b)
{
	const SiftScopeMemberKey *x = a,
			   *y = b;

	if (x->member != y->member)
		return x->member < y->member ? -1 : 1;
	return x->key < y->key ? -1 : x->key > y->key;
}

void
sift_scope_spool_sort(SiftScopeSpool *s)
{
	if (s->n > 0)
		qsort(s->v, s->n, sizeof(*s->v), row_order);
	s->at = 0;
}

void
sift_scope_spool_rewind(SiftScopeSpool *s)
{
	s->at = 0;
}

bool
sift_scope_spool_next(SiftScopeSpool *s, int64 *a, int64 *b)
{
	if (s->at == s->n)
		return false;
	*a = s->v[s->at].member;
	*b = s->v[s->at++].key;
	return true;
}

void
sift_scope_spool_free(SiftScopeSpool *s)
{
	free(s->v);
	free(s);
}

static uint64 seed = 88172645463325252u;

static uint32
rnd(uint32 n)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return (uint32) (seed % n);
}

static void
same_map(const SiftScopePairMap *x, const SiftScopePairMap *y)
{
	SIFT_CHECK(x->nkeys == y->nkeys && x->nmembers == y->nmembers && x->npairs == y->npairs);
	SIFT_CHECK(memcmp(x->scope_keys, y->scope_keys, sizeof(int64) * x->nkeys) == 0);
	SIFT_CHECK(memcmp(x->member_ids, y->member_ids, sizeof(int64) * x->nmembers) == 0);
	SIFT_CHECK(memcmp(x->offsets, y->offsets, sizeof(uint32) * (x->nmembers + 1)) == 0);
	SIFT_CHECK(memcmp(x->key_ordinals, y->key_ordinals, sizeof(uint32) * x->npairs) == 0);
}

/* Random pairs, repeated ones among them, as a sort and as an array: the two maps are equal, and no array of the sorted one is larger than its members */
static void
matches_array_build(int round)
{
	uint32		n = 1 + rnd(3000),
				members = 1 + rnd(n),
				keys = 1 + rnd(64);
	bool		multiset = round % 2;
	SiftScopeSpool *s = sift_scope_spool_new(NULL);
	SiftScopeMemberKey *rows = palloc(sizeof(SiftScopeMemberKey) * n);
	SiftScopePairMap *sorted,
			   *built;

	for (uint32 i = 0; i < n; i++)
	{
		rows[i] = (SiftScopeMemberKey) {rnd(members), rnd(keys)};
		sift_scope_spool_put(s, rows[i].member, rows[i].key);
	}
	largest = 0;
	sorted = sorted_map(s, NULL, multiset);
	SIFT_CHECKF(largest <= Max(sizeof(int64) * Max(sorted->nmembers, sorted->nkeys), sizeof(uint32) * Max(sorted->npairs, sorted->nmembers + 1)),
				"an array of the map asks for %zu bytes, more than the %u members, %u keys and %u pairs it holds", (size_t) largest, sorted->nmembers, sorted->nkeys,
				sorted->npairs);
	built = sift_spm_build(rows, n, multiset);
	same_map(sorted, built);
	sift_spm_free(sorted);
	sift_spm_free(built);
	sift_scope_spool_free(s);
}

static void
empty_sort(void)
{
	SiftScopeSpool *s = sift_scope_spool_new(NULL);
	SiftScopePairMap *pm = sorted_map(s, NULL, false);

	SIFT_CHECK(pm->nkeys == 0 && pm->nmembers == 0 && pm->npairs == 0 && pm->offsets[0] == 0);
	sift_spm_free(pm);
	sift_scope_spool_free(s);
}

int
main(void)
{
	for (int round = 0; round < 64; round++)
		matches_array_build(round);
	empty_sort();
	return sift_check_summary();
}
