/*
 * test_distinct - the exact COUNT(DISTINCT) set (agg/distinct/distinct.c) past 1 GB: its table grows as far as its budget, and a key of more than ~477 MB goes to its partition
 * file as one record of its bytes, with no scratch twice its size.
 */
#include "postgres.h"

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "agg/distinct/distinct.c"

#define WIDE_KEY	((uint32) 506 << 20)

static uint32 spilled_len;
static uint8 spilled_mark;

SiftSpillSet *
sift_spill_create(MemoryContext cx, uint32 nparts)
{
	return (SiftSpillSet *) palloc(1);
}

uint32
sift_spill_nparts_of(const SiftSpillSet *s)
{
	return SIFT_SPILL_MIN_PARTS;
}

void
sift_spill_put(SiftSpillSet *s, uint32 part, uint64 tag, const void *bytes, uint32 len)
{
	spilled_mark = *(const uint8 *) bytes;
	spilled_len = len;
}

static void
resize_past_1gb(void *arg)
{
	dist_tab_resize(arg, (uint32) 1 << 26);
}

static void
test_table_grows_past_1gb_within_its_budget(void)
{
	SiftDistinct *d = sift_distinct_create(CurrentMemoryContext, (uint64) 4 << 30);

	SIFT_CHECK(pgshim_try(resize_past_1gb, d));
	SIFT_CHECK(d->cap == (uint32) 1 << 26 && d->tab[d->cap - 1].key == NULL);
	pgshim_reset();
}

typedef struct Wide
{
	SiftDistinct *d;
	uint8	   *key;
} Wide;

static void
spill_wide(void *arg)
{
	Wide	   *w = arg;

	w->d->spill = sift_spill_create(w->d->cx, SIFT_SPILL_MIN_PARTS);
	dist_put_spill(w->d, 1, 7, w->key, WIDE_KEY);
}

static void
test_wide_key_spills_as_one_whole_record(void)
{
	Wide		w = {.d = sift_distinct_create(CurrentMemoryContext, 64 * 1024),.key = calloc(1, WIDE_KEY)};

	SIFT_CHECK(pgshim_try(spill_wide, &w));
	SIFT_CHECK(spilled_mark == UINT8_MAX && spilled_len == WIDE_KEY + sizeof(uint32) + 1);
	SIFT_CHECK(w.d->scratch_cap == spilled_len);
	free(w.key);
	pgshim_reset();
}

int
main(void)
{
	test_table_grows_past_1gb_within_its_budget();
	test_wide_key_spills_as_one_whole_record();
	puts("test_distinct: ok");
	return 0;
}
