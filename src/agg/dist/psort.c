/*
 * agg/dist/psort.c - count(DISTINCT): the sort of the pairs of a set that outgrew hash memory
 * The pairs (sort id of the group above the value, 8 bytes) fill an array of at most hash memory, which is sorted by radix and stripped of repeats when it is full. What the
 * array cannot keep goes out as a sorted run to a temporary file, and the runs are merged into one sorted file of distinct pairs when the scan ends: a pass reads the runs of
 * one file, a fan-in of them at a time, into the next file and ends the one it read, so the runs a pass has consumed are not kept; the fan-in and the read buffer of a run
 * are what hash memory holds.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/buffile.h"

#include "agg/dist/int.h"
#include "agg/ltree.h"
#include "core/cmp.h"

#define PS_START		1024		/* pairs of the first array */
#define PS_CHUNK		8192		/* pairs a run is read in, at most */
#define PS_CHUNK_MIN	512			/* ... and at least: a page of the file is 8 kB */
#define PS_PROBE		(1 << 20)	/* pairs of the array at which its repeats are looked at before it grows */
#define PS_FANIN		32			/* runs merged at a time, at most: a read buffer each */
#define PS_FANIN_MIN	4
#define PS_INTERRUPT	4096		/* pairs between two looks at the interrupts of a merge */

typedef struct PsRun
{
	uint64		off;			/* bytes of the file the next chunk is at */
	uint64		left;			/* pairs not read yet */
	uint32		i;
	uint32		n;
	uint64	   *buf;
} PsRun;

struct PairSort
{
	MemoryContext cxt;
	uint64	   *buf;
	uint64		n;
	uint64		cap;
	uint64		maxcap;
	BufFile    *runs;			/* the runs of one generation: those the array wrote, then those of the pass that read the generation before */
	uint64	   *runlen;
	uint64	   *runoff;			/* the byte of the file a run starts at */
	uint64		fend;			/* bytes of the file */
	uint32		nruns;
	uint32		runcap;
	uint64		budget;			/* hash memory: what a merge may hold in read buffers */
	uint32		chunk;			/* pairs of the read buffer of a run (and of the buffer of what a merge writes) */
	uint32		fanin;			/* runs merged at a time */
	bool		unique;			/* the array lost less than a quarter of its pairs to repeats at PS_PROBE: it grows without looking */
	BufFile    *out;			/* the merged distinct pairs */
	uint64		nout;
	uint64		pos;			/* the pairs given so far */
	uint64	   *rbuf;			/* the read buffer of the merged pairs, made when they are first read from the file */
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
	/* the array holds at most half the budget, so it and the tmp array of ps_radix fit it together */
	s->maxcap = Min(Max(budget / sizeof(uint64) / 2, PS_START), MaxAllocHugeSize / sizeof(uint64) / 2);
	s->budget = budget;
	s->chunk = (uint32) Min(Max((int64) (budget / sizeof(uint64) / (PS_FANIN + 1)), PS_CHUNK_MIN), PS_CHUNK);
	s->fanin = (uint32) Min(Max((int64) (budget / ((uint64) s->chunk * sizeof(uint64))) - 1, PS_FANIN_MIN), PS_FANIN);
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
	r->n = (uint32) Min(r->left, s->chunk);
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

/* the distinct pairs a merge writes to the end of a file, a chunk at a time */
typedef struct PsSink
{
	BufFile    *to;
	uint64		n;				/* the pairs of the run being written */
	uint32		nw;
	uint32		cap;
	uint64	   *w;
} PsSink;

static void
sink_begin(PsSink *k, BufFile *to, uint32 cap)
{
	*k = (PsSink) {.to = to,.cap = cap,.w = palloc(cap * sizeof(uint64))};
}

static void
sink_flush(PsSink *k)
{
	if (k->nw == 0)
		return;
	BufFileWrite(k->to, k->w, k->nw * sizeof(uint64));
	k->nw = 0;
}

static void
sink_put(PsSink *k, uint64 v)
{
	k->w[k->nw++] = v;
	k->n++;
	if (k->nw == k->cap)
		sink_flush(k);
}

/* the distinct pairs of the runs first .. first+k-1 in order, to sink; the pairs the sink had before are not part of its run */
static void
ps_merge_runs(PairSort *s, uint32 first, uint32 k, PsSink *sink)
{
	PsRun	  **r = palloc(k * sizeof(PsRun *));
	SiftLTree	lt;
	uint64		prev = 0,
				seen = 0;
	bool		any = false;

	sink->n = 0;
	for (uint32 j = 0; j < k; j++)
	{
		r[j] = palloc(sizeof(PsRun));
		r[j]->buf = palloc(s->chunk * sizeof(uint64));
		r[j]->off = s->runoff[first + j];
		r[j]->left = s->runlen[first + j];
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
		if ((++seen % PS_INTERRUPT) == 0)
			CHECK_FOR_INTERRUPTS();
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
	{
		pfree(r[j]->buf);
		pfree(r[j]);
	}
	pfree(r);
}

static void
ps_set_runs(PairSort *s, uint32 n, uint64 **len, uint64 **off)
{
	*len = MemoryContextAlloc(s->cxt, n * sizeof(uint64));
	*off = MemoryContextAlloc(s->cxt, n * sizeof(uint64));
}

/*
 * One pass: the runs of the file are merged a fan-in of them at a time into runs of a new file, the file they were read from ends, and the new file is what the next pass reads. The groups
 * are as even as they can be (a lone run left over would be copied for nothing); every run of the new file has at least two of the old.
 */
static void
ps_pass(PairSort *s)
{
	const uint32 groups = (s->nruns + s->fanin - 1) / s->fanin;
	BufFile    *to;
	uint64	   *len,
			   *off;
	uint32		first = 0;
	PsSink		sink;
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	to = BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	ps_set_runs(s, groups, &len, &off);
	sink_begin(&sink, to, s->chunk);
	for (uint32 g = 0; g < groups; g++)
	{
		const uint32 k = s->nruns / groups + (g < s->nruns % groups);

		off[g] = g == 0 ? 0 : off[g - 1] + len[g - 1] * sizeof(uint64);
		ps_merge_runs(s, first, k, &sink);
		len[g] = sink.n;
		first += k;
	}
	pfree(sink.w);
	BufFileClose(s->runs);
	pfree(s->runlen);
	pfree(s->runoff);
	s->runs = to;
	s->runlen = len;
	s->runoff = off;
	s->nruns = s->runcap = groups;
	s->fend = off[groups - 1] + len[groups - 1] * sizeof(uint64);
}

/*
 * The runs merged into the file of the distinct pairs: passes while there are more runs than a fan-in, then the last merge into a file of its own, which stays to be read as often as
 * the pairs are asked for; the file of the runs ends with it.
 */
static void
ps_merge(PairSort *s)
{
	PsSink		sink;
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	while (s->nruns > s->fanin)
		ps_pass(s);
	s->out = BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	sink_begin(&sink, s->out, s->chunk);
	ps_merge_runs(s, 0, s->nruns, &sink);
	s->nout = sink.n;
	pfree(sink.w);
	BufFileClose(s->runs);
	s->runs = NULL;
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
	if (s->rbuf)
		pfree(s->rbuf);
	if (s->runlen)
		pfree(s->runlen);
	if (s->runoff)
		pfree(s->runoff);
	pfree(s);
}
