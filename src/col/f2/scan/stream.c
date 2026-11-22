/*
 * col/f2/scan/stream.c - the addresses of an unordered scan held in memory in the order of the addresses (col/f2/scan/stream.h).
 *
 * check-memory: allow-allocs the keys are bounded by work_mem, past which the scan sorts on disk
 */
#include "postgres.h"

#include "miscadmin.h"

#include "col/f2/scan/stream.h"

struct SiftF2Stream
{
	uint64	   *key;
	uint64		n;
	uint64		cap;
	int64		pos;			/* the next key to hand out: -1 past the first when going backward */
	bool		started;
};

static int
key_cmp(const void *a, const void *b)
{
	const uint64 x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

bool
sift_f2_stream_put(F2Scan *s, uint64 key)
{
	SiftF2Stream *st = s->stream;

	if (st == NULL)
		st = s->stream = MemoryContextAllocZero(s->cx, sizeof(SiftF2Stream));
	if (st->n == st->cap)
	{
		const uint64 cap = st->cap == 0 ? 1024 : st->cap * 2;

		if (cap * sizeof(uint64) > (uint64) work_mem * 1024)
			return false;
		st->key = st->key != NULL ? repalloc(st->key, cap * sizeof(uint64)) : MemoryContextAlloc(s->cx, cap * sizeof(uint64));
		st->cap = cap;
	}
	st->key[st->n++] = key;
	return true;
}

void
sift_f2_stream_drain(F2Scan *s, void (*put) (F2Scan *s, uint64 key))
{
	SiftF2Stream *st = s->stream;

	for (uint64 i = 0; st != NULL && i < st->n; i++)
		put(s, st->key[i]);
	if (st != NULL)
	{
		pfree(st->key);
		pfree(st);
	}
	s->stream = NULL;
}

void
sift_f2_stream_finish(F2Scan *s)
{
	qsort(s->stream->key, s->stream->n, sizeof(uint64), key_cmp);
}

bool
sift_f2_stream_next(F2Scan *s, ScanDirection dir, uint64 *key)
{
	SiftF2Stream *st = s->stream;

	if (ScanDirectionIsNoMovement(dir))
		return false;
	if (!st->started)
	{
		st->pos = ScanDirectionIsForward(dir) ? 0 : (int64) st->n - 1;
		st->started = true;
	}
	if (st->pos < 0 || (uint64) st->pos >= st->n)
		return false;
	*key = st->key[ScanDirectionIsForward(dir) ? st->pos++ : st->pos--];
	return true;
}
