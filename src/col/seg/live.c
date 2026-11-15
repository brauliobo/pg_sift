/*
 * col/seg/live.c - the check and the repair of the liveness run of a column segment: its units as the run knows them, what sift.col_verify says of a run
 * that is not what sift_live_clear keeps it, and what sift.col_repair restores
 */
#include "postgres.h"

#include "col/seg_int.h"
#include "core/rowmem.h"

static SiftLiveUnit *
seg_units(SiftColSegment *seg)
{
	SiftLiveUnit *units = sift_rows_alloc(Max(seg->nsb, 1) * sizeof(SiftLiveUnit));

	for (uint32 u = 0; u < seg->nsb; u++)
		units[u] = (SiftLiveUnit) {seg->sbdir[u].id - seg->hdr->sb_lo, seg->sbdir[u].nrows, seg->sbdir[u].rank_base};
	return units;
}

void
sift_cseg_live_audit(SiftColSegment *seg, SiftLiveAudit *a)
{
	sift_live_audit(&PRIV(seg)->live, seg_units(seg), seg->nsb, a);
}

void
sift_cseg_live_repair(SiftColSegment *seg, SiftLiveAudit *a)
{
	sift_live_repair(&PRIV(seg)->live, seg_units(seg), seg->nsb, a);
}

#define LIVE_CORRUPT(seg, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" segment %llu is corrupted", RelationGetRelationName((seg)->index), ULL((seg)->entry.gen)), \
			 errdetail(__VA_ARGS__), \
			 errhint("A VACUUM of pg_sift 4.2.1 to 4.3.5 could damage the liveness pages of a segment of more than 65,152 superblocks: " \
					 "SELECT * FROM sift.col_repair('%s') restores them, REINDEX rebuilds the index.", RelationGetRelationName((seg)->index))))

void
sift_cseg_live_verify(SiftColSegment *seg)
{
	SiftLiveAudit a;

	sift_cseg_live_audit(seg, &a);
	if (a.pages_damaged > 0)
		LIVE_CORRUPT(seg, "%u liveness pages, the first block %u, have a header or a special space (type, flags, reserved, generation) that is not that of a liveness page of the segment.",
					 a.pages_damaged, a.first_damaged);
	if (a.units_lost > 0)
		LIVE_CORRUPT(seg, "%u units have a cleared liveness bit and no dirty bit: a scan reads their rows as live.", a.units_lost);
	if (a.ndead_header != a.cleared)
		LIVE_CORRUPT(seg, "The liveness header counts %llu dead rows, the liveness bits %llu.", ULL(a.ndead_header), ULL(a.cleared));
}
