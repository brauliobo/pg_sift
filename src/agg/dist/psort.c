/*
 * agg/dist/psort.c - count(DISTINCT): the sort of the pairs of a set that outgrew hash memory
 * The pairs (sort id of the group above the value, 8 bytes) fill an array of at most hash memory, which is sorted by radix and stripped of repeats when it is full. What the
 * array cannot keep goes out as a sorted run to a temporary file, and the runs are merged into one sorted file of distinct pairs when the scan ends.
 */
#include "postgres.h"

#include "storage/buffile.h"

#include "agg/dist/int.h"
#include "agg/ltree.h"
#include "core/cmp.h"

#define PS_START		1024		/* pairs of the first array */
#define PS_CHUNK		8192		/* pairs a run is read in */
#define PS_PROBE		(1 << 20)	/* pairs of the array at which its repeats are looked at before it grows */
/* MEM[MEM-AGG-10]: PairSort merge buffers (PS_FANIN=32 runs at a time * 64KB = 2MiB; was 64 runs, 4MiB) */
#define PS_FANIN		32			/* runs merged at a time: a read buffer each */

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
	uint64	   *runoff;			/* the byte of the file a run starts at (the runs a merge pass made follow the runs the array wrote) */
	uint64		fend;			/* bytes of the file */
	uint32		nruns;
	uint32		runcap;
	bool		unique;			/* the array lost less than a quarter of its pairs to repeats at PS_PROBE: it grows without looking */
	BufFile    *out;			/* the merged distinct pairs */
	uint64		nout;
	uint64		pos;			/* the pairs given so far */
	/* MEM[MEM-AGG-10]: PairSort rbuf (PS_CHUNK*8 = 64KB read buffer, made when the merged pairs are first read from the file; was inside every PairSort) */
	uint64	   *rbuf;
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
	/* MEM[MEM-AGG-10]: PairSort array and radix tmp (the array holds at most half the budget, so it and the tmp array of ps_radix fit it together; was the whole budget twice) */
	s->maxcap = Min(Max(budget / sizeof(uint64) / 2, PS_START), MaxAllocHugeSize / sizeof(uint64) / 2);
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
		SIFT_REGROW_IN(s->cxt, s->runlen, s->runcap * sizeof(uint64));
		SIFT_REGROW_IN(s->cxt, s->runoff, s->runcap * sizeof(uint64));
	}
	ps_strip(s);
	BufFileWrite(s->runs, s->buf, s->n * sizeof(uint64));
	s->runoff[s->nruns] = s->fend;
	s->runlen[s->nruns++] = s->n;
	s->fend += s->n * sizeof(uint64);
	s->n = 0;
}

/*
 * the array is full: it grows up to hash memory, then the repeats go, and what is still more than half of it goes out as a run. A set of many repeats
 * would grow to hash memory before it lost any, so the array is stripped at PS_PROBE pairs, and again each time it fills while the repeats were a quarter of it
 */
static void
ps_room(PairSort *s)
{
	if (s->cap >= PS_PROBE && s->cap < s->maxcap && s->nruns == 0 && !s->unique)
	{
		uint64		n = s->n;

		s->unique = ps_strip(s) > n - n / 4;
		if (s->n <= s->cap / 2)
			return;
	}
	if (s->cap < s->maxcap)
	{
		uint64		cap = Min(s->cap ? s->cap * 2 : PS_START, s->maxcap);

		SIFT_REGROW_HUGE_IN(s->cxt, s->buf, cap * sizeof(uint64));
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

static bool
ps_run_less(void *arg, int a, int b)
{
	PsRun	  **r = arg;

	if (r[a]->i >= r[a]->n)
		return false;
	if (r[b]->i >= r[b]->n)
		return true;
	return r[a]->buf[r[a]->i] < r[b]->buf[r[b]->i];
}

/* the distinct pairs a merge writes, PS_CHUNK at a time */
typedef struct PsSink
{
	BufFile    *to;
	bool		seek;			/* the file is also read from: each write goes to its place at off */
	uint64		off;
	uint64		n;
	uint32		nw;
	uint64		w[PS_CHUNK];
} PsSink;

static void
sink_flush(PsSink *k)
{
	if (k->nw == 0)
		return;
	if (k->seek)
		SIFT_INVARIANT(BufFileSeek(k->to, 0, (off_t) k->off, SEEK_SET) == 0, "sift: could not seek in the runs of a count(DISTINCT)");
	BufFileWrite(k->to, k->w, k->nw * sizeof(uint64));
	k->off += k->nw * sizeof(uint64);
	k->nw = 0;
}

static void
sink_put(PsSink *k, uint64 v)
{
	k->w[k->nw++] = v;
	k->n++;
	if (k->nw == PS_CHUNK)
		sink_flush(k);
}

/* the distinct pairs of the runs idx[0 .. k-1] in order, to sink */
static void
ps_merge_runs(PairSort *s, const uint32 *idx, uint32 k, PsSink *sink)
{
	PsRun	  **r = palloc(k * sizeof(PsRun *));
	SiftLTree	lt;
	uint64		prev = 0;
	bool		any = false;

	for (uint32 j = 0; j < k; j++)
	{
		r[j] = palloc(sizeof(PsRun));
		r[j]->off = s->runoff[idx[j]];
		r[j]->left = s->runlen[idx[j]];
		r[j]->i = r[j]->n = 0;
		(void) ps_run_fill(s, r[j]);
	}
	sift_ltree_init(&lt, (int) k, ps_run_less, r);
	for (;;)
	{
		PsRun	   *top = r[sift_ltree_head(&lt)];
		uint64		v;

		if (top->i >= top->n)
			break;
		v = top->buf[top->i];
		if (!any || v != prev)
			sink_put(sink, v);
		any = true;
		prev = v;
		if (++top->i == top->n)
			(void) ps_run_fill(s, top);
		sift_ltree_replay(&lt);
	}
	sink_flush(sink);
	sift_ltree_free(&lt);
	for (uint32 j = 0; j < k; j++)
		pfree(r[j]);
	pfree(r);
}

static int
ps_len_cmp(const void *a, const void *b, void *arg)
{
	const uint64 *len = arg;
	const uint64 x = len[*(const uint32 *) a],
				y = len[*(const uint32 *) b];

	return x < y ? -1 : x > y;
}

/*
 * The runs merged into the file of the distinct pairs. At most PS_FANIN runs are read at a time (a read buffer each): more are reduced first, the fewest smallest ones that leave
 * PS_FANIN of them becoming one run at the end of the file of the runs.
 */
static void
ps_merge(PairSort *s)
{
	uint32	   *idx = palloc(s->nruns * sizeof(uint32));
	uint32		live = s->nruns;
	PsSink	   *sink = palloc(sizeof(PsSink));
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	for (uint32 k = 0; k < live; k++)
		idx[k] = k;
	while (live > PS_FANIN)
	{
		uint32		m = live - PS_FANIN + 1;

		qsort_arg(idx, live, sizeof(uint32), ps_len_cmp, s->runlen);
		*sink = (PsSink) {s->runs, true, s->fend, 0, 0};
		ps_merge_runs(s, idx, m, sink);
		if (s->nruns == s->runcap)
		{
			s->runcap *= 2;
			SIFT_REGROW_IN(s->cxt, s->runlen, s->runcap * sizeof(uint64));
			SIFT_REGROW_IN(s->cxt, s->runoff, s->runcap * sizeof(uint64));
		}
		s->runoff[s->nruns] = s->fend;
		s->runlen[s->nruns] = sink->n;
		s->fend += sink->n * sizeof(uint64);
		memmove(idx, idx + m, (live - m) * sizeof(uint32));
		idx[live - m] = s->nruns++;
		live -= m - 1;
	}
	s->out = BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	*sink = (PsSink) {s->out, false, 0, 0, 0};
	ps_merge_runs(s, idx, live, sink);
	s->nout = sink->n;
	pfree(sink);
	pfree(idx);
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
			if (s->rbuf == NULL)
				s->rbuf = MemoryContextAlloc(s->cxt, sizeof(uint64) * PS_CHUNK);
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
	if (s->runoff)
		pfree(s->runoff);
	pfree(s);
}
