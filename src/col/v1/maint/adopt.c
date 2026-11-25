/*
 * col/v1/maint/adopt.c - the partitions of a key-cut index that no step has recorded
 *
 * An index written before the directory recorded partitions (5.0) is in partitions all the same: the segments of the size of one that no two meet in. One directory update records them, with
 * the bound of the least row of each (the first at the lowest key): it writes no segment and changes no generation, so the records of deleted rows (keyed by generation and unit) stay where
 * they are and no reader sees a different answer. The update is the directory's: a crash leaves the directory before it or after it.
 */
#include "postgres.h"
#include "col/v1/maint/int.h"
#include "col/v1/order/bound.h"
#include "core/dirpart.h"

typedef struct Adopt
{
	int			n;
	uint64	   *gens;
	SiftSegEntry *neu;
	uint8		(*lo)[SIFT_BOUND_BYTES];
} Adopt;

static void
adopt_switch(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	Adopt	   *a = arg;

	for (int j = 0; j < a->n; j++)
	{
		bool		found = false;

		for (int i = 0; i < dir->nsegs && !found; i++)
			found = dir->segs[i].gen == a->gens[j];
		SIFT_INVARIANT(found, "segment %llu left the directory while its partitions were recorded", (unsigned long long) a->gens[j]);
	}
	sift_dir_part_replace(a->gens, a->n, a->neu, a->n, a->lo);
}

/* The segments of the size of a partition that a key cut: their ranges, the bounds of their least and greatest rows, and their bytes; their number. idx: where each is in the snapshot */
static int
candidates(Relation index, const SiftDirSnapshot *snap, SiftRange *range, uint64 *bytes, int *idx)
{
	MemoryContext scx = maint_segment_cx();
	int			nc = 0;

	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		SiftColSegment *seg;
		bool		ok;

		if (e->kind != SIFT_SEG_IMMUTABLE || (e->flags & SIFT_SEG_SCOPE) || !(e->flags & SIFT_SEG_BYKEY) || !maint_is_partition(e))
			continue;
		seg = sift_cseg_open(index, e, scx);
		ok = sift_colk_segment_bound(seg, false, &range[nc].lo) && sift_colk_segment_bound(seg, true, &range[nc].hi);
		sift_cseg_close(seg);
		MemoryContextReset(scx);
		if (ok)
		{
			bytes[nc] = e->bytes;
			idx[nc++] = i;
		}
	}
	MemoryContextDelete(scx);
	return nc;
}

/* The directory version and index a pass found no partitions to record in: the segments are as they were then, so is the answer */
static Oid	tried_index = InvalidOid;
static uint64 tried_version = 0;

static bool
adopt_partitions(Relation index)
{
	SiftDirSnapshot *snap;
	SiftSegEntry *entries;
	SiftDirPart *parts;
	SiftRange  *range;
	uint64	   *bytes;
	uint32	   *pick;
	int		   *idx;
	int			nc,
				np;
	Adopt		a;

	if (!sift_col_is_index(index) || !sift_col_key_cut(sift_col_rel(index)))
		return false;
#ifdef SIFT_TESTING
	if (!sift_test_col_record_partitions)
		return false;
#endif
	snap = sift_col_dir_read(index, false);
	maint_use_partitions(index, snap);
	if (maint_part_kb == 0 || (tried_index == RelationGetRelid(index) && tried_version == snap->version) || sift_dir_parts_read(index, snap, &entries, &parts) > 0)
		return false;
	range = palloc(Max(snap->nsegs, 1) * sizeof(SiftRange));
	bytes = palloc(Max(snap->nsegs, 1) * sizeof(uint64));
	idx = palloc(Max(snap->nsegs, 1) * sizeof(int));
	pick = palloc(Max(snap->nsegs, 1) * sizeof(uint32));
	nc = candidates(index, snap, range, bytes, idx);
	np = sift_range_apart(range, bytes, nc, pick);
	tried_index = RelationGetRelid(index);
	tried_version = snap->version;
	if (np < 2)
		return false;
	a = (Adopt) {.n = np,.gens = palloc(np * sizeof(uint64)),.neu = palloc(np * sizeof(SiftSegEntry)),.lo = palloc(np * SIFT_BOUND_BYTES)};
	for (int k = 0; k < np; k++)
	{
		SiftBound	empty;

		sift_bound_clear(&empty);
		a.neu[k] = snap->segs[idx[pick[k]]];
		a.gens[k] = a.neu[k].gen;
		sift_bound_pack(k == 0 ? &empty : &range[pick[k]].lo, a.lo[k]);
	}
	sift_dir_update(index, &(SiftDirChange) {.fn = adopt_switch,.arg = &a,.extra = InvalidBuffer});
	return true;
}

bool
sift_col_adopt_partitions(Relation index)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift_col adopt partitions");
	MemoryContext old = MemoryContextSwitchTo(cx);
	bool		adopted = adopt_partitions(index);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return adopted;
}
