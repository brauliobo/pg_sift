/*
 * bm25/runs/reader.c - the reader of a run: terms, documents and postings in order
 * the byte reader of a run file
 * the heap that orders the readers
 */
#include "core/host.h"
#include "core/cmp.h"
#include "core/varint.h"
#include "bm25/runs/runs_priv.h"

/* Read buffer of each of nruns readers: what half the budget leaves after their file buffers, at most IO_BUF */
static int
reader_buf(Size budget, int nruns)
{
	return Min(IO_BUF, Max(BLCKSZ, (int) Min(budget / 2 / nruns, INT_MAX) - BLCKSZ));
}

void
io_flush(RunIO *io)
{
	BufFileWrite(io->file, io->buf, io->len);
	io->off += io->len;
	io->len = 0;
}

/*
 * A run ends with a varint 0 (no more terms), then the samples of its terms: varint number of samples, varint offset of the 0, and
 * for each sample varint length, the term, varint offset of its record. A sample is taken at the first term and once SAMPLE_BYTES of the run
 * passed since the last one: a reader that wants the terms from one on (a range of a parallel build) starts at the last sample before it.
 */
#define SAMPLE_BYTES	TEST_KNOB(sift_test_range_bytes, 65536)

void
run_sample(RunIO *io, const char *term, int len)
{
	uint64		at = io_tell(io);
	uint8	   *p;

	if (io->nsmp > 0 && at - io->smp_at < SAMPLE_BYTES)
		return;
	if (io->smp_len + len + 2 * 10 > io->smp_cap)
	{
		io->smp_cap = Max(4096, io->smp_cap * 2);
		io->smp = io->smp ? repalloc(io->smp, io->smp_cap) : palloc(io->smp_cap);
	}
	p = sift_put_varint(io->smp + io->smp_len, len);
	memcpy(p, term, len);
	p = sift_put_varint(p + len, at);
	io->smp_len = p - io->smp;
	io->smp_at = at;
	io->nsmp++;
}

/* Writes the end of the run: no more terms, the samples */
void
run_finish(SiftRun *run, RunIO *io)
{
	uint64		end = io_tell(io);

	io_varint(io, 0);
	run->trailer = io_tell(io);
	io_varint(io, io->nsmp);
	io_varint(io, end);
	if (io->smp_len > 0)
		io_put(io, io->smp, io->smp_len);
	io_flush(io);
	if (io->smp)
		pfree(io->smp);
	io->smp = NULL;
	io->smp_len = io->smp_cap = 0;
}

void
runs_reader_next_term(RunReader *rd)
{
	if (rd->sc)
	{
		const char *term;
		SiftTermInfo info;

		if (!sift_seg_cursor_term(rd->sc, &term, &rd->len, &info))
		{
			rd->len = 0;
			return;
		}
		memcpy(rd->term, term, rd->len);
		rd->df = info.df;
		rd->thaspos = (info.flags & SIFT_TERM_POS) != 0;
		return;
	}
	rd->len = io_get_varint(&rd->io);
	if (rd->len == 0)
		return;
	SIFT_INVARIANT(rd->len <= SIFT_MAX_TERM_BYTES, "sift: corrupt build run (term of %d bytes)", rd->len);
	for (int i = 0; i < rd->len; i++)
		rd->term[i] = io_byte(&rd->io);
	rd->df = io_get_varint(&rd->io);
	rd->thaspos = io_get_varint(&rd->io);
	SIFT_INVARIANT(!rd->thaspos || rd->haspos, "sift: corrupt build run (positions in a run without)");
}

int
runs_reader_cmp(Datum a, Datum b, void *arg)
{
	const RunReader *x = (const RunReader *) DatumGetPointer(a),
			   *y = (const RunReader *) DatumGetPointer(b);
	int			c = sift_term_cmp(x->term, x->len, y->term, y->len);

	/* binaryheap keeps the largest on top */
	return c ? -c : y->idx - x->idx;
}

int
runs_doc_heap_cmp(Datum a, Datum b, void *arg)
{
	const RunReader *x = (const RunReader *) DatumGetPointer(a),
			   *y = (const RunReader *) DatumGetPointer(b);

	return pg_cmp_u64(y->dkey, x->dkey);
}

/* Moves to the next doc of the run; false when it has none */
bool
runs_reader_next_doc(RunReader *rd)
{
	if (rd->sc)
		return sift_seg_cursor_doc(rd->sc, &rd->dkey, &rd->ddl);
	if (rd->dleft == 0)
		return false;
	rd->dkey += io_get_varint(&rd->io);
	rd->ddl = io_get_varint(&rd->io);
	rd->dleft--;
	return true;
}

static inline void
record_put(RunReader *rd, uint8 c)
{
	if (rd->prlen == rd->prcap)
	{
		rd->prcap = Max(256, rd->prcap * 2);
		rd->prec = rd->prec ? repalloc_huge(rd->prec, rd->prcap) : MemoryContextAllocHuge(CurrentMemoryContext, rd->prcap);
	}
	rd->prec[rd->prlen++] = c;
}

static inline uint32
record_varint(RunReader *rd)
{
	uint32		x = 0;

	for (int s = 0;; s += 7)
	{
		uint8		c = io_byte(&rd->io);

		record_put(rd, c);
		x |= (uint32) (c & 0x7f) << s;
		if (!(c & 0x80))
			return x;
	}
}

/* The position record of the current posting (see bm25/analyze.h) into rd->prec */
static void
read_record(RunReader *rd)
{
	if (record_varint(rd) & 1)
	{
		uint32		n = record_varint(rd);

		for (uint32 i = 0; i <= n; i++)
			record_varint(rd);
	}
}

/* A run's postings of the current term */
bool
runs_post_next(RunReader *rd)
{
	uint64		v;

	if (rd->pleft == 0)
		return false;
	if (rd->sc)
	{
		const uint8 *rec;

		SIFT_INVARIANT(sift_seg_cursor_posting(rd->sc, &rd->pkey, &rd->ptf, &rd->pdl, &rec, &rd->prlen),
					   "sift: a segment has fewer live postings of a term than it counted");
		rd->prec = (uint8 *) rec;
		rd->pleft--;
		return true;
	}
	v = io_get_varint(&rd->io);
	rd->ptf = (v & 1) ? io_byte(&rd->io) : 1;
	rd->pkey += v >> 1;
	rd->pdl = io_get_varint(&rd->io);
	rd->prlen = 0;
	if (rd->thaspos)
		read_record(rd);
	rd->pleft--;
	return true;
}

/* Opens the runs for reading; the docs and the positions of every run are known by then */
RunReader *
runs_open_readers(SiftRun **runs, int nruns, Size budget, bool *positions, uint64 *ndocs)
{
	RunReader  *rd = palloc0(nruns * sizeof(RunReader));

	*positions = true;
	*ndocs = 0;
	for (int i = 0; i < nruns; i++)
	{
		rd[i].idx = i;
		rd[i].io.file = runs_run_open(runs[i]);
		rd[i].io.cap = reader_buf(budget, nruns);
		rd[i].io.buf = palloc(rd[i].io.cap);
		SIFT_INVARIANT(BufFileSeek(rd[i].io.file, 0, 0, SEEK_SET) == 0, "sift: could not rewind a build run");
		rd[i].dleft = io_get_varint(&rd[i].io);
		rd[i].haspos = io_get_varint(&rd[i].io);
		*ndocs += rd[i].dleft;
		*positions &= rd[i].haspos;
	}
	return rd;
}
