/*
 * col/f2/scan/part_answer.c - a sink that answers a whole part from what its header holds (the dictionaries of the part, the zones of its granules) without a granule read.
 */
#include "postgres.h"

#include "col/f2/f2_part_pin_scan.h"
#include "col/f2/scan/scan_int.h"

/* Whether the sink takes the part from the dictionaries of its leaves: the rows of the part are all of it, and each leaf the sink takes as codes has one */
bool
sift_f2_part_dicts_answer(Relation rel, F2Scan *s, const SiftPartitionEntry *e, SiftF2PartPin *pin, const SiftPendDelIndex *px)
{
	const SiftPartDicts *ds;
	SiftGranuleDict *dicts;
	bool		answered;

	if (s->part_dict_sink == NULL || s->sink_dict == NULL || s->npreds > 0 || s->bridge != NULL || px != NULL || (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 ||
		pin->desc.dv_blk != InvalidBlockNumber || pin->desc.ndead != 0)
		return false;
	ds = sift_part_dicts(rel, pin->part);
	dicts = palloc0(Max(s->nsink_leaves, 1) * sizeof(SiftGranuleDict));
	for (int j = 0; j < s->nsink_leaves; j++)
	{
		const SiftPartDict *pd = s->sink_dict[j] ? sift_part_dict_find(ds, s->sink_leaves[j], SIFT_DICT_KIND_STR) : NULL;

		if (pd != NULL)
			dicts[j] = (SiftGranuleDict) {.n = pd->n,.off = pd->off,.bytes = pd->bytes,.part = true};
	}
	answered = s->part_dict_sink(s->sink_arg, &pin->desc, dicts);
	pfree(dicts);
	return answered;
}

/* Whether the zones of the sink leaves answer the part: no row of it is excluded or changed, and no granule of a leaf lacks a value */
bool
sift_f2_zones_answer(Relation rel, F2Scan *s, SiftF2PartPin *pin, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px)
{
	const uint32 ng = pin->part->hdr.ngranules;
	SiftF2PartZone *zone;
	int			nexc;

	if (s->zone_sink == NULL || s->npreds > 0 || s->nsink_leaves == 0 || px != NULL || s->bridge != NULL || d->dv_blk != InvalidBlockNumber || (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA))
		return false;
	(void) sift_f2_pin_exc(rel, pin, &nexc);
	if (nexc > 0)
		return false;
	zone = palloc(s->nsink_leaves * sizeof(SiftF2PartZone));
	for (int j = 0; j < s->nsink_leaves; j++)
	{
		const SiftZone *z = sift_f2_pin_zones(rel, pin, s->sink_leaves[j]);

		zone[j] = (SiftF2PartZone) {.min = PG_INT64_MAX,.max = PG_INT64_MIN};
		if (z == NULL || ng == 0)
			return false;
		for (uint32 g = 0; g < ng; g++)
		{
			if (z[g].flags & SIFT_ZONE_NONE)
				return false;
			zone[j].min = Min(zone[j].min, z[g].min);
			zone[j].max = Max(zone[j].max, z[g].max);
		}
	}
	return s->zone_sink(s->sink_arg, d, zone);
}
