/*
 * agg/ht/rec.c - the buffer of the records of the groups: past 512 MB it grows to one allocation, never to a request the allocator refuses, and a record
 * of more than one allocation (a wide key and a wide distinct value) fails naming it.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/ht/rec.c"
#include "test/common/check.h"
#include "test/common/pgshim.h"

#define BIG		((Size) 520 << 20)

typedef struct Grow
{
	SiftRecBuf	b;
	Size		len;
} Grow;

static void
grow(void *arg)
{
	Grow	   *g = arg;

	buf_reserve(&g->b, g->len);
}

static void
test_buffer_past_512mb_grows_within_one_allocation(void)
{
	Grow		g = {.b = {.p = palloc(BIG),.cap = BIG},.len = BIG + 64};

	SIFT_CHECKF(pgshim_try(grow, &g), "growing past 512 MB failed: %s", pgshim_errmsg);
	SIFT_CHECKF(g.b.cap >= g.len && g.b.cap <= MaxAllocSize, "cap %u", g.b.cap);
	pgshim_reset();
}

typedef struct Dist
{
	SiftRecBuf	b;
	SiftSaltedHT ht;
	SiftAggSlot64 s;
} Dist;

static void
wide_distinct(void *arg)
{
	Dist	   *d = arg;

	build_distinct(&d->b, &d->ht, &d->s, REC_DISTINCT_BYTES, 0, (const uint8 *) &d->s, (uint32) MaxAllocSize);
}

static void
test_record_past_one_allocation_fails_naming_it(void)
{
	Dist		d = {0};

	SIFT_CHECK(!pgshim_try(wide_distinct, &d));
	SIFT_CHECKF(pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED && strstr(pgshim_errmsg, "the record of a group") != NULL, "%s", pgshim_errmsg);
	pgshim_reset();
}

int
main(void)
{
	pgshim_keep();
	test_buffer_past_512mb_grows_within_one_allocation();
	test_record_past_one_allocation_fails_naming_it();
	return sift_check_summary();
}
