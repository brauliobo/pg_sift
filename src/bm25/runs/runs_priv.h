/*
 * bm25/runs/runs_priv.h - what the files of the sorted runs share
 * the statics one file calls from another
 */
#ifndef SIFT_BM25_RUNS_PRIV_H
#define SIFT_BM25_RUNS_PRIV_H

#include "bm25/runs_int.h"

/* Merge input: one run positioned on a term */
typedef struct RunReader
{
	RunIO		io;
	int			idx;
	char		term[SIFT_MAX_TERM_BYTES];
	int			len;			/* 0 at end */
	uint64		df;
	bool		haspos;			/* the run holds position records */
	bool		thaspos;		/* ... for the current term */
	/* docs */
	uint64		dleft;
	SiftKey		dkey;
	uint16		ddl;
	/* postings of the current term */
	uint64		pleft;
	SiftKey		pkey;
	uint8		ptf;
	uint16		pdl;
	uint8	   *prec;			/* position record of the current posting */
	uint32		prlen;
	Size		prcap;
	SiftSegCursor *sc;			/* the reader of a segment, not of a run: its live docs and postings */
} RunReader;

/* Where a merge writes: a segment, or a run (the format of spill_to) that a later merge reads */
typedef struct MergeOut
{
	SiftSegmentWriter *sw;
	SiftRun    *run;			/* the run that is written, when there is no sw */
	RunIO		io;
	SiftKey		prev;
} MergeOut;

/* The samples of a run (see run_sample), read through the reader's file: where its terms end, and the sample at index i starts and stops */
typedef struct RunSample
{
	uint64		off;
	uint64		vol;			/* bytes up to the next sample, or the end of the terms */
	int			len;
	const char *term;
} RunSample;

extern void	runs_run_note_term(SiftRun *run, int len, uint64 df, uint64 posbytes);

extern BufFile *	runs_run_open(SiftRun *run);

extern void	runs_run_release(SiftRun *run);

extern void	runs_reader_next_term(RunReader *rd);

extern int	runs_reader_cmp(Datum a, Datum b, void *arg);

extern int	runs_doc_heap_cmp(Datum a, Datum b, void *arg);

extern bool	runs_reader_next_doc(RunReader *rd);

extern void	runs_merge_docs(MergeOut *mo, RunReader *rd, int nruns);

extern bool	runs_post_next(RunReader *rd);

extern RunReader *	runs_open_readers(SiftRun **runs, int nruns, Size budget, bool *positions, uint64 *ndocs);

extern void	runs_merge_terms(RunReader *rd, int nruns, bool positions, MergeOut *mo, const SiftTermCut *hi);

extern void	runs_merge_readers(RunReader *rd, int nruns, bool positions, MergeOut *mo);

extern SiftSegmentWriter *	runs_segw_begin(SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref, MemoryContext mcxt, bool positions);

extern int	runs_choose_cuts(RunSample *all, int nall, uint64 min_bytes, int target, SiftTermCut *cuts);

static inline uint8
io_byte(RunIO *io)
{
	if (io->pos == io->len)
	{
		io->len = BufFileRead(io->file, io->buf, io->cap);
		io->pos = 0;
		SIFT_INVARIANT(io->len != 0, "sift: unexpected end of a build run");
	}
	return io->buf[io->pos++];
}

static inline uint64
io_get_varint(RunIO *io)
{
	uint64		x = 0;

	for (int s = 0;; s += 7)
	{
		uint8		c = io_byte(io);

		x |= (uint64) (c & 0x7f) << s;
		if (!(c & 0x80))
			return x;
	}
}

#endif