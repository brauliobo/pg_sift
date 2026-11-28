/*
 * col/f2/compact/merge.h - the order in which a compaction reads the granules of its inputs (compact.c). It writes an output each time its memory fills, so the rows of an output are those
 * of the granules it read last: read in the order of their keys, a granule at a time from the input whose next rows can have the least key, the outputs of inputs that do not overlap do not
 * overlap either, and an input that holds a few rows inside the range of another (a row whose key an UPDATE changed) joins the output of that range, not the last one. Each part is sorted, so
 * the next rows of an input have a key at least that of its rows read last (of its first granule: the least key of the part).
 */
#ifndef SIFT_COMPACT_MERGE_H
#define SIFT_COMPACT_MERGE_H

#include "col/f2/compact/compact.h"

typedef struct SiftCompactMerge
{
	int			n;
	uint8		(*bound)[SIFT_BOUND_BYTES];	/* of each input: no row of it that is still to be read has a lesser key */
	bool	   *done;
} SiftCompactMerge;

/* The inputs at[0 .. n), none read yet */
extern SiftCompactMerge *sift_compact_merge_begin(const SiftPartDesc *const *at, int n);

/* The input to read a granule of next: the one with the least bound (of those with the same, the first), -1 when every input is done */
extern int	sift_compact_merge_next(const SiftCompactMerge *m);

/* The rows of input i just read (a granule or a slice of one): its bound is raised to the greatest key among them */
extern void sift_compact_merge_read(SiftCompactMerge *m, int i, const SiftRowBuf *rb);

/* The places of the inputs at[0 .. n) by their least key (of those with the same one, in the order given): the order of the shares of a compaction of several participants */
extern int *sift_compact_key_order(const SiftPartDesc *const *at, int n);

#endif							/* SIFT_COMPACT_MERGE_H */
