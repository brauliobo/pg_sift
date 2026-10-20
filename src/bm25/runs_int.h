/*
 * bm25/runs_int.h - shared by runs.c (the sorted runs and their merge) and inv.c (the inverted buffer).
 */
#ifndef SIFT_RUNS_INT_H
#define SIFT_RUNS_INT_H

#include "core/host.h"

#include "bm25/runs.h"

#define BATCH			1024
#define IO_BUF			65536

/*
 * What a term takes of a segment, at most: a posting 8 bytes of its blob besides its position record, a
 * dictionary entry 32 bytes besides the term (lengths, df, flags, bound; front coding is ignored).
 */
#define BLOB_POSTING_BYTES	8
#define DICT_ENTRY_BYTES	32

/* Buffered sequential IO on a run file */
typedef struct RunIO
{
	BufFile    *file;
	uint8	   *buf;
	int			cap;			/* of buf when reading */
	int			pos;
	int			len;
} RunIO;

/* Feeds postings of one term to the posting writer in batches */
typedef struct Batch
{
	SiftPostingWriter *pw;
	RunIO	   *run;			/* a run (see spill_to) to write the postings to, instead of pw */
	SiftKey		prev;			/* ... the key of its last posting */
	int			n;
	SiftKey		keys[BATCH];
	uint8		tf[BATCH];
	uint16		dl[BATCH];
	bool		haspos;			/* the term's postings carry position records */
	uint64		termpos;		/* bytes of the position records of the term's postings so far */
	uint8	   *pos;			/* of the n postings, back to back */
	Size		poslen;
	Size		poscap;
	uint32		plen[BATCH];
} Batch;

extern SIFT_INTERNAL void io_flush(RunIO *io);

static inline void
io_put(RunIO *io, const void *p, int n)
{
	if (io->len + n > IO_BUF)
		io_flush(io);
	if (n > IO_BUF)
	{
		BufFileWrite(io->file, p, n);
		return;
	}
	memcpy(io->buf + io->len, p, n);
	io->len += n;
}

static inline void
io_varint(RunIO *io, uint64 x)
{
	uint8		v[10];
	int			n = 0;

	for (; x >= 0x80; x >>= 7)
		v[n++] = (uint8) (x | 0x80);
	v[n++] = (uint8) x;
	io_put(io, v, n);
}

/* runs.c */
extern SIFT_INTERNAL SiftRun *run_new(MemoryContext mcxt, SharedFileSet *fileset, int worker, int id);
extern SIFT_INTERNAL void batch_flush(Batch *bt);
extern SIFT_INTERNAL void batch_put(Batch *bt, SiftKey key, uint8 tf, uint16 dl, const uint8 *rec, uint32 len);

#endif							/* SIFT_RUNS_INT_H */
