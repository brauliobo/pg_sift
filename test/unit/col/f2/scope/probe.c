/*
 * test/unit/col/f2/scope/probe.c - the keys of a scope that a probed pair map holds (col/f2/scope/probe.c) when its survey spilled past maintenance_work_mem: the distinct keys of the
 * spool are read into one array of their size, not doubled from 1024, which put 67 million keys past one allocation (1 GB); the array of the keys of a member grows to at most one
 * allocation; and the loops of the survey and of the keys answer a cancel (S120-11). The spool is a sorted array here.
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
#include "col/f2/scope/probe.c"
#undef palloc
#undef repalloc

volatile sig_atomic_t InterruptPending = false;
static int	interrupts;

void
ProcessInterrupts(void)
{
	interrupts++;
}

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

#define DISTINCT	1100
#define REPEATS		3

/* The keys of a set put to a spool (survey_spill), each three times more as the rows that follow: the map holds each once, ascending, in an array of their size */
static void
spilled_set(void)
{
	Survey		sv = {0};
	int64	   *keys;
	uint32		n;

	sift_u64_set_init(&sv.keys, 16);
	for (int64 k = 0; k < DISTINCT; k++)
		sift_u64_set_add(&sv.keys, (uint64) ((k * 7919) % DISTINCT) * 3);
	interrupts = 0;
	survey_spill(&sv);
	SIFT_CHECKF(interrupts > 0, "the spill of a set of %d keys answers a cancel", DISTINCT);
	for (int r = 0; r < REPEATS; r++)
		for (int64 k = DISTINCT - 1; k >= 0; k--)
			sift_scope_spool_put(sv.spill, k * 3, 0);
	largest = 0;
	interrupts = 0;
	keys = set_keys(&sv, &n);
	SIFT_CHECKF(n == DISTINCT, "%u distinct keys, not %d", n, DISTINCT);
	for (uint32 i = 0; i < n; i++)
		SIFT_CHECKF(keys[i] == (int64) i * 3, "key %u is %lld, not %lld", i, (long long) keys[i], (long long) i * 3);
	SIFT_CHECKF(largest <= sizeof(int64) * DISTINCT, "the keys of the map ask for %zu bytes, more than the %d keys they are", (size_t) largest, DISTINCT);
	SIFT_CHECKF(interrupts >= (1 + REPEATS) * DISTINCT, "the read of %d spooled keys answers a cancel at each, %d times", (1 + REPEATS) * DISTINCT, interrupts);
	pfree(keys);
}

static void
empty_spill(void)
{
	Survey		sv = {0};
	uint32		n = 1;

	sv.spill = sift_scope_spool_new(NULL);
	pfree(set_keys(&sv, &n));
	SIFT_CHECK(n == 0);
}

/* The keys of one member grown past half of one allocation (64 Mi of them, 512 MB): the next growth stops at one allocation, it does not ask for 1 GB */
static void
member_keys_cap(void *arg)
{
	PairSource *s = arg;

	keys_add(s, s->capkeys, 1);
}

static void
member_keys_at_limit(void)
{
	PairSource	s = {.cx = CurrentMemoryContext,.capkeys = 1u << 26};

	s.keys = MemoryContextAllocHuge(s.cx, sizeof(int64) * s.capkeys);
	SIFT_CHECKF(pgshim_try(member_keys_cap, &s), "the keys of a member grow past %u: %s", 1u << 26, pgshim_errmsg);
	SIFT_CHECKF((Size) s.capkeys * sizeof(int64) <= MaxAllocSize && s.capkeys > (1u << 26) && s.keys[1u << 26] == 1, "the keys of a member hold %u in one allocation", s.capkeys);
	pfree(s.keys);
}

int
main(void)
{
	InterruptPending = true;
	spilled_set();
	empty_spill();
	member_keys_at_limit();
	return sift_check_summary();
}
