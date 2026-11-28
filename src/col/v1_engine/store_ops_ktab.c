/*
 * col/v1_engine/store_ops_ktab.c - SiftAggV6 answering whole segments of a Format 1 index from their key tables (col/v1/ktab.h, option key_table).
 *
 * The key table of a leaf is its histogram over every row of the segment. A statement the cube folds from the groups of that leaf (the key, the tests and the FILTERs all of it) takes the
 * table of a segment every unit of which is exact, once, at the first unit of it the source offers; its other units add nothing. The units of the other segments go to the cube.
 */
#include "postgres.h"

#include "agg/agg.h"
#include "agg/engine.h"
#include "col/source_fold.h"
#include "col/v1/store_ops.h"

typedef struct Ktab
{
	SiftV1UnitSink sink;
	SiftV1UnitSink *cube;
	SiftV6ScanStats *stats;
	MemoryContext cx;
	int			leaf;
	List	   *answered;		/* the segments their tables answered */
	List	   *read;			/* the segments that are read */
} Ktab;

static bool
answer(Ktab *k, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, const void *seg)
{
	SiftSrcHist h;
	const bool	ok = rd->seg_hist(src, unit, k->leaf, &h) && sift_v1_cube_fold_seg(k->cube, &h);
	MemoryContext old = MemoryContextSwitchTo(k->cx);

	if (ok)
		k->answered = lappend(k->answered, (void *) seg);
	else
		k->read = lappend(k->read, (void *) seg);
	MemoryContextSwitchTo(old);
	return ok;
}

static bool
offer(void *arg, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	Ktab	   *k = arg;
	const void *seg = unit->nsu == 0 ? rd->unit_seg(src, unit) : NULL;

	if (seg == NULL || list_member_ptr(k->read, seg) || (!list_member_ptr(k->answered, seg) && !answer(k, rd, src, unit, seg)))
		return k->cube->offer(k->cube->arg, rd, src, unit);
	k->stats->grans_preagg++;
	return true;
}

SiftV1UnitSink *
sift_v1_ktab_begin(const SiftSrcFoldArgs *a, MemoryContext cx, SiftV1UnitSink *cube)
{
	const SiftV1ReaderOps *rd = sift_v1_reader_of(a->host->rd_index == NULL);
	Ktab	   *k;

	if (cube == NULL || rd->seg_hist == NULL || sift_v1_cube_seg_leaf(cube) < 0)
		return cube;
	k = MemoryContextAllocZero(cx, sizeof(Ktab));
	k->cube = cube;
	k->stats = a->stats;
	k->cx = cx;
	k->leaf = sift_v1_cube_seg_leaf(cube);
	k->sink = (SiftV1UnitSink) {.offer = offer,.arg = k};
	return &k->sink;
}
