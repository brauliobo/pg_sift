/*
 * agg/spill/int.h - what agg/spill.c and the sweep of agg/spill/stream.c share
 * The state of the spill of a scan, and the helpers of its first order key.
 */
#ifndef SIFT_AGG_SPILL_DIR_INT_H
#define SIFT_AGG_SPILL_DIR_INT_H

#include "agg/agg_int.h"

#define SPILL_MAX_PARTS		128		/* one BufFile (8 kB) each */
#define SPILL_MIN_GROUPS	128		/* a table with fewer groups is not spilled: its memory is something else (the key ranges) */

typedef struct SpillSet
{
	int			depth;
	int			n;				/* partitions: for ranges, those of the values and one more for NULL */
	int64	   *bound;			/* ranges: n - 2 points, ascending; partition p < n - 1 holds the values from bound[p - 1] up to bound[p] (those of its parent at the ends) */
	BufFile    *f[SPILL_MAX_PARTS + 1];
	uint32		nblobs[SPILL_MAX_PARTS + 1];
} SpillSet;


typedef struct AggSpill
{
	Size		budget;			/* hash memory */
	Size		floor;			/* what an empty table holds */
	uint32		keep;			/* groups of the table written last: the next table of the scan starts with room for them */
	int			nparts;
	bool		spilled;
	int64		groups;			/* of the partitions returned so far */
	Tuplestorestate *out;		/* the groups returned, in order: the table holds one partition, a rescan replays them */
	TupleTableSlot *mslot;
	int64		nread;
	int64		nwritten;
	bool		replayed;		/* the node was scanned again: the store is read as well as written (the groups made since are stepped over) */
	bool		done;
	MemoryContext tmp;			/* the temporaries of one blob, dumped or merged (agg/blob/ keeps them for the life of a process) */
	SpillSet   *set;			/* the level 0 partitions the scan writes */
	List	   *work;			/* partitions to merge, as SpillWork */
	double	   *amax;			/* avg(float8): the largest absolute input of the tables written */
	bool		ranged;			/* the partitions are ranges of the first order key, in the order of the result */
	bool		desc;
	bool		nulls_first;
	pg_prng_state prng;
	int64	   *sample;			/* ranged: values of the first order key of the groups written to the stream */
	uint32		nsample;
	uint64		nseen;
	uint64		nstream;		/* ranged: groups written to the stream */
} AggSpill;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* spill.c */
extern bool spill_over(AggExec *ex);
extern int64 spill_datum_int(Oid typid, Datum d);
extern Datum spill_int_datum(Oid typid, int64 v);
extern bool spill_lead_value(AggExec *ex, uint32 g, int64 *v);
extern bool spill_lead_zoned(AggExec *ex);

/* stream.c */
extern bool gstream_holds(const AggExec *ex);
extern bool gstream_next(AggExec *ex);

#pragma GCC visibility pop

#endif							/* SIFT_AGG_SPILL_DIR_INT_H */
