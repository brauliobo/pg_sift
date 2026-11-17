/*
 * core/sortruns.c - sorted runs of fixed-size records on temp files (core/sortruns.h)
 */
#include "postgres.h"

#include "core/sortruns.h"

typedef struct RunWriter
{
	SiftSortRunSet *set;
	BufFile    *file;
	char	   *blk;
	uint32		used;
	uint32		capfence;
	SiftSortRun		run;
} RunWriter;

static inline uint64
rec_key(const void *rec)
{
	uint64		k;

	memcpy(&k, rec, sizeof(k));
	return k;
}

static void
writer_begin(RunWriter *w, SiftSortRunSet *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	*w = (RunWriter) {.set = s,.file = BufFileCreateTemp(true),.blk = palloc(BLCKSZ),.capfence = 16};
	w->run.fence = palloc(w->capfence * sizeof(uint64));
	MemoryContextSwitchTo(old);
}

static void
writer_flush(RunWriter *w)
{
	memset(w->blk + w->used, 0, BLCKSZ - w->used);
	BufFileWrite(w->file, w->blk, BLCKSZ);
	w->used = 0;
}

static void
writer_put(RunWriter *w, const void *rec)
{
	if (w->used == 0)
	{
		if (w->run.nblocks == w->capfence)
		{
			w->capfence *= 2;
			w->run.fence = repalloc_huge(w->run.fence, w->capfence * sizeof(uint64));
		}
		w->run.fence[w->run.nblocks++] = rec_key(rec);
	}
	memcpy(w->blk + w->used, rec, w->set->recsz);
	w->used += w->set->recsz;
	w->run.n++;
	if (w->used == BLCKSZ)
		writer_flush(w);
}

static SiftSortRun
writer_end(RunWriter *w)
{
	SiftSortRun		r;

	if (w->used > 0)
		writer_flush(w);
	pfree(w->blk);
	r = w->run;
	r.file = w->file;
	return r;
}

static void
run_close(SiftSortRun *r)
{
	BufFileClose(r->file);
	pfree(r->fence);
}

void
sift_srun_init(SiftSortRunSet *s, MemoryContext cxt, uint32 recsz)
{
	Assert(BLCKSZ % recsz == 0 && recsz >= sizeof(uint64));
	*s = (SiftSortRunSet) {.cxt = cxt,.recsz = recsz};
}

void
sift_srun_cur_init(SiftSortRunCur *c, const SiftSortRun *run, uint32 recsz, MemoryContext cxt)
{
	*c = (SiftSortRunCur) {.run = run,.recsz = recsz,.blkno = -1,.blk = MemoryContextAlloc(cxt, BLCKSZ)};
}

const void *
sift_srun_cur_peek(SiftSortRunCur *c)
{
	uint64		per = BLCKSZ / c->recsz;
	int64		b = (int64) (c->pos / per);

	if (c->pos >= c->run->n)
		return NULL;
	if (b != c->blkno)
	{
		BufFileSeekBlock(c->run->file, b);
		BufFileReadExact(c->run->file, c->blk, BLCKSZ);
		c->blkno = b;
	}
	return c->blk + (c->pos % per) * c->recsz;
}

void
sift_srun_cur_next(SiftSortRunCur *c)
{
	c->pos++;
}

/* The two newest runs as one: a stable merge, the older one first for equal keys */
static void
merge_last(SiftSortRunSet *s)
{
	SiftSortRun    *a = &s->runs[s->nruns - 2],
			   *b = &s->runs[s->nruns - 1];
	SiftSortRunCur	ca,
				cb;
	RunWriter	w;
	const void *ra,
			   *rb;

	writer_begin(&w, s);
	sift_srun_cur_init(&ca, a, s->recsz, s->cxt);
	sift_srun_cur_init(&cb, b, s->recsz, s->cxt);
	for (ra = sift_srun_cur_peek(&ca), rb = sift_srun_cur_peek(&cb); ra != NULL || rb != NULL;)
	{
		if (rb == NULL || (ra != NULL && rec_key(ra) <= rec_key(rb)))
		{
			writer_put(&w, ra);
			sift_srun_cur_next(&ca);
			ra = sift_srun_cur_peek(&ca);
		}
		else
		{
			writer_put(&w, rb);
			sift_srun_cur_next(&cb);
			rb = sift_srun_cur_peek(&cb);
		}
		CHECK_FOR_INTERRUPTS();
	}
	pfree(ca.blk);
	pfree(cb.blk);
	run_close(a);
	run_close(b);
	*a = writer_end(&w);
	s->nruns--;
}

void
sift_srun_add(SiftSortRunSet *s, const void *recs, uint64 n)
{
	RunWriter	w;

	if (n == 0)
		return;
	writer_begin(&w, s);
	for (uint64 i = 0; i < n; i++)
		writer_put(&w, (const char *) recs + i * s->recsz);
	if (s->nruns == s->capruns)
	{
		s->capruns = Max(8, s->capruns * 2);
		SIFT_REGROW_HUGE_IN(s->cxt, s->runs, s->capruns * sizeof(SiftSortRun));
	}
	s->runs[s->nruns++] = writer_end(&w);
	while (s->nruns >= 2 && s->runs[s->nruns - 2].n <= 2 * s->runs[s->nruns - 1].n)
		merge_last(s);
}

void
sift_srun_reset(SiftSortRunSet *s)
{
	for (uint32 i = 0; i < s->nruns; i++)
		run_close(&s->runs[i]);
	s->nruns = 0;
}

/* The first block that can hold a key >= lo: the last whose first key is below it */
static uint32
run_start_block(const SiftSortRun *r, uint64 lo)
{
	uint32		l = 0,
				h = r->nblocks;

	while (l < h)
	{
		uint32		mid = l + (h - l) / 2;

		if (r->fence[mid] < lo)
			l = mid + 1;
		else
			h = mid;
	}
	return l > 0 ? l - 1 : 0;
}

void
sift_srun_range(const SiftSortRunSet *s, uint64 lo, uint64 hi, void **out, uint32 *n, uint32 *cap)
{
	uint32		per = BLCKSZ / s->recsz;

	for (uint32 i = 0; i < s->nruns; i++)
	{
		const SiftSortRun *r = &s->runs[i];
		SiftSortRunCur	c;
		const void *rec;

		if (r->n == 0 || r->fence[0] > hi)
			continue;
		sift_srun_cur_init(&c, r, s->recsz, s->cxt);
		c.pos = (uint64) run_start_block(r, lo) * per;
		for (rec = sift_srun_cur_peek(&c); rec != NULL && rec_key(rec) <= hi; rec = sift_srun_cur_peek(&c))
		{
			if (rec_key(rec) >= lo)
			{
				if (*n == *cap)
				{
					*cap = Max(16, *cap * 2);
					*out = *out ? repalloc_huge(*out, (Size) *cap * s->recsz) : MemoryContextAlloc(s->cxt, (Size) *cap * s->recsz);
				}
				memcpy((char *) *out + (Size) *n * s->recsz, rec, s->recsz);
				(*n)++;
			}
			sift_srun_cur_next(&c);
		}
		pfree(c.blk);
	}
}
