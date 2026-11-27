/*
 * agg/group/fast.c - the key bytes of a batch of groups: a batch of 512 MB to 1 GB grows its buffer to one allocation, never by doubling past it, and a batch
 * of more fails naming it. A text MIN or MAX of more than 512 MB takes a block of its own length, not the next power of 2.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/group/fast.c"
#include "test/common/check.h"
#include "test/common/pgshim.h"

#define BIG		((Size) 520 << 20)

typedef struct Room
{
	SiftWideBatch wb;
	size_t		need;
} Room;

static void
room(void *arg)
{
	Room	   *r = arg;

	batch_room(&r->wb, r->need);
}

static Room
room_of(size_t need)
{
	return (Room) {.wb = {.buf = palloc(BIG),.cap = BIG},.need = need};
}

static void
test_batch_under_1gb_fits_one_allocation(void)
{
	Room		r = room_of(BIG + 1);

	SIFT_CHECKF(pgshim_try(room, &r), "a batch of 520 MB failed: %s", pgshim_errmsg);
	SIFT_CHECKF(r.wb.cap >= r.need && r.wb.cap <= MaxAllocSize, "cap %zu", r.wb.cap);
	pgshim_reset();
}

static void
test_batch_over_1gb_fails_naming_it(void)
{
	Room		r = room_of((size_t) MaxAllocSize + 1);

	SIFT_CHECK(!pgshim_try(room, &r));
	SIFT_CHECKF(strstr(pgshim_errmsg, "the keys of a batch of groups") != NULL, "%s", pgshim_errmsg);
	pgshim_reset();
}

typedef struct Offer
{
	SiftStrSlab *sl;
	SiftStrMM	mm;
	const uint8 *p;
	uint32		len;
} Offer;

static void
offer(void *arg)
{
	Offer	   *o = arg;

	sift_strmm_offer(o->sl, &o->mm, true, o->p, o->len);
}

static void
test_text_min_over_512mb_fits_one_block(void)
{
	Offer		o = {.sl = sift_strslab_new(CurrentMemoryContext),.p = palloc0(((Size) 512 << 20) + 1),.len = ((uint32) 512 << 20) + 1};

	SIFT_CHECKF(pgshim_try(offer, &o), "a MIN of 512 MB + 1 failed: %s", pgshim_errmsg);
	SIFT_CHECKF(o.mm.len == o.len && o.mm.cap >= o.len && o.mm.cap < ((uint32) 1 << 30), "len %u cap %u", o.mm.len, o.mm.cap);
	pgshim_reset();
}

int
main(void)
{
	pgshim_keep();
	test_batch_under_1gb_fits_one_allocation();
	test_batch_over_1gb_fails_naming_it();
	test_text_min_over_512mb_fits_one_block();
	return sift_check_summary();
}
