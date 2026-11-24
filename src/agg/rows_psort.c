/*
 * agg/rows_psort.c - the external sort of the pairs of a count(DISTINCT) of agg/rows.c (agg/rows_psort.h).
 * The runs the scan writes are kept in levels, a file to each: a run goes to the end of the file of the level 0, and the fan-in runs of a level that are there become one run at the end of
 * the file of the next level while the scan goes on, the file they were in ending. So a sort holds one file for each level (a handful, however many runs the scan makes) and the
 * readers of a merge, whose number and buffers are what the memory of the sort allows.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/memutils.h"

#include "agg/ltree.h"
#include "agg/rows_psort.h"
#include "core/host.h"

#define PS_FANIN		64		/* runs merged at a time, at most */
#define PS_FANIN_MIN	4
#define PS_READ_PAIRS	4096	/* 64 kB of a run a read, at most */
#define PS_READ_MIN		256		/* ... and at least: 4 kB */
#define PS_FIRST_CAP	1024
#define PS_SMALL		64		/* fewer pairs than this are put in order by insertion */
#define PS_LEVELS		16		/* fan-in of 4 or more: more runs than a scan can make */
#define PS_INTERRUPT	4096	/* pairs between two looks at the interrupts of a merge */

/* the runs of a level: in one file, one after the other; fewer than a fan-in of them, since the fan-in-th makes a run of the level above */
typedef struct PsLevel
{
	BufFile    *file;
	uint32		n;
	uint64		end;			/* bytes of the file */
	uint64		off[PS_FANIN];	/* of each run, the byte of the file it starts at */
	uint64		len[PS_FANIN];	/* its pairs */
} PsLevel;

typedef struct PsReader
{
	BufFile    *file;
	uint64		off;			/* the byte of the file the next read is at */
	uint64		left;			/* pairs of the run not read into buf */
	RowsPair   *buf;
	uint32		cap;			/* pairs buf holds */
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
	uint32		fanin;			/* runs merged at a time */
	uint32		read_pairs;		/* pairs of the read buffer of a run */
	bool		spilled;
	PsLevel		level[PS_LEVELS];
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
	/* the readers of a merge hold what the run being made leaves of the memory of the sort: the other half */
	s->read_pairs = (uint32) Min(Max((int64) (mem_bytes / 2 / sizeof(RowsPair) / (PS_FANIN + 1)), PS_READ_MIN), PS_READ_PAIRS);
	s->fanin = (uint32) Min(Max((int64) (mem_bytes / 2 / ((uint64) s->read_pairs * sizeof(RowsPair))) - 1, PS_FANIN_MIN), PS_FANIN);
	return s;
}

static void level_up(RowsPSort *s, int lv);

/* the end of the file of a level: where its next run starts */
static PsLevel *
level_file(RowsPSort *s, int lv)
{
	PsLevel    *l = &s->level[lv];

	SIFT_INVARIANT(lv < PS_LEVELS, "sift: the runs of a distinct sort of rows have more levels than the scan can make");
	if (l->file == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->cx);

		l->file = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	return l;
}

/* a run of m pairs written to the end of a level is complete: the level that holds a fan-in of runs makes one of the next */
static void
level_close_run(RowsPSort *s, int lv, uint64 m)
{
	PsLevel    *l = &s->level[lv];

	l->len[l->n++] = m;
	l->end += m * sizeof(RowsPair);
	if (l->n == s->fanin)
		level_up(s, lv);
}

/* a pair of buf is a run when buf is full: sorted, merged, and written to the first level */
static void
flush_run(RowsPSort *s)
{
	PsLevel    *l;
	uint64		m;

	if (s->scratch_cap < s->n)
	{
		if (s->scratch)
			pfree(s->scratch);
		s->scratch = MemoryContextAlloc(s->cx, s->held * sizeof(RowsPair));
		s->scratch_cap = s->held;
	}
	m = rows_psort_sort_pairs(s->buf, s->scratch, s->n);
	l = level_file(s, 0);
	l->off[l->n] = l->end;
	BufFileWrite(l->file, s->buf, m * sizeof(RowsPair));
	s->spilled = true;
	s->n = 0;
	level_close_run(s, 0, m);
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
	r->n = (uint32) Min(r->left, (uint64) r->cap);
	r->pos = 0;
	if (r->n > 0)
	{
		SIFT_INVARIANT(BufFileSeek(r->file, 0, (off_t) r->off, SEEK_SET) == 0, "sift: could not seek in a run of a distinct sort");
		BufFileReadExact(r->file, r->buf, (size_t) r->n * sizeof(RowsPair));
	}
	r->off += (uint64) r->n * sizeof(RowsPair);
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

/* A reader of a run: it reads read_pairs of it at a time from its start */
static void
reader_begin(RowsPSort *s, PsReader *r, BufFile *file, uint64 off, uint64 npairs)
{
	r->file = file;
	r->off = off;
	r->left = npairs;
	r->buf = MemoryContextAlloc(s->cx, s->read_pairs * sizeof(RowsPair));
	r->cap = s->read_pairs;
	reader_fill(r);
}

/* A merge of the runs of the levels from lv up to the top, or of the runs of the level lv alone, which it reads from their start; the files stay for the caller */
static PsMerge *
merge_begin(RowsPSort *s, int lv, bool alone)
{
	PsMerge    *m = MemoryContextAllocZero(s->cx, sizeof(PsMerge));
	int			n = 0;

	for (int i = lv; i < PS_LEVELS && (!alone || i == lv); i++)
		n += s->level[i].n;
	m->nin = n;
	m->rd = MemoryContextAllocZero(s->cx, Max(n, 1) * sizeof(PsReader));
	n = 0;
	for (int i = lv; i < PS_LEVELS && (!alone || i == lv); i++)
		for (uint32 k = 0; k < s->level[i].n; k++)
			reader_begin(s, &m->rd[n++], s->level[i].file, s->level[i].off[k], s->level[i].len[k]);
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
		pfree(m->rd[i].buf);
	sift_ltree_free(&m->lt);
	pfree(m->rd);
	pfree(m);
}

/* The runs of level lv become one run at the end of the file of the level above, and their file ends */
static void
level_up(RowsPSort *s, int lv)
{
	PsLevel    *from = &s->level[lv];
	PsLevel    *to = level_file(s, lv + 1);
	PsMerge    *m = merge_begin(s, lv, true);
	RowsPair	p;
	uint64		total = 0;

	to->off[to->n] = to->end;
	while (merge_next(m, &p))
	{
		BufFileWrite(to->file, &p, sizeof(p));
		if ((++total % PS_INTERRUPT) == 0)
			CHECK_FOR_INTERRUPTS();
	}
	merge_end(m);
	BufFileClose(from->file);
	*from = (PsLevel) {0};
	level_close_run(s, lv + 1, total);
}

static uint32
runs_of(const RowsPSort *s)
{
	uint32		n = 0;

	for (int i = 0; i < PS_LEVELS; i++)
		n += s->level[i].n;
	return n;
}

void
rows_psort_finish(RowsPSort *s)
{
	if (!s->spilled)
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
	while (runs_of(s) > s->fanin)
	{
		int			lv = 0;

		while (s->level[lv].n == 0)
			lv++;
		level_up(s, lv);
	}
	s->merge = merge_begin(s, 0, false);
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
	for (int i = 0; i < PS_LEVELS; i++)
		if (s->level[i].file)
			BufFileClose(s->level[i].file);
	if (s->buf)
		pfree(s->buf);
	if (s->scratch)
		pfree(s->scratch);
	pfree(s);
}
