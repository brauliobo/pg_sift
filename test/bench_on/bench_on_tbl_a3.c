/*
 * bench_on_tbl_a3.c - the touched rows of a transaction (table/f2/f2_lock_touched.c) against a model: random adds, removals by position (as release_tier1 walks them) and forgets keep the
 * list of the subtransaction, the dense array and the hash of it in step, and the cost per operation does not grow with the rows held. Standalone: the file is included, with the
 * calls of the server it makes provided here; test/bench_on/run.sh builds it with ASan and UBSan.
 */
#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#undef printf
#undef fprintf

#include "../../src/table/f2/f2_lock_touched.c"

static MemoryContext txn_cx = NULL;

MemoryContext
sift_rowlock_txn_cx(void)
{
	return txn_cx;
}

SubTransactionId
GetCurrentSubTransactionId(void)
{
	return 1;
}

TransactionId
GetCurrentTransactionId(void)
{
	return 700;
}

void *
MemoryContextAlloc(MemoryContext context, Size size)
{
	return malloc(size);
}

void *
MemoryContextAllocZero(MemoryContext context, Size size)
{
	return calloc(1, size);
}

void *
repalloc(void *p, Size size)
{
	return realloc(p, size);
}

void
pfree(void *p)
{
	free(p);
}

uint32
hash_bytes(const unsigned char *k, int keylen)
{
	uint64		h = UINT64CONST(1469598103934665603);

	for (int i = 0; i < keylen; i++)
		h = (h ^ k[i]) * UINT64CONST(1099511628211);
	return (uint32) (h ^ (h >> 32));
}

bool
errstart_cold(int elevel, const char *domain)
{
	fprintf(stderr, "FAIL: an error was raised\n");
	exit(1);
}

int
errmsg_internal(const char *fmt,...)
{
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	exit(1);
}

static uint64
rnd(void)
{
	static uint64 s = 88172645463325252ull;

	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	return s;
}

static double
clock_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	exit(1);
}

static SiftRowLockKey
key_of(uint64 tid)
{
	SiftRowLockKey k = {0};

	k.locator.relNumber = 5;
	k.tid = tid;
	return k;
}

/* the list, the array and the hash agree: every entry is on the list once, found by its key, and held at its slot */
static void
check(void)
{
	uint32		seen = 0;

	for (int32 i = sift_touched_sub_first(1); i >= 0; i = sift_touched_next((uint32) i))
	{
		SiftRowLockKey k = sift_touched_at((uint32) i)->key;

		if ((uint32) i >= ntouched || ++seen > ntouched)
			fail("the list leaves the array");
		if (!is_touched(&k, 1))
			fail("an entry of the list is not found by its key");
		if (ixcap != 0 && touched_ix[ix_slot_of((uint32) i)] != (uint32) i + 1)
			fail("an entry is not at its slot");
	}
	if (seen != ntouched)
		fail("the list does not hold every entry");
}

static double
run(uint32 rows, bool model)
{
	double		t0,
				t1;
	uint32		ops = 0;

	sift_touched_reset();
	t0 = clock_ns();
	for (uint32 i = 0; i < rows; i++)
	{
		SiftRowLockKey k = key_of(i + 1);

		if (!sift_touched_add(&k, 1) || sift_touched_add(&k, 1))
			fail("an add is not once");
		ops += 2;
		if (model && i % 7 == 0)
			check();
	}
	while (ntouched > 0)
	{
		uint32		at = (uint32) (rnd() % ntouched);
		int32		next = sift_touched_next(at);
		int32		moved;

		if (rnd() % 3 == 0)
		{
			SiftRowLockKey k = sift_touched_at(at)->key;

			sift_touched_forget(&k);
			if (is_touched(&k, 1))
				fail("a forgotten row is still held");
		}
		else
		{
			SiftRowLockKey k = sift_touched_at(at)->key;

			moved = sift_touched_remove(at);
			(void) next;
			(void) moved;
			if (is_touched(&k, 1))
				fail("a removed row is still held");
		}
		ops++;
		if (model && ntouched % 5 == 0)
			check();
	}
	t1 = clock_ns();
	if (sift_touched_sub_first(1) >= 0)
		fail("a list is left with no row");
	sift_touched_reset();
	return (t1 - t0) / ops;
}

/* the walk of release_tier1: entries of the list are removed where they match, and the position held for the next step follows the entry that moved */
static void
walk(uint32 rows, unsigned modulus)
{
	uint32		kept = 0;
	int32		i;

	sift_touched_reset();
	for (uint32 r = 0; r < rows; r++)
	{
		SiftRowLockKey k = key_of(r + 1);

		(void) sift_touched_add(&k, 1);
		kept += r % modulus != 0;
	}
	for (i = sift_touched_sub_first(1); i >= 0;)
	{
		int32		next = sift_touched_next((uint32) i),
					moved;

		if ((sift_touched_at((uint32) i)->key.tid - 1) % modulus != 0)
		{
			i = next;
			continue;
		}
		if ((uint32) i >= ntouched || (next >= 0 && (uint32) next >= ntouched))
		{
			fprintf(stderr, "walk: rows=%u modulus=%u i=%d next=%d ntouched=%u\n", rows, modulus, i, next, ntouched);
			fail("a position of the walk is out of range");
		}
		moved = sift_touched_remove((uint32) i);
		i = moved >= 0 && moved == next ? i : next;
	}
	if (ntouched != kept)
		fail("the walk left other rows than the ones that did not match");
	check();
	for (i = sift_touched_sub_first(1); i >= 0; i = sift_touched_next((uint32) i))
		if ((sift_touched_at((uint32) i)->key.tid - 1) % modulus == 0)
			fail("a matching row is left");
	sift_touched_reset();
}

int
main(void)
{
	for (unsigned m = 1; m <= 5; m++)
	{
		walk(10, m);
		walk(100, m);
		walk(5000, m);
	}
	double		small,
				large;

	(void) run(40, true);
	(void) run(300, true);
	(void) run(3000, true);
	small = run(1000, false);
	large = run(100000, false);
	printf("touched rows: %.1f ns/op at 1000 rows, %.1f ns/op at 100000 rows (ratio %.2f)\n", small, large, large / small);
	if (large > 20 * small)
		fail("the cost per operation grows with the rows held");
	printf("ok\n");
	return 0;
}
