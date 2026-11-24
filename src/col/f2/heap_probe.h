/*
 * col/f2/heap_probe.h - which entries of an index the heap has no live version for (the validation pass of CREATE INDEX CONCURRENTLY, col/f2/f2_write.c)
 */
#ifndef SIFT_COL_F2_HEAP_PROBE_H
#define SIFT_COL_F2_HEAP_PROBE_H

#include "postgres.h"

#include "storage/buf.h"
#include "utils/rel.h"

#include "col/f2/pend_del.h"

typedef struct SiftHeapProbe
{
	Relation	heap;
	TransactionId oldest;
	Buffer		vm;
	Buffer		page;
} SiftHeapProbe;

extern void sift_heap_probe_begin(SiftHeapProbe *pr, Relation heap);
extern void sift_heap_probe_end(SiftHeapProbe *pr);

/* No version of the row the entry at tid names is live or in progress: the delete of it, with the transaction that ended its last version (none when every snapshot sees it ended) */
extern bool sift_heap_probe_gone(SiftHeapProbe *pr, uint64 tid, SiftPendDelRec *rec);

#endif							/* SIFT_COL_F2_HEAP_PROBE_H */
