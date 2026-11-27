/*
 * agg/join/grace.c - the limits of the partitioned hash join: a row whose key and payload together pass one allocation (or wrap 32 bits) fails naming it before
 * anything is copied, and the loops over a skewed bucket (the pairs of a probe row, the rehash of the table) take a cancel.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/join/grace.c"
#include "test/common/check.h"
#include "test/common/pgshim.h"

volatile sig_atomic_t InterruptPending = false;
static int	interrupts;
static int	emitted;

void
ProcessInterrupts(void)
{
	InterruptPending = false;
	interrupts++;
	ereport(ERROR, (errcode(ERRCODE_QUERY_CANCELED), errmsg("canceling statement due to user request")));
}

static void
emit(void *arg, const uint8 *probe, uint32 plen, const uint8 *build, uint32 blen)
{
	if (++emitted == 1)
		InterruptPending = true;
}

typedef struct Row
{
	SiftGraceJoin *j;
	const void *key;
	uint32		klen;
	const void *payload;
	uint32		plen;
	int			n;
	int			cancel_at;
} Row;

static SiftGraceJoin *
join_new(void)
{
	return sift_gjoin_create(CurrentMemoryContext, SGJ_INNER, (uint64) 64 << 20, 0, emit, NULL);
}

static void
build(void *arg)
{
	Row		   *r = arg;

	for (int i = 0; i < r->n; i++)
	{
		if (i == r->cancel_at)
			InterruptPending = true;
		sift_gjoin_build(r->j, r->key, r->klen, false, r->payload, r->plen);
	}
}

static void
probe(void *arg)
{
	Row		   *r = arg;

	sift_gjoin_probe(r->j, r->key, r->klen, false, r->payload, r->plen);
}

static void
test_row_over_1gb_fails_naming_it(void)
{
	Row			r = {.j = join_new(),.key = palloc(600 << 20),.klen = 600 << 20,.payload = palloc(600 << 20),.plen = 600 << 20,.n = 1,.cancel_at = -1};

	SIFT_CHECK(!pgshim_try(build, &r));
	SIFT_CHECKF(pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED && strstr(pgshim_errmsg, "a row of a hash join") != NULL, "%s", pgshim_errmsg);
	pgshim_reset();
}

static void
test_row_past_4gb_does_not_wrap(void)
{
	static const uint8 small[16];
	Row			r = {.j = join_new(),.key = small,.klen = 0xC0000000u,.payload = small,.plen = 0x50000000u,.n = 1,.cancel_at = -1};

	SIFT_CHECK(!pgshim_try(build, &r));
	SIFT_CHECKF(strstr(pgshim_errmsg, "a row of a hash join") != NULL, "%s", pgshim_errmsg);
	r.j = join_new();
	sift_gjoin_build_done(r.j);
	SIFT_CHECK(!pgshim_try(probe, &r));
	SIFT_CHECKF(strstr(pgshim_errmsg, "a row of a hash join") != NULL, "the probe side too: %s", pgshim_errmsg);
	pgshim_reset();
}

static void
test_skewed_bucket_takes_a_cancel(void)
{
	const uint32 key = 42,
				payload = 7;
	Row			r = {.j = join_new(),.key = &key,.klen = sizeof(key),.payload = &payload,.plen = sizeof(payload),.n = 1000,.cancel_at = 64};

	interrupts = 0;
	SIFT_CHECKF(!pgshim_try(build, &r) && interrupts == 1, "the rehash of a table takes a cancel: %d", interrupts);
	r.j = join_new();
	r.cancel_at = -1;
	SIFT_CHECK(pgshim_try(build, &r));
	sift_gjoin_build_done(r.j);
	emitted = 0;
	SIFT_CHECKF(!pgshim_try(probe, &r) && interrupts == 2 && emitted == 1, "the pairs of one key take a cancel: %d after %d pairs", interrupts, emitted);
	InterruptPending = false;
	pgshim_reset();
}

int
main(void)
{
	pgshim_keep();
	test_row_over_1gb_fails_naming_it();
	test_skewed_bucket_takes_a_cancel();
	test_row_past_4gb_does_not_wrap();
	return sift_check_summary();
}
