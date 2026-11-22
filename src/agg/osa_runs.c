/*
 * agg/osa_runs.c - the values of an ordered set that outgrew work_mem: sorted runs of (group, value, count) pairs, merged when the scan has ended
 * The histogram or array of pairs (agg/osa.c) is the first run's buffer; when it is full it is sorted, equal pairs are added up and it goes to a temporary
 * file of its own. At the end the runs are merged in the order the readers walk (group, then value) by a loser tree, a pair costing log2 of the runs compares.
 * A tuplesort of the same pairs takes about ten times the instructions for each value (a heap tuple formed, compared through the generic comparator and read
 * back for every value). A merge takes at most OSA_FANIN runs, so that it holds a fixed number of read buffers; more than OSA_MAX_RUNS runs while the scan goes
 * on, and the smallest OSA_FANIN of them become one (a pair is then written about log64 of the runs times, where merging all the runs each time rewrote the
 * biggest one every time); more than OSA_FANIN at the end, and the fewest smallest that leave OSA_FANIN do.
 */
#include "postgres.h"

#include "storage/buffile.h"

#include "agg/agg_int.h"
#include "agg/ltree.h"

#define OSA_FANIN	64			/* runs merged at a time */
#define OSA_MAX_RUNS	256		/* runs kept while the scan goes on */
#define OSA_RB		512			/* entries one run is read in */

#define ST_SORT osa_sort_slots
#define ST_ELEMENT_TYPE OsaSlot
#define ST_COMPARE(a, b) ((a)->sid1 != (b)->sid1 ? ((a)->sid1 < (b)->sid1 ? -1 : 1) : ((a)->key < (b)->key ? -1 : ((a)->key > (b)->key ? 1 : 0)))
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

typedef struct OsaFile
{
	BufFile    *file;
	uint64		n;				/* entries */
} OsaFile;

typedef struct OsaSrc
{
	BufFile    *file;
	uint64		left;			/* entries of the run not read yet */
	OsaSlot    *buf;
	uint32		nbuf;
	uint32		at;
} OsaSrc;

typedef struct OsaMerge
{
	int			k;
	OsaSrc	   *src;
	SiftLTree	lt;
} OsaMerge;

typedef struct OsaRuns
{
	MemoryContext cxt;
	OsaSlot    *buf;			/* the run being made */
	uint64		n;
	uint64		cap;
	uint64		room;			/* the most entries a buffer may hold: work_mem */
	OsaFile    *run;
	int			nrun;
	int			run_cap;
	OsaMerge   *merge;
	uint64		cur;			/* the walk of a single run kept in memory */
	bool		inmem;
} OsaRuns;

static inline bool
slot_less(const OsaSlot *a, const OsaSlot *b)
{
	return a->sid1 != b->sid1 ? a->sid1 < b->sid1 : a->key < b->key;
}

/* the dense array of pairs of the histogram becomes the buffer of the first run */
void
osa_runs_begin(AggExec *ex, Osa *o)
{
	OsaRuns    *rs = MemoryContextAllocZero(ex->tcxt, sizeof(OsaRuns));

	rs->cxt = ex->tcxt;
	rs->buf = o->slot;
	rs->n = o->n;
	rs->cap = o->cap;
	rs->room = Max((uint64) work_mem * 1024 / sizeof(OsaSlot), o->cap);
	o->runs = rs;
	o->slot = NULL;
	o->flat = false;
}

static inline bool
src_done(const OsaSrc *s)
{
	return s->at >= s->nbuf;
}

static void
src_fill(OsaSrc *s)
{
	s->nbuf = (uint32) Min(s->left, (uint64) OSA_RB);
	s->at = 0;
	if (s->nbuf > 0)
		BufFileReadExact(s->file, s->buf, s->nbuf * sizeof(OsaSlot));
	s->left -= s->nbuf;
}

static bool
merge_less(void *arg, int a, int b)
{
	const OsaMerge *m = arg;
	const OsaSrc *sa = &m->src[a],
			   *sb = &m->src[b];

	if (src_done(sa))
		return false;
	if (src_done(sb))
		return true;
	return slot_less(&sa->buf[sa->at], &sb->buf[sb->at]);
}

/* A merge of the runs run[0 .. k-1], read from their start; the files are closed when the merge ends */
static OsaMerge *
merge_begin(OsaRuns *rs, const OsaFile *run, int k)
{
	OsaMerge   *m = MemoryContextAllocZero(rs->cxt, sizeof(OsaMerge));

	m->k = k;
	m->src = MemoryContextAllocZero(rs->cxt, k * sizeof(OsaSrc));
	for (int i = 0; i < k; i++)
	{
		OsaSrc	   *s = &m->src[i];

		s->file = run[i].file;
		s->left = run[i].n;
		s->buf = MemoryContextAlloc(rs->cxt, OSA_RB * sizeof(OsaSlot));
		if (BufFileSeek(s->file, 0, 0, SEEK_SET) != 0)
			elog(ERROR, "could not rewind a run of an ordered set");
		src_fill(s);
	}
	sift_ltree_init(&m->lt, k, merge_less, m);
	return m;
}

/* the next pair of the merged runs, equal pairs of several runs added up while their count fits */
static bool
merge_next(OsaMerge *m, OsaSlot *out)
{
	for (bool first = true;; first = false)
	{
		OsaSrc	   *s = &m->src[sift_ltree_head(&m->lt)];

		if (src_done(s))
			return !first;
		if (first)
			*out = s->buf[s->at];
		else if (s->buf[s->at].sid1 != out->sid1 || s->buf[s->at].key != out->key || (uint64) out->cnt + s->buf[s->at].cnt > UINT32_MAX)
			return true;
		else
			out->cnt += s->buf[s->at].cnt;
		if (++s->at == s->nbuf)
			src_fill(s);
		sift_ltree_replay(&m->lt);
	}
}

static void
merge_end(OsaMerge *m)
{
	for (int i = 0; i < m->k; i++)
	{
		BufFileClose(m->src[i].file);
		pfree(m->src[i].buf);
	}
	sift_ltree_free(&m->lt);
	pfree(m->src);
	pfree(m);
}

static int
run_cmp(const void *a, const void *b)
{
	const uint64 x = ((const OsaFile *) a)->n,
				y = ((const OsaFile *) b)->n;

	return x < y ? -1 : x > y;
}

/* the smallest k runs are merged into one, which takes the place of the last of them */
static void
merge_down(OsaRuns *rs, int k)
{
	OsaMerge   *m;
	OsaFile		nf = {NULL, 0};
	OsaSlot		e;
	MemoryContext oc = MemoryContextSwitchTo(rs->cxt);

	nf.file = BufFileCreateTemp(false);
	MemoryContextSwitchTo(oc);
	qsort(rs->run, rs->nrun, sizeof(OsaFile), run_cmp);
	m = merge_begin(rs, rs->run, k);
	while (merge_next(m, &e))
	{
		BufFileWrite(nf.file, &e, sizeof(e));
		nf.n++;
	}
	merge_end(m);
	memmove(rs->run, rs->run + k, (rs->nrun - k) * sizeof(OsaFile));
	rs->nrun -= k;
	rs->run[rs->nrun++] = nf;
}

/* the buffer sorted, equal pairs added up, its entries returned in place */
static uint64
sort_buf(OsaRuns *rs)
{
	uint64		w = 0;

	osa_sort_slots(rs->buf, rs->n);
	for (uint64 i = 0; i < rs->n; i++)
	{
		if (w > 0 && rs->buf[w - 1].sid1 == rs->buf[i].sid1 && rs->buf[w - 1].key == rs->buf[i].key &&
			(uint64) rs->buf[w - 1].cnt + rs->buf[i].cnt <= UINT32_MAX)
			rs->buf[w - 1].cnt += rs->buf[i].cnt;
		else
			rs->buf[w++] = rs->buf[i];
	}
	return w;
}

/* the buffer is a run of its own file */
static void
flush_run(OsaRuns *rs)
{
	uint64		w = sort_buf(rs);
	MemoryContext old = MemoryContextSwitchTo(rs->cxt);
	BufFile    *f = BufFileCreateTemp(false);

	MemoryContextSwitchTo(old);
	BufFileWrite(f, rs->buf, w * sizeof(OsaSlot));
	if (rs->nrun == rs->run_cap)
	{
		rs->run_cap = Max(rs->run_cap * 2, 8);
		rs->run = rs->run ? repalloc(rs->run, rs->run_cap * sizeof(OsaFile)) : MemoryContextAlloc(rs->cxt, rs->run_cap * sizeof(OsaFile));
	}
	rs->run[rs->nrun++] = (OsaFile) {f, w};
	rs->n = 0;
	if (rs->nrun > OSA_MAX_RUNS)
		merge_down(rs, OSA_FANIN);
}

void
osa_runs_put(Osa *o, uint32 sid, uint64 key, uint32 cnt)
{
	OsaRuns    *rs = o->runs;

	if (rs->n == rs->cap)
	{
		if (rs->cap < rs->room)
		{
			OsaSlot    *nb;

			rs->cap = Min(rs->cap * 2, rs->room);
			nb = MemoryContextAlloc(rs->cxt, rs->cap * sizeof(OsaSlot));
			memcpy(nb, rs->buf, rs->n * sizeof(OsaSlot));
			pfree(rs->buf);
			rs->buf = nb;
		}
		else
			flush_run(rs);
	}
	rs->buf[rs->n++] = (OsaSlot) {key, sid + 1, cnt};
}

/* the scan has ended: what the runs hold is read in order from here (osa_runs_next) */
void
osa_runs_sort(Osa *o)
{
	OsaRuns    *rs = o->runs;

	if (rs->nrun == 0)
	{
		rs->n = sort_buf(rs);
		rs->inmem = true;
		rs->cur = 0;
		return;
	}
	if (rs->n > 0)
		flush_run(rs);
	if (rs->nrun > OSA_FANIN)
		merge_down(rs, rs->nrun - OSA_FANIN + 1);
	rs->merge = merge_begin(rs, rs->run, rs->nrun);
	rs->nrun = 0;
}

bool
osa_runs_next(Osa *o, OsaRun *r)
{
	OsaRuns    *rs = o->runs;
	OsaSlot		e;

	if (rs->inmem)
	{
		if (rs->cur >= rs->n)
			return false;
		e = rs->buf[rs->cur++];
	}
	else if (!merge_next(rs->merge, &e))
		return false;
	r->sid = e.sid1 - 1;
	r->key = e.key;
	r->cnt = e.cnt;
	return true;
}

void
osa_runs_end(Osa *o)
{
	OsaRuns    *rs = o->runs;

	if (rs == NULL)
		return;
	if (rs->merge)
		merge_end(rs->merge);
	for (int i = 0; i < rs->nrun; i++)
		BufFileClose(rs->run[i].file);
	if (rs->run)
		pfree(rs->run);
	pfree(rs->buf);
	pfree(rs);
	o->runs = NULL;
}
