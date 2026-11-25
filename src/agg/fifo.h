/*
 * agg/fifo.h - the tuples one step of a node made and its caller has not taken yet. A node that gives its rows as an engine makes them (a stream of groups, a window) steps the engine until a
 * tuple is here, gives the tuples one by one and, with the queue empty, steps again: what is held is a step's worth of tuples, and a statement that stops reading (a LIMIT) leaves the rest of the
 * input unread and writes no file. A step makes a vector of tuples whatever work_mem is: the queue holds that much in memory, and spills past it as a tuplestore does.
 */
#ifndef SIFT_AGG_V6_FIFO_H
#define SIFT_AGG_V6_FIFO_H

#include "postgres.h"

#include "executor/tuptable.h"
#include "utils/tuplestore.h"

#define SIFT_V6_FIFO_MIN_KB	1024

typedef struct SiftV6Fifo
{
	MemoryContext row_cx;		/* what one tuple takes to be built, reset by put */
	Tuplestorestate *store;
} SiftV6Fifo;

/* The queue, in the memory context of the query */
extern void sift_v6_fifo_open(SiftV6Fifo *f, MemoryContext query_cx);
extern void sift_v6_fifo_put(SiftV6Fifo *f, TupleDesc desc, const Datum *values, const bool *isnull);
/* The next tuple in the slot, NULL (and an empty queue) when there is none */
extern TupleTableSlot *sift_v6_fifo_get(SiftV6Fifo *f, TupleTableSlot *slot);
/* Drops the tuples that are not taken */
extern void sift_v6_fifo_clear(SiftV6Fifo *f);
extern void sift_v6_fifo_close(SiftV6Fifo *f);

#endif							/* SIFT_AGG_V6_FIFO_H */
