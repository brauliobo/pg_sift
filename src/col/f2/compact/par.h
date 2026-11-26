/*
 * col/f2/compact/par.h - the compaction of one group of parts by the participants of VACUUM (PARALLEL n): the granules of its inputs are one list that is cut into as many shares as there
 * are participants (a share is the granules [lo, hi) of one part or of several, so that a group of one large part is shared too), each participant reads the rows of its share and writes them
 * as parts of its own, and the leader of the compaction commits what all of them wrote in the one change of the manifest of a serial compaction.
 */
#ifndef SIFT_COMPACT_PAR_H
#define SIFT_COMPACT_PAR_H

#include "col/f2/compact/compact.h"

/* The participants of the compaction of a group (the one that runs VACUUM and its workers) that VACUUM (PARALLEL n) asks for: fewer than 2 is a serial compaction */
extern PGDLLIMPORT int sift_vacuum_parallel_workers;

typedef enum SiftCompactParResult
{
	SIFT_COMPACT_PAR_OFF,		/* not compacted here: too small a group, no worker to launch, no VACUUM (PARALLEL n) */
	SIFT_COMPACT_PAR_DONE,		/* the outputs are written, none committed */
	SIFT_COMPACT_PAR_UNSETTLED	/* a deletion of an input is not settled: the outputs are to be given back */
} SiftCompactParResult;

/* The parts in[0 .. nin) of the partition, granules[i] the granules of in[i]; the outputs and what the participants read and wrote are in *out (nout of them, in the memory of the caller) and res */
extern SiftCompactParResult sift_compact_par_write(Relation rel, uint64 part_id, const SiftPartDesc *in, const uint32 *granules, int nin, int64 hot_cut, bool bounds_hold,
												   FullTransactionId horizon, SiftCompactResult *res, SiftPartDesc **out, int *nout);

/* compact.c: what one participant writes of its share of the inputs, the granules [lo[i], hi[i]) of in[i], into out (room for out_cap parts: more is an error, the parts given back); false: a deletion is not settled */
extern bool sift_compact_write_slice(Relation rel, uint64 part_id, const SiftPartSpec *spec, const SiftPartDesc *in, int nin, const uint32 *lo, const uint32 *hi, int64 hot_cut, bool bounds_hold,
									 FullTransactionId horizon, int nparticipants, SiftCompactResult *res, SiftPartDesc *out, int out_cap, int *nout);

#endif							/* SIFT_COMPACT_PAR_H */
