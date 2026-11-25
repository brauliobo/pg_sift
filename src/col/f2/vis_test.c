/*
 * col/f2/vis_test.c - test-only SQL functions sift._test_f2_*: the seal of a part and the heap visibility map for the rows of its granules (core/vis_f2.h).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/table.h"

#include "col/f2/test_int.h"
#include "core/catname.h"
#include "core/f2/vis.h"
#include "core/xid_cache.h"

PG_FUNCTION_INFO_V1(sift_test_f2_seal);
PG_FUNCTION_INFO_V1(sift_test_f2_vis_count);
PG_FUNCTION_INFO_V1(sift_test_f2_part_xids);
PG_FUNCTION_INFO_V1(sift_test_f2_vers);

/* (index, part_id, partition lower key, generation): seals the part for its inserters, the horizon being the next transaction id, and commits the descriptor; (sealed, flags of the descriptor) */
Datum
sift_test_f2_seal(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	SiftPartDesc d = *f2t_find_gen(parts, n, PG_GETARG_INT64(3));
	SiftXidHorizonCache hc;
	bool		sealed;
	Datum		v[2];
	bool		nl[2] = {0};

	sift_xid_horizon_init(&hc, ReadNextFullTransactionId());
	sealed = sift_part_try_seal(rel, (uint32) entry.part_id, &d, &hc);
	if (sealed)
		sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = entry.part_id,.key_lo = entry.key_lo,.update_parts = &d,.nupdate = 1});
	v[0] = BoolGetDatum(sealed);
	v[1] = Int32GetDatum(d.flags);
	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, nl);
}

/* (index, part_id, partition lower key, generation): the least and the greatest transaction id of the writers of the part, as the descriptor has them (0: none) */
Datum
sift_test_f2_part_xids(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	const SiftPartDesc *d = f2t_find_gen(parts, n, PG_GETARG_INT64(3));
	Datum		v[2] = {Int64GetDatum(U64FromFullTransactionId(d->min_xid)), Int64GetDatum(U64FromFullTransactionId(d->max_xid))};
	bool		nl[2] = {0};

	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, nl);
}

/* (index, part_id, partition lower key, generation): the granules of the part, those that are of one transaction and command, and the bytes of the version section (0: none) */
Datum
sift_test_f2_vers(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	const SiftPartDesc *d = f2t_find_gen(parts, n, PG_GETARG_INT64(3));
	SiftPart   *part = sift_part_open(rel, d->header_blk, d->part_gen, (uint32) entry.part_id);
	SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
	const SiftSecDirEntry *vs = sift_part_section(part, SEC_VERS);
	int			single = 0;
	Datum		v[3];
	bool		nl[3] = {0};

	for (uint32 g = 0; g < part->hdr.ngranules; g++)
		single += (dir[g].flags & SIFT_GDESC_SINGLE_XACT) != 0;
	v[0] = Int32GetDatum(part->hdr.ngranules);
	v[1] = Int32GetDatum(single);
	v[2] = Int64GetDatum(vs != NULL ? vs->len : 0);
	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, nl);
}

/*
 * (index, heap, stale): the rows of the parts of the index that the snapshot sees in the heap, by sift_vis_f2_granule_filter; the heap pages it read. stale: the index is read as a
 * scan that read the parts before the last deletion vectors were written would (no deletion vector, the manifest and the generations of the deletion vectors one behind): the map must not be trusted
 */
Datum
sift_test_f2_vis_count(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	Relation	heap = table_open(sift_rel_must_exist(PG_GETARG_OID(1)), AccessShareLock);
	bool		stale = PG_GETARG_BOOL(2);
	Snapshot	snap = GetActiveSnapshot();
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	SiftVis		vis;
	SiftVisF2Guard guard;
	uint64		visible = 0;
	Datum		v[2];
	bool		nl[2] = {0};

	sift_vis_begin(&vis, heap, snap, NULL, 0);
	sift_vis_f2_begin(&guard, rel, stale ? mr.version - 1 : mr.version);
	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int p = 0; p < np; p++)
		{
			SiftPartDesc *d = &parts[p];
			SiftPart   *part = sift_part_open(rel, d->header_blk, d->part_gen, (uint32) mr.entries[i].part_id);
			SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
			SiftDv	   *dv = d->dv_blk != InvalidBlockNumber ? sift_dv_read(rel, d, (uint32) mr.entries[i].part_id, f2t_dir_rows(rel, part)) : NULL;
			uint64		tlen;
			const SiftTidIndexEntry *tix = sift_part_read_section(rel, part, SEC_TID_INDEX, &tlen);
			SiftPartDesc seen = *d;

			if (stale && d->dv_blk != InvalidBlockNumber)
				seen.dv_gen = d->dv_gen - 1;
			sift_vis_f2_part(&guard, &mr.entries[i], &seen);
			for (uint32 g = 0; g < part->hdr.ngranules; g++)
			{
				uint64	   *tids = sift_part_read_tids(rel, part, dir, g);
				const SiftDvGran *dg = dv && !stale ? sift_dv_granule(dv, g) : NULL;
				uint64		sel[SIFT_DV_MASK_WORDS] = {0},
							dead[SIFT_DV_MASK_WORDS];

				for (int r = 0; r < dir[g].nrows; r++)
					sel[r >> 6] |= UINT64CONST(1) << (r & 63);
				if (dg != NULL && sift_dv_mask64(dg, snap, dead) > 0)
					for (int w = 0; w < SIFT_DV_MASK_WORDS; w++)
						sel[w] &= ~dead[w];
				visible += sift_vis_f2_granule_filter(&vis, &guard, tix[g].tid_min, tix[g].tid_max, tids, dir[g].nrows, sel);
			}
		}
	}
	v[0] = Int64GetDatum(visible);
	v[1] = Int64GetDatum(vis.heap_pages);
	sift_vis_end(&vis);
	table_close(heap, NoLock);
	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, nl);
}

#endif							/* SIFT_TESTING */
