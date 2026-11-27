/*
 * agg/star/exec.c - the table of a dimension under a budget past 1 GB (work_mem >= 2 GB a dimension): a row that would double its arena past one allocation
 * is refused, as a row past the budget is, so that the join reads the dimension in parts instead of failing at the allocator's limit.
 */
#include "postgres.h"

#include <sys/mman.h>

#include "utils/memutils.h"

#include "agg/star/star.h"
#include "test/common/check.h"
#include "test/common/pgshim.h"

#define BIG		((Size) 520 << 20)

typedef struct Fill
{
	SiftStarDim d;
	const char *bytes;
	bool		second_fits;
} Fill;

static SiftStarRow
row_of(const char *p, uint32 len)
{
	SiftStarRow r = {0};

	r.key_p[0] = p;
	r.key_len[0] = len;
	return r;
}

static void
fill(void *arg)
{
	Fill	   *f = arg;
	SiftStarShape sh = {.mode = SV6J_INNER_EQUI,.nkeys = 1,.kcls = {SSC_STR},.budget = UINT64_MAX};
	SiftStarRow r = row_of(f->bytes, BIG);

	sift_star_dim_init(&f->d, &sh);
	SIFT_CHECK(sift_star_dim_add(&f->d, &r));
	r = row_of(f->bytes, 16);
	f->second_fits = sift_star_dim_add(&f->d, &r);
}

static void
test_arena_past_one_allocation_is_refused(void)
{
	Fill		f = {.bytes = mmap(NULL, BIG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)};

	SIFT_CHECK(f.bytes != MAP_FAILED);
	SIFT_CHECKF(pgshim_try(fill, &f), "a row past one allocation failed: %s", pgshim_errmsg);
	SIFT_CHECKF(!f.second_fits, "a row that doubles the arena past MaxAllocSize was taken");
	munmap((void *) f.bytes, BIG);
	pgshim_reset();
}

int
main(void)
{
	pgshim_keep();
	test_arena_past_one_allocation_is_refused();
	return sift_check_summary();
}
