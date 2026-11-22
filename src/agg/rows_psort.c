/*
 * agg/rows_psort.c - the external sort of the pairs of a count(DISTINCT) of agg/rows.c (agg/rows_psort.h).
 */
#include "postgres.h"

#include "storage/buffile.h"
#include "utils/memutils.h"

#include "agg/ltree.h"
#include "agg/rows_psort.h"
#include "core/host.h"

#define PS_FANIN		64
#define PS_READ_PAIRS	4096	/* 64 kB of a run a read */
#define PS_FIRST_CAP	1024
#define PS_SMALL		64		/* fewer pairs than this are put in order by insertion */

typedef struct PsRun
{
	BufFile    *file;
	uint64		npairs;
} PsRun;

typedef struct PsReader
{
	BufFile    *file;
	uint64		left;			/* pairs of the file not read into buf */
	RowsPair   *buf;
	uint32		n;
	uint32		pos;
} PsReader;

typedef struct PsMerge
{
	int			nin;
	PsReader   *rd;
	SiftLTree	lt;
} PsMerge;

struct RowsPSort
{
	MemoryContext cx;
	RowsPair   *buf;
	RowsPair   *scratch;
	uint64		cap;			/* the pairs a run may hold */
	uint64		held;			/* the pairs of buf, and of scratch when it is the size of buf */
	uint64		n;
	uint64		scratch_cap;
	PsRun	   *run;
	int			nrun;
	int			run_cap;
	uint64		at;				/* in memory: the next pair of buf */
	PsMerge    *merge;
};

static inline bool
key_less(const RowsPair *a, const RowsPair *b)
{
	return a->g != b->g ? a->g < b->g : a->v < b->v;
}

static inline bool
key_equal(const RowsPair *a, const RowsPair *b)
{
	return a->g == b->g && a->v == b->v;
}

/* byte b (0 .. 11) of the sort key: the 8 of the value with its sign bit flipped, then the 4 of the group */
static inline uint32
key_byte(const RowsPair *p, int b)
{
	return b < 8 ? (uint32) (((uint64) p->v ^ (UINT64CONST(1) << 63)) >> (8 * b)) & 0xFF : (p->g >> (8 * (b - 8))) & 0xFF;
}

static void
insertion_sort(RowsPair *a, uint64 n)
{
	for (uint64 i = 1; i < n; i++)
	{
		const RowsPair x = a[i];
		uint64		j = i;

		for (; j > 0 && key_less(&x, &a[j - 1]); j--)
			a[j] = a[j - 1];
		a[j] = x;
	}
}

/* LSD radix sort of the 12 key bytes with all the histograms made in one pass; a byte every pair has the same value of is not a pass */
static void
radix_sort(RowsPair *a, RowsPair *scratch, uint64 n)
{
	uint32		hist[12][256] = {{0}};
	RowsPair   *src = a,
			   *dst = scratch;

	for (uint64 i = 0; i < n; i++)
		for (int b = 0; b < 12; b++)
			hist[b][key_byte(&a[i], b)]++;
	for (int b = 0; b < 12; b++)
	{
		uint32		next[256];
		uint32		sum = 0;

		if (hist[b][key_byte(&a[0], b)] == n)
			continue;
		for (int k = 0; k < 256; k++)
		{
			next[k] = sum;
			sum += hist[b][k];
		}
		for (uint64 i = 0; i < n; i++)
			dst[next[key_byte(&src[i], b)]++] = src[i];
		{
			RowsPair   *t = src;

			src = dst;
			dst = t;
		}
	}
	if (src != a)
		memcpy(a, src, n * sizeof(RowsPair));
}

uint64
rows_psort_sort_pairs(RowsPair *a, RowsPair *scratch, uint64 n)
{
	uint64		w = 0;

	if (n < PS_SMALL)
		insertion_sort(a, n);
	else
		radix_sort(a, scratch, n);
	for (uint64 i = 0; i < n; i++)
		if (w > 0 && key_equal(&a[w - 1], &a[i]))
			a[w - 1].mask |= a[i].mask;
		else
			a[w++] = a[i];
	return w;
}

RowsPSort *
rows_psort_begin(MemoryContext cx, uint64 mem_bytes)
{
	RowsPSort  *s = MemoryContextAllocZero(cx, sizeof(RowsPSort));

	s->cx = cx;
	s->cap = Min(Max(mem_bytes / 2 / sizeof(RowsPair), (uint64) PS_FIRST_CAP), MaxAllocSize / sizeof(RowsPair));
	return s;
}

/* a pair of buf is a run when buf is full: sorted, merged, and written */
static void
flush_run(RowsPSort *s)
{
	BufFile    *f;
	uint64		m;

	if (s->scratch_cap < s->n)
	{
		if (s->scratch)
			pfree(s->scratch);
		s->scratch = MemoryContextAlloc(s->cx, s->held * sizeof(RowsPair));
		s->scratch_cap = s->held;
	}
	m = rows_psort_sort_pairs(s->buf, s->scratch, s->n);
	f = BufFileCreateTemp(false);
	BufFileWrite(f, s->buf, m * sizeof(RowsPair));
	if (s->nrun == s->run_cap)
	{
		s->run_cap = Max(s->run_cap * 2, 8);
		s->run = s->run ? repalloc(s->run, s->run_cap * sizeof(PsRun)) : MemoryContextAlloc(s->cx, s->run_cap * sizeof(PsRun));
	}
	s->run[s->nrun++] = (PsRun) {f, m};
	s->n = 0;
}

void
rows_psort_put(RowsPSort *s, uint32 g, int64 v, uint32 mask)
{
	if (s->n == s->held)
	{
		if (s->held == s->cap)
			flush_run(s);
		else
		{
			const uint64 held = Min(Max(s->held * 2, (uint64) PS_FIRST_CAP), s->cap);

			s->buf = s->buf ? repalloc(s->buf, held * sizeof(RowsPair)) : MemoryContextAlloc(s->cx, held * sizeof(RowsPair));
			s->held = held;
		}
	}
	s->buf[s->n++] = (RowsPair) {v, g, mask};
}

static inline bool
reader_done(const PsReader *r)
{
	return r->pos >= r->n;
}

static void
reader_fill(PsReader *r)
{
	r->n = (uint32) Min(r->left, (uint64) PS_READ_PAIRS);
	r->pos = 0;
	if (r->n > 0)
		BufFileReadExact(r->file, r->buf, (size_t) r->n * sizeof(RowsPair));
	r->left -= r->n;
}

static bool
merge_less(void *arg, int a, int b)
{
	const PsMerge *m = arg;
	const PsReader *ra = &m->rd[a],
			   *rb = &m->rd[b];

	if (reader_done(ra))
		return false;
	if (reader_done(rb))
		return true;
	if (key_equal(&ra->buf[ra->pos], &rb->buf[rb->pos]))
		return a < b;
	return key_less(&ra->buf[ra->pos], &rb->buf[rb->pos]);
}

/* A merge of the runs run[0 .. n-1], which it reads from their start; the files are closed when the merge is */
static PsMerge *
merge_begin(RowsPSort *s, const PsRun *run, int n)
{
	PsMerge    *m = MemoryContextAllocZero(s->cx, sizeof(PsMerge));

	m->nin = n;
	m->rd = MemoryContextAllocZero(s->cx, n * sizeof(PsReader));
	for (int i = 0; i < n; i++)
	{
		PsReader   *r = &m->rd[i];

		r->file = run[i].file;
		r->left = run[i].npairs;
		r->buf = MemoryContextAlloc(s->cx, PS_READ_PAIRS * sizeof(RowsPair));
		SIFT_INVARIANT(BufFileSeek(r->file, 0, 0, SEEK_SET) == 0, "sift: could not rewind a run of a distinct sort");
		reader_fill(r);
	}
	sift_ltree_init(&m->lt, n, merge_less, m);
	return m;
}

static bool
merge_next(PsMerge *m, RowsPair *out)
{
	if (reader_done(&m->rd[sift_ltree_head(&m->lt)]))
		return false;
	for (bool first = true;; first = false)
	{
		PsReader   *r = &m->rd[sift_ltree_head(&m->lt)];

		if (reader_done(r) || (!first && !key_equal(&r->buf[r->pos], out)))
			return true;
		if (first)
			*out = r->buf[r->pos];
		else
			out->mask |= r->buf[r->pos].mask;
		if (++r->pos == r->n)
			reader_fill(r);
		sift_ltree_replay(&m->lt);
	}
}

static void
merge_end(PsMerge *m)
{
	for (int i = 0; i < m->nin; i++)
	{
		BufFileClose(m->rd[i].file);
		pfree(m->rd[i].buf);
	}
	sift_ltree_free(&m->lt);
	pfree(m->rd);
	pfree(m);
}

static int
run_cmp(const void *a, const void *b)
{
	const uint64 x = ((const PsRun *) a)->npairs,
				y = ((const PsRun *) b)->npairs;

	return x < y ? -1 : x > y;
}

/* more than PS_FANIN runs: the smallest PS_FANIN of them become one, until the final merge can take them all */
static void
reduce_runs(RowsPSort *s)
{
	while (s->nrun > PS_FANIN)
	{
		PsMerge    *m;
		BufFile    *f = BufFileCreateTemp(false);
		RowsPair	p;
		uint64		total = 0;

		qsort(s->run, s->nrun, sizeof(PsRun), run_cmp);
		m = merge_begin(s, s->run, PS_FANIN);
		while (merge_next(m, &p))
		{
			BufFileWrite(f, &p, sizeof(p));
			total++;
		}
		merge_end(m);
		memmove(s->run, s->run + PS_FANIN, (s->nrun - PS_FANIN) * sizeof(PsRun));
		s->nrun -= PS_FANIN;
		s->run[s->nrun++] = (PsRun) {f, total};
	}
}

void
rows_psort_finish(RowsPSort *s)
{
	if (s->nrun == 0)
	{
		if (s->n > 0)
		{
			RowsPair   *scratch = s->n >= PS_SMALL ? MemoryContextAlloc(s->cx, s->n * sizeof(RowsPair)) : NULL;

			s->n = rows_psort_sort_pairs(s->buf, scratch, s->n);
			if (scratch)
				pfree(scratch);
		}
		s->at = 0;
		return;
	}
	if (s->n > 0)
		flush_run(s);
	if (s->scratch)
		pfree(s->scratch);
	s->scratch = NULL;
	pfree(s->buf);
	s->buf = NULL;
	reduce_runs(s);
	s->merge = merge_begin(s, s->run, s->nrun);
	s->nrun = 0;
}

bool
rows_psort_next(RowsPSort *s, RowsPair *out)
{
	if (s->merge)
		return merge_next(s->merge, out);
	if (s->at >= s->n)
		return false;
	*out = s->buf[s->at++];
	return true;
}

void
rows_psort_end(RowsPSort *s)
{
	if (s->merge)
		merge_end(s->merge);
	for (int i = 0; i < s->nrun; i++)
		BufFileClose(s->run[i].file);
	if (s->buf)
		pfree(s->buf);
	if (s->scratch)
		pfree(s->scratch);
	if (s->run)
		pfree(s->run);
	pfree(s);
}
