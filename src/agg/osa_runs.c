/*
 * agg/osa_runs.c - the values of an ordered set that outgrew work_mem: sorted runs of (group, value, count) pairs, merged when the scan has ended
 * The histogram or array of pairs (agg/osa.c) is the first run's buffer; when it is full it is sorted, equal pairs are added up and it goes to a temporary
 * file. At the end the runs are merged in the order the readers walk (group, then value). A tuplesort of the same pairs takes about ten times the
 * instructions for each value (a heap tuple formed, compared through the generic comparator and read back for every value). More than OSA_FANIN runs
 * are merged into one while the scan goes on, so that the merge holds a fixed number of read buffers.
 */
#include "postgres.h"

#include "storage/buffile.h"

#include "agg/agg_int.h"

#define OSA_FANIN	16			/* runs merged at a time */
#define OSA_RB		512			/* entries one run is read in */

#define ST_SORT osa_sort_slots
#define ST_ELEMENT_TYPE OsaSlot
#define ST_COMPARE(a, b) ((a)->sid1 != (b)->sid1 ? ((a)->sid1 < (b)->sid1 ? -1 : 1) : ((a)->key < (b)->key ? -1 : ((a)->key > (b)->key ? 1 : 0)))
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

typedef struct OsaSrc
{
	uint64		pos;			/* next entry to read from the file */
	uint64		left;			/* entries of the run not read yet */
	OsaSlot    *buf;
	uint32		nbuf;
	uint32		at;
} OsaSrc;

typedef struct OsaRuns
{
	MemoryContext cxt;
	OsaSlot    *buf;			/* the run being made */
	uint64		n;
	uint64		cap;
	uint64		room;			/* the most entries a buffer may hold: work_mem */
	BufFile    *file;
	uint64		fend;			/* entries written */
	int			nrun;
	uint64		rstart[OSA_FANIN];
	uint64		rlen[OSA_FANIN];
	/* the merge */
	int			k;
	OsaSrc	   *src;
	int			cached;			/* the source with the smallest head, -2 not found yet */
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

static void
write_entries(BufFile *f, uint64 at, const OsaSlot *e, uint64 n)
{
	BufFileSeek(f, 0, (off_t) (at * sizeof(OsaSlot)), SEEK_SET);
	BufFileWrite(f, e, n * sizeof(OsaSlot));
}

static void
src_fill(OsaRuns *rs, OsaSrc *s)
{
	uint32		m = (uint32) Min(s->left, (uint64) OSA_RB);

	BufFileSeek(rs->file, 0, (off_t) (s->pos * sizeof(OsaSlot)), SEEK_SET);
	BufFileReadExact(rs->file, s->buf, m * sizeof(OsaSlot));
	s->pos += m;
	s->left -= m;
	s->nbuf = m;
	s->at = 0;
}

/* the runs [0, k) of the file are merged from here: one read buffer each */
static void
merge_open(OsaRuns *rs, int k)
{
	if (rs->src == NULL)
	{
		rs->src = MemoryContextAllocZero(rs->cxt, OSA_FANIN * sizeof(OsaSrc));
		for (int i = 0; i < OSA_FANIN; i++)
			rs->src[i].buf = MemoryContextAlloc(rs->cxt, OSA_RB * sizeof(OsaSlot));
	}
	rs->k = k;
	rs->cached = -2;
	for (int i = 0; i < k; i++)
	{
		rs->src[i].pos = rs->rstart[i];
		rs->src[i].left = rs->rlen[i];
		rs->src[i].nbuf = rs->src[i].at = 0;
		if (rs->src[i].left > 0)
			src_fill(rs, &rs->src[i]);
	}
}

/* the source with the smallest head, -1 when all are read */
static int
merge_min(OsaRuns *rs)
{
	int			best = -1;

	if (rs->cached != -2)
		return rs->cached;
	for (int i = 0; i < rs->k; i++)
		if (rs->src[i].at < rs->src[i].nbuf && (best < 0 || slot_less(&rs->src[i].buf[rs->src[i].at], &rs->src[best].buf[rs->src[best].at])))
			best = i;
	return rs->cached = best;
}

static void
merge_advance(OsaRuns *rs, int i)
{
	OsaSrc	   *s = &rs->src[i];

	if (++s->at == s->nbuf && s->left > 0)
		src_fill(rs, s);
	rs->cached = -2;
}

/* the next pair of the merged runs, equal pairs of several runs added up while their count fits */
static bool
merge_next(OsaRuns *rs, OsaSlot *out)
{
	int			m = merge_min(rs);

	if (m < 0)
		return false;
	*out = rs->src[m].buf[rs->src[m].at];
	merge_advance(rs, m);
	while ((m = merge_min(rs)) >= 0)
	{
		const OsaSlot *h = &rs->src[m].buf[rs->src[m].at];

		if (h->sid1 != out->sid1 || h->key != out->key || (uint64) out->cnt + h->cnt > UINT32_MAX)
			break;
		out->cnt += h->cnt;
		merge_advance(rs, m);
	}
	return true;
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

/* OSA_FANIN runs are in the file: they are merged into one in a new file */
static void
merge_down(OsaRuns *rs)
{
	BufFile    *old = rs->file,
			   *nf;
	uint64		total = 0,
				w = 0;
	OsaSlot		e;
	MemoryContext oc = MemoryContextSwitchTo(rs->cxt);

	nf = BufFileCreateTemp(false);
	MemoryContextSwitchTo(oc);

	merge_open(rs, rs->nrun);
	while (merge_next(rs, &e))
	{
		rs->buf[w++] = e;
		if (w == rs->cap)
		{
			write_entries(nf, total, rs->buf, w);
			total += w;
			w = 0;
		}
	}
	if (w > 0)
	{
		write_entries(nf, total, rs->buf, w);
		total += w;
	}
	BufFileClose(old);
	rs->file = nf;
	rs->fend = total;
	rs->nrun = 1;
	rs->rstart[0] = 0;
	rs->rlen[0] = total;
}

/* the buffer is a run of the file */
static void
flush_run(OsaRuns *rs)
{
	uint64		w = sort_buf(rs);

	if (rs->file == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(rs->cxt);

		rs->file = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	write_entries(rs->file, rs->fend, rs->buf, w);
	rs->rstart[rs->nrun] = rs->fend;
	rs->rlen[rs->nrun] = w;
	rs->nrun++;
	rs->fend += w;
	rs->n = 0;
	if (rs->nrun == OSA_FANIN)
		merge_down(rs);
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

	if (rs->file == NULL)
	{
		rs->n = sort_buf(rs);
		rs->inmem = true;
		rs->cur = 0;
		return;
	}
	if (rs->n > 0)
		flush_run(rs);
	merge_open(rs, rs->nrun);
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
	else if (!merge_next(rs, &e))
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
	if (rs->file)
		BufFileClose(rs->file);
	pfree(rs->buf);
	if (rs->src)
	{
		for (int i = 0; i < OSA_FANIN; i++)
			pfree(rs->src[i].buf);
		pfree(rs->src);
	}
	pfree(rs);
	o->runs = NULL;
}
