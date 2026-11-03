/*
 * agg/dist/psort.c - count(DISTINCT): the sort of the pairs of a set that outgrew hash memory
 * The pairs (sort id of the group above the value, 8 bytes) fill an array of at most hash memory, which is sorted by radix and stripped of repeats when it is full. What the
 * array cannot keep goes out as a sorted run to a temporary file, and the runs are merged into one sorted file of distinct pairs when the scan ends.
 */
#include "postgres.h"

#include "lib/binaryheap.h"
#include "storage/buffile.h"

#include "agg/dist/int.h"
#include "core/cmp.h"

#define PS_START		1024		/* pairs of the first array */
#define PS_CHUNK		8192		/* pairs a run is read in */

typedef struct PsRun
{
	uint64		off;			/* bytes of the file the next chunk is at */
	uint64		left;			/* pairs not read yet */
	uint32		i;
	uint32		n;
	uint64		buf[PS_CHUNK];
} PsRun;

struct PairSort
{
	MemoryContext cxt;
	uint64	   *buf;
	uint64		n;
	uint64		cap;
	uint64		maxcap;
	BufFile    *runs;
	uint64	   *runlen;
	uint32		nruns;
	uint32		runcap;
	BufFile    *out;			/* the merged distinct pairs */
	uint64		nout;
	uint64		pos;			/* the pairs given so far */
	uint64		rbuf[PS_CHUNK];
	uint32		ri;
	uint32		rn;
};

#define PS_KEY(x)		((int64_t) *(x))
#define PS_ZERO(x)		0
static void
ps_cmp_sort(uint64_t *a, size_t n)
{
	sift_sort_u64(a, n);
}

static uint64_t *
ps_alloc(size_t bytes)
{
	return palloc_extended(bytes, MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);
}

SIFT_DEFINE_RADIX_SORT(ps_radix, uint64_t, PS_KEY, PS_ZERO, PS_ZERO, ps_alloc, pfree, ps_cmp_sort)

PairSort *
psort_begin(MemoryContext cxt, uint64 budget)
{
	PairSort   *s = MemoryContextAllocZero(cxt, sizeof(PairSort));

	s->cxt = cxt;
	s->maxcap = Min(Max(budget / sizeof(uint64), PS_START), MaxAllocHugeSize / sizeof(uint64) / 2);
	return s;
}

/* the array sorted, each pair once; the count left */
static uint64
ps_strip(PairSort *s)
{
	uint64		u = 0;

	ps_radix(s->buf, s->n);
	for (uint64 i = 0; i < s->n; i++)
		if (u == 0 || s->buf[u - 1] != s->buf[i])
			s->buf[u++] = s->buf[i];
	s->n = u;
	return u;
}

static void
ps_write_run(PairSort *s)
{
	if (s->runs == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->cxt);

		s->runs = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	if (s->nruns == s->runcap)
	{
		s->runcap = s->runcap ? s->runcap * 2 : 8;
		s->runlen = s->runlen ? repalloc(s->runlen, s->runcap * sizeof(uint64)) : MemoryContextAlloc(s->cxt, s->runcap * sizeof(uint64));
	}
	ps_strip(s);
	BufFileWrite(s->runs, s->buf, s->n * sizeof(uint64));
	s->runlen[s->nruns++] = s->n;
	s->n = 0;
}

/* the array is full: it grows up to hash memory, then the repeats go, and what is still more than half of it goes out as a run */
static void
ps_room(PairSort *s)
{
	if (s->cap < s->maxcap)
	{
		uint64		cap = Min(s->cap ? s->cap * 2 : PS_START, s->maxcap);

		s->buf = s->buf ? repalloc_huge(s->buf, cap * sizeof(uint64)) : MemoryContextAllocHuge(s->cxt, cap * sizeof(uint64));
		s->cap = cap;
		return;
	}
	if (s->nruns == 0 && ps_strip(s) <= s->cap / 2)
		return;
	ps_write_run(s);
}

void
psort_put(PairSort *s, uint64 v)
{
	if (s->n == s->cap)
		ps_room(s);
	s->buf[s->n++] = v;
}

static int
ps_run_cmp(Datum a, Datum b, void *arg)
{
	PsRun	  **r = arg;
	uint64		x = r[DatumGetInt32(a)]->buf[r[DatumGetInt32(a)]->i],
				y = r[DatumGetInt32(b)]->buf[r[DatumGetInt32(b)]->i];

	return (x < y) - (x > y);
}

static bool
ps_run_fill(PairSort *s, PsRun *r)
{
	if (r->left == 0)
		return false;
	r->n = (uint32) Min(r->left, PS_CHUNK);
	SIFT_INVARIANT(BufFileSeek(s->runs, 0, (off_t) r->off, SEEK_SET) == 0, "sift: could not seek in the runs of a count(DISTINCT)");
	BufFileReadExact(s->runs, r->buf, r->n * sizeof(uint64));
	r->off += r->n * sizeof(uint64);
	r->left -= r->n;
	r->i = 0;
	return true;
}

/* the runs merged into the file of the distinct pairs */
static void
ps_merge(PairSort *s)
{
	PsRun	  **r = palloc(s->nruns * sizeof(PsRun *));
	binaryheap *h = binaryheap_allocate(s->nruns, ps_run_cmp, r);
	uint64	   *w = palloc(PS_CHUNK * sizeof(uint64));
	uint32		nw = 0;
	uint64		off = 0,
				prev = 0;
	bool		any = false;
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	s->out = BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	for (uint32 k = 0; k < s->nruns; k++)
	{
		r[k] = palloc(sizeof(PsRun));
		r[k]->off = off;
		r[k]->left = s->runlen[k];
		off += s->runlen[k] * sizeof(uint64);
		if (ps_run_fill(s, r[k]))
			binaryheap_add_unordered(h, Int32GetDatum(k));
	}
	binaryheap_build(h);
	while (!binaryheap_empty(h))
	{
		PsRun	   *top = r[DatumGetInt32(binaryheap_first(h))];
		uint64		v = top->buf[top->i];

		if (!any || v != prev)
		{
			w[nw++] = v;
			s->nout++;
			if (nw == PS_CHUNK)
			{
				BufFileWrite(s->out, w, nw * sizeof(uint64));
				nw = 0;
			}
		}
		any = true;
		prev = v;
		if (++top->i < top->n || ps_run_fill(s, top))
			binaryheap_replace_first(h, binaryheap_first(h));
		else
			binaryheap_remove_first(h);
	}
	if (nw)
		BufFileWrite(s->out, w, nw * sizeof(uint64));
	for (uint32 k = 0; k < s->nruns; k++)
		pfree(r[k]);
	pfree(r);
	pfree(w);
	binaryheap_free(h);
}

/* all the pairs are in: sorted, each once */
void
psort_finish(PairSort *s)
{
	if (s->nruns == 0)
	{
		ps_strip(s);
		s->nout = s->n;
		return;
	}
	if (s->n > 0)
		ps_write_run(s);
	pfree(s->buf);
	s->buf = NULL;
	s->cap = 0;
	ps_merge(s);
}

void
psort_rewind(PairSort *s)
{
	s->pos = 0;
	s->ri = s->rn = 0;
	SIFT_INVARIANT(s->out == NULL || BufFileSeek(s->out, 0, 0, SEEK_SET) == 0, "sift: could not rewind the pairs of a count(DISTINCT)");
}

bool
psort_next(PairSort *s, uint64 *v)
{
	if (s->pos == s->nout)
		return false;
	if (s->out == NULL)
		*v = s->buf[s->pos++];
	else
	{
		if (s->ri == s->rn)
		{
			s->rn = (uint32) Min(s->nout - s->pos, PS_CHUNK);
			s->ri = 0;
			BufFileReadExact(s->out, s->rbuf, s->rn * sizeof(uint64));
		}
		*v = s->rbuf[s->ri++];
		s->pos++;
	}
	return true;
}

void
psort_end(PairSort *s)
{
	if (s->buf)
		pfree(s->buf);
	if (s->runs)
		BufFileClose(s->runs);
	if (s->out)
		BufFileClose(s->out);
	if (s->runlen)
		pfree(s->runlen);
	pfree(s);
}
