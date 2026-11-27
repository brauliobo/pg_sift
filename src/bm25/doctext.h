/*
 * bm25/doctext.h - the sorted docs of a promotion or a merge, and the text of the rows they stand for
 */
#ifndef SIFT_BM25_DOCTEXT_H
#define SIFT_BM25_DOCTEXT_H

#include "postgres.h"

#include "access/tableam.h"
#include "executor/executor.h"
#include "utils/tuplesort.h"

#include "bm25/analyze/analyze.h"
#include "bm25/doc.h"
#include "bm25/wbuf.h"

#define PACK_TERM	(2 + sizeof(uint32))

typedef struct DocCursor
{
	Tuplesortstate *sort;
	SiftKey		key;			/* of the doc last returned */
	uint16		dl;
	uint32		nterms;
	SiftTermFreq *terms;		/* point into the sorted doc, valid until the next call */
	uint32		maxterms;
	bool		any;
} DocCursor;

typedef struct DocText
{
	bool		snapshot;		/* a snapshot is pushed: the text of the docs of a batch may be out of line, and a worker has none */
	uint32		batch_left;		/* the docs the snapshot serves still */
	Relation	index;
	Relation	heap;
	IndexInfo  *info;
	EState	   *estate;
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
	SiftAnalyzer *an;
	SiftDocKeys keys;
	BlockNumber nblocks;
	bool		settle;			/* a row missed waits for the writers of the table first, once (doctext_fetch) */
	/* a table whose rows lie in an order of their own: the texts of the next docs read in that order (sift_doctext_prime), and handed out in the order of the docs */
	MemoryContext wcx;
	SiftKey    *wkey;
	text	  **wtext;
	uint32		nwin;
	uint32		wpos;
	uint32		wmax;			/* the docs of the next window: it doubles while the texts of a window take less than half of what they may, and halves when they take more */
} DocText;

extern bool sift_doc_next(DocCursor *c);
extern DocText *sift_doctext_open(Relation index, bool windowed);
extern void sift_doctext_close(DocText *d);
extern bool sift_doctext_writers_busy(Relation index);
extern void sift_doctext_prime(DocText *d, DocCursor *c);
extern bool sift_doctext_terms(DocText *d, const ItemPointerData *tid, SiftTermFreq **terms, int *nterms, uint32 *dl, int *len);

#endif
