/*
 * agg/blob/int.h - what the files of agg/blob/ share
 * The merge's view of an aggregate in a dump, and the functions the dump, the sets and the
 * merge call in one another.
 */
#ifndef SIFT_AGG_BLOB_DIR_INT_H
#define SIFT_AGG_BLOB_DIR_INT_H

#include "agg/agg_int.h"

typedef struct OsaRec
{
	uint32		g;				/* group of the blob */
	uint32		pad;
	uint64		key;
	uint64		cnt;
} OsaRec;

extern void bw_values(StringInfo b, const OutTable *t, const uint32 *old, uint32 nused);
extern uint32 * renumber(uint32 *ids, uint32 n, uint32 nold, uint32 *nused);
extern bool set_agg(const SiftAggAgg *a);
extern bool set_words(const SiftAggAgg *a, const Dist *d);
extern void blob_set(AggExec *ex, int ai, const uint32 *gl, uint32 ng, const uint32 *cidx, const PartPairs *pre, StringInfo b);
extern void blob_osa(AggExec *ex, int ai, const uint32 *cidx, uint32 n, StringInfo b);
extern void blob_gen(AggExec *ex, int ai, const uint32 *gl, uint32 ng, StringInfo b);

typedef struct BlobAgg
{
	AggAcc		a;				/* the arrays of the blob, read only */
	uint32		npairs;
	const uint32 *pg;
	const uint32 *pv;
	uint32		nchunks;		/* count(DISTINCT) bitmap chunks: group and number of each, then their bits */
	const uint32 *cg;
	const uint32 *cc;
	const uint64 *bits;
	uint32		nwg;			/* a narrow set: the rows of words, of nk words, the first holding the values from 64 * wbase; the group of each */
	uint32		nk;
	int64		wbase;
	const uint32 *wg;
	const uint64 *wrows;
	uint32	   *xv;				/* dictionary values: the ids in the table */
	const struct varlena **nv;			/* numeric sums out of the integer form: the value of each group that has one */
	uint32		nrec;			/* ordered sets: the runs of the values */
	const OsaRec *rec;
	const char *gcur;			/* generic aggregates: the next partial state */
} BlobAgg;
extern bool numeric_sum_agg(const SiftAggAgg *a);
extern OutTable * mset_table(AggExec *ex, int ai);

static inline void
bw_put(StringInfo b, const void *p, Size n)
{
	appendBinaryStringInfo(b, p, n);
	while (b->len % 8)
		appendStringInfoChar(b, 0);
}

static inline const void *
br_get(const char **p, Size n)
{
	const void *r = *p;

	*p += TYPEALIGN(8, n);
	return r;
}

#endif							/* SIFT_AGG_BLOB_DIR_INT_H */
