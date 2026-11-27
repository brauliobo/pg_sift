/*
 * agg/par/group_fold.c - the record of a set of no keys of a parallel aggregate: a record over 1 GB fails naming it before its buffer grows, the load of a participant counts
 * the record and tail buffers, and an export gives them back.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "utils/memutils.h"

#include "agg/par/group_fold.c"
#include "test/common/check.h"
#include "test/common/pgshim.h"

static SiftV6Spec spec;
static SiftV6Agg agg;
static ParEnv env;
static SiftV6Group group;
static V6Set set;
static V6Acc plain[1];
static ParLocal local;
static uint64 put_bytes;

static void
put(void *arg, const uint8 *rec, uint32 len)
{
	put_bytes += len;
}

/* the winner of the text MIN: len bytes in the arena */
static void
set_winner(uint32 len)
{
	SiftTextSlot *slot = (SiftTextSlot *) &plain[0];

	group.tx = (SiftTextArena) {.cx = CurrentMemoryContext,.buf = palloc(len),.cap = len,.used = len};
	*slot = (SiftTextSlot) {.n = 1,.v0 = 0,.v1 = len};
}

/* a participant whose one aggregate is a text MIN, its winner of len bytes in the arena */
static void
local_with_winner(uint32 len)
{
	agg = (SiftV6Agg) {.kind = SV6_MIN,.arg = {.typid = TEXTOID}};
	spec.naggs = 1;
	spec.agg = &agg;
	env = (ParEnv) {.spec = &spec,.has_tail = true};
	group = (SiftV6Group) {.spec = &spec,.cx = CurrentMemoryContext,.nsets = 1,.set = &set};
	set = (V6Set) {.owner = &group,.plain = plain};
	set_winner(len);
	local = (ParLocal) {.env = &env,.g = &group,.cx = CurrentMemoryContext};
	initStringInfo(&local.tail);
	put_bytes = 0;
}

static void
export_set(void *arg)
{
	export_plain(&local, 0, put, NULL);
}

static void
test_record_over_1gb_fails_before_it_grows(void)
{
	local_with_winner((uint32) MaxAllocSize - 16);
	SIFT_CHECK(!pgshim_try(export_set, NULL));
	SIFT_CHECKF(pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED && strstr(pgshim_errmsg, "over 1 GB") != NULL, "%s", pgshim_errmsg);
	SIFT_CHECKF(local.rec_cap == 0, "the record buffer did not grow: %zu", local.rec_cap);
	pgshim_reset();
}

static void
test_export_gives_back_its_buffers(void)
{
	const uint32 wide = 8 << 20;
	SiftV6RecSink sink = {.put = put};
	uint64		before;

	local_with_winner(wide);
	before = par_local_load(&local);
	record_room(&local, wide);
	enlargeStringInfo(&local.tail, wide);
	SIFT_CHECKF(par_local_load(&local) >= before + 2 * (uint64) wide, "the load counts the record and the tail: %llu after %llu", (unsigned long long) par_local_load(&local),
				(unsigned long long) before);
	set_winner(wide);
	par_local_export(&local, &sink);
	SIFT_CHECKF(put_bytes > wide, "the record went: %llu", (unsigned long long) put_bytes);
	SIFT_CHECKF(local.rec_cap == 0 && local.rec == NULL, "the record buffer is given back: %zu", local.rec_cap);
	SIFT_CHECKF(local.tail.maxlen < wide, "the tail is given back: %d", local.tail.maxlen);
	SIFT_CHECKF(par_local_load(&local) < wide, "the load after the export: %llu", (unsigned long long) par_local_load(&local));
	pgshim_reset();
}

int
main(void)
{
	pgshim_keep();
	test_export_gives_back_its_buffers();
	test_record_over_1gb_fails_before_it_grows();
	return sift_check_summary();
}
