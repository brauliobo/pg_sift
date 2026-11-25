/*
 * col/f2/part/pin_scan.c - what the scan of an index keeps of a part with its pin (col/f2/part/pin_scan.h).
 *
 * check-memory: allow-allocs every array follows the leaves or the granules of one part, and the pins around them are bounded by the pin cache
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/f2/part/pin_scan.h"
#include "col/f2/point_dir.h"

SiftF2PartPin *
sift_f2_scan_pin(Relation rel, const SiftPartDesc *d, uint32 part_id)
{
	return sift_f2_pin_open(rel, sift_f2_store_open(rel), d, part_id);
}

bool
sift_f2_exc_has(const SiftVariantExcSummary *ex, int nex, uint32 g, int col)
{
	int			lo = 0,
				hi = nex;

	while (lo < hi)
	{
		int			mid = lo + ((hi - lo) >> 1);

		if (ex[mid].granule < g || (ex[mid].granule == g && ex[mid].col < col))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < nex && ex[lo].granule == g && ex[lo].col == col;
}

const SiftVariantExcSummary *
sift_f2_pin_exc(Relation rel, SiftF2PartPin *pin, int *nexc)
{
	if (!pin->exc_read)
	{
		MemoryContext old = MemoryContextSwitchTo(pin->cx);
		uint64		len;

		pin->exc = sift_part_read_section(rel, pin->part, SEC_VARIANT_EXC, &len);
		pin->nexc = pin->exc != NULL ? len / sizeof(SiftVariantExcSummary) : 0;
		pin->exc_read = true;
		MemoryContextSwitchTo(old);
	}
	*nexc = pin->nexc;
	return pin->exc;
}

const SiftZone *
sift_f2_pin_zones(Relation rel, SiftF2PartPin *pin, int leaf)
{
	const SiftSecDirEntry *zs = sift_part_section(pin->part, SEC_ZONES);
	const uint32 ng = pin->part->hdr.ngranules;

	if (zs == NULL || zs->len != (uint64) pin->part->hdr.ncols * ng * sizeof(SiftZone))
		return NULL;
	if (pin->zones == NULL)
		pin->zones = MemoryContextAllocZero(pin->cx, (uint64) pin->part->hdr.ncols * sizeof(SiftZone *));
	if (pin->zones[leaf] == NULL)
	{
		pin->zones[leaf] = MemoryContextAlloc(pin->cx, (uint64) ng * sizeof(SiftZone));
		sift_part_read_section_bytes(rel, pin->part, zs, (uint64) leaf * ng * sizeof(SiftZone), (uint64) ng * sizeof(SiftZone), pin->zones[leaf]);
	}
	return pin->zones[leaf];
}

const SiftPointDir *
sift_f2_pin_point(Relation rel, SiftF2PartPin *pin)
{
	if (!pin->point_read)
	{
		pin->point = sift_point_dir_open(rel, pin->part, pin->cx);
		pin->point_read = true;
	}
	return pin->point;
}
