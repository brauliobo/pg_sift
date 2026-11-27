/*
 * test/unit/col/f2/scan/stream_cap.c - the addresses of an unordered scan (col/f2/scan/stream.c) move to the sort once the next array would pass 1 GB, whatever work_mem allows:
 * at work_mem = 1GB the array of 67,108,864 keys doubled to exactly 1 GB, which repalloc refuses (S119-08).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "col/f2/scan/stream.c"

#define KEYS_AT_HALF_GB	((uint64) 1 << 26)

int			work_mem = 1048576;

static bool taken;

static void
put_past_half_gb(void *arg)
{
	F2Scan	   *s = arg;

	s->stream->n = s->stream->cap = KEYS_AT_HALF_GB;
	taken = sift_f2_stream_put(s, 1);
}

int
main(void)
{
	F2Scan		s = {.cx = CurrentMemoryContext};

	SIFT_CHECK(sift_f2_stream_put(&s, 0));
	s.stream->key = MemoryContextAlloc(s.cx, KEYS_AT_HALF_GB * sizeof(uint64));
	SIFT_CHECKF(pgshim_try(put_past_half_gb, &s), "a key past half a GB at work_mem = 1GB raises no error: %s", pgshim_errmsg);
	SIFT_CHECK(!taken);
	return sift_check_summary();
}
