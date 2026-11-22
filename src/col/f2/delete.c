/*
 * col/delete.c - deleting rows by TID (col/delete.h).
 */
#include "core/host.h"

#include "storage/lmgr.h"

#include "col/f2/delete.h"
#include "col/f2/delvec_delta.h"
#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_del_slab.h"
#include "core/page_io.h"

typedef struct TidAt
{
	uint64		tid;
	uint16		row;
} TidAt;

static int
tidat_cmp(const void *a, const void *b)
{
	uint64		x = ((const TidAt *) a)->tid,
				y = ((const TidAt *) b)->tid;

	return x < y ? -1 : x > y;
}

/* A row that is dead already is an older one whose address a later row took after a VACUUM: a delete of that address is for another row */
static bool
row_dead(const SiftDv *base, uint32 g, uint16 row)
{
	const SiftDvGran *dg = base ? sift_dv_granule(base, g) : NULL;
	bool		dead = false;

	if (dg != NULL)
		(void) sift_dv_find(dg, row, &dead);
	return dead;
}

/* The rows the deletes name by the address their row has now (an address change of the column delta chain): the row is where the chain puts it, whatever raw address the part holds */
static int
locate_remapped(const SiftPart *part, const SiftCdRemapMap *rm, const SiftPendDelRec *recs, int n, bool *done, SiftDvMark *marks, const SiftDv *base)
{
	int			nm = 0;

	for (int i = 0; i < n; i++)
	{
		const SiftCdRemap *m = done[i] ? NULL : sift_cd_remap_by_new(rm, recs[i].tid);

		if (m == NULL || m->part_gen != part->hdr.part_gen || row_dead(base, m->granule_id, m->row))
			continue;
		marks[nm++] = (SiftDvMark) {m->granule_id, m->row, recs[i].xmax, recs[i].cmax};
		done[i] = true;
	}
	return nm;
}

/* The live row of the sorted granule that has the address raw in the part: a row that moved to another address has left it, a dead one is an older row the heap gave the address to again */
static const TidAt *
find_row(const TidAt *at, int nrows, uint64 tid, const SiftCdRemapMap *rm, uint64 part_gen, uint32 g, const SiftDv *base)
{
	int			lo = 0,
				hi = nrows;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (at[mid].tid < tid)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (; lo < nrows && at[lo].tid == tid; lo++)
		if ((rm == NULL || !sift_cd_remap_supersedes(rm, part_gen, g, at[lo].row, tid)) && !row_dead(base, g, at[lo].row))
			return &at[lo];
	return NULL;
}

/* Marks the rows of the part that the unresolved TIDs name; the TIDs it marked are set in done. rm: the address changes of the partition's column delta chain, NULL when it has none */
int
sift_del_locate(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, const SiftPendDelRec *recs, int n, bool *done, SiftDvMark *marks, const SiftDv *base, const SiftCdRemapMap *rm)
{
	int			nm = rm != NULL ? locate_remapped(part, rm, recs, n, done, marks, base) : 0;
	uint64		len;
	const SiftTidIndexEntry *tix = sift_part_read_section(rel, part, SEC_TID_INDEX, &len);

	if (tix == NULL || len != (uint64) part->hdr.ngranules * sizeof(*tix))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The part has no usable TID index.");
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		bool		any = false;
		TidAt	   *at;
		uint64	   *tids;

		for (int i = 0; i < n && !any; i++)
			any = !done[i] && recs[i].tid >= tix[g].tid_min && recs[i].tid <= tix[g].tid_max && (tix[g].blk_bloom & sift_tid_block_bit(recs[i].tid)) != 0;
		if (!any)
			continue;
		tids = sift_part_read_tids(rel, part, dir, g);
		/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
		at = palloc(dir[g].nrows * sizeof(TidAt));
		for (int r = 0; r < dir[g].nrows; r++)
			at[r] = (TidAt) {tids[r], r};
		qsort(at, dir[g].nrows, sizeof(TidAt), tidat_cmp);
		for (int i = 0; i < n; i++)
		{
			const TidAt *hit = done[i] ? NULL : find_row(at, dir[g].nrows, recs[i].tid, rm, part->hdr.part_gen, g, base);

			if (hit == NULL)
				continue;
			marks[nm++] = (SiftDvMark) {g, hit->row, recs[i].xmax, recs[i].cmax};
			done[i] = true;
		}
	}
	return nm;
}

static uint16 *
granule_rows(const SiftGranuleDesc *dir, uint32 ng)
{
	uint16	   *rows = palloc(Max(ng, 1) * sizeof(uint16));

	for (uint32 g = 0; g < ng; g++)
		rows[g] = dir[g].nrows;
	return rows;
}

typedef struct PartMarks
{
	SiftDvMark *marks;
	int			nm;
	SiftDv	   *base;
	uint16	   *grows;
	uint32		ngranules;
	bool		unaborted;
} PartMarks;

static int
mark_cmp(const void *a, const void *b)
{
	const SiftDvMark *x = a,
			   *y = b;

	if (x->granule != y->granule)
		return x->granule < y->granule ? -1 : 1;
	return (int) x->row - (int) y->row;
}

/* The marks, ascending, as the granules of a delta: rows is the array they point into */
static int
delta_grans(SiftDvMark *marks, int nm, SiftDvDeltaGran *gran, uint16 *rows)
{
	int			ng = 0;

	qsort(marks, nm, sizeof(SiftDvMark), mark_cmp);
	for (int i = 0; i < nm; i++)
	{
		rows[i] = marks[i].row;
		if (i == 0 || marks[i].granule != marks[i - 1].granule)
			gran[ng++] = (SiftDvDeltaGran) {.granule_id = marks[i].granule,.rows = rows + i};
		gran[ng - 1].nrows++;
	}
	return ng;
}

static bool
one_deleter(const SiftPendDelRec *recs, int n)
{
	for (int i = 1; i < n; i++)
		if (recs[i].xmax != recs[0].xmax || recs[i].cmax != recs[0].cmax)
			return false;
	return n > 0;
}

/* The marks of the statement appended in place to the vectors that take them; the parts they leave are in taken[] as false */
static void
append_deltas(Relation rel, uint64 part_id, SiftPartDesc *parts, PartMarks *pm, int nparts, const SiftPendDelRec *recs, int n, bool *taken)
{
	SiftDvDeltaPart *dp = palloc(Max(nparts, 1) * sizeof(SiftDvDeltaPart));
	int		   *at = palloc(Max(nparts, 1) * sizeof(int));
	bool	   *took = palloc(Max(nparts, 1) * sizeof(bool));
	int			nd = 0;

	if (!one_deleter(recs, n))
		return;
	for (int p = 0; p < nparts; p++)
	{
		SiftDvDeltaGran *gran;
		uint16	   *rows;

		if (pm[p].nm == 0 || pm[p].unaborted || !sift_dvd_part_takes(&parts[p]))
			continue;
		gran = palloc(pm[p].nm * sizeof(SiftDvDeltaGran));
		rows = palloc(pm[p].nm * sizeof(uint16));
		memset(gran, 0, pm[p].nm * sizeof(SiftDvDeltaGran));
		dp[nd] = (SiftDvDeltaPart) {.desc = &parts[p],.gran = gran,.ngran = delta_grans(pm[p].marks, pm[p].nm, gran, rows)};
		at[nd++] = p;
	}
	sift_dv_append_delta_batch(rel, (uint32) part_id, dp, nd, recs[0].xmax, recs[0].cmax, took);
	for (int i = 0; i < nd; i++)
		taken[at[i]] = took[i];
}

int
sift_del_bulk(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs_in, int n_in, bool fold, const SiftPartSpec *spec)
{
	SiftManifestRead mr;
	SiftPartitionEntry entry;
	SiftPartDesc *parts;
	int			nparts,
				n = n_in,
				ncow = 0,
				nrel = 0,
				npend = 0,
				npend_pages = 0,
				nkeep,
				resolved = 0;
	SiftPendDelRec *recs,
			   *keep,
			   *pend = NULL;
	BlockNumber *pend_pages = NULL;
	bool	   *done,
			   *taken;
	PartMarks  *pm;
	SiftCdRemapMap *rm;
	SiftPartDesc *updated;
	SiftExtent *rel_ext;
	SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo};

	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	entry = mr.entries[0];
	if (fold && entry.pend_del_blk != InvalidBlockNumber)
		pend = sift_pend_del_read(rel, part_id, entry.pend_del_blk, &npend, &pend_pages, &npend_pages);
	recs = palloc((n_in + npend + 1) * sizeof(SiftPendDelRec));
	memcpy(recs, recs_in, n_in * sizeof(SiftPendDelRec));
	if (npend > 0)
		memcpy(recs + n, pend, npend * sizeof(SiftPendDelRec));
	n += npend;
	keep = palloc(Max(n, 1) * sizeof(SiftPendDelRec));
	n = sift_pend_del_unpair(recs, n, keep, &nkeep);
	done = palloc0(Max(n, 1) * sizeof(bool));
	if (entry.l0_slab_blk != InvalidBlockNumber)
	{
		int			absorbed = sift_del_slab_flush(rel, part_id, key_lo, spec, entry.l0_slab_blk, recs, n, done);

		if (absorbed >= 0)
		{
			resolved += absorbed;
			mr = sift_manifest_read_range(rel, key_lo, NULL);
			entry = mr.entries[0];
		}
	}
	rm = sift_cdi_remap_load(rel, &entry);
	parts = sift_manifest_read_parts(rel, &entry, &nparts);
	pm = palloc0(Max(nparts, 1) * sizeof(PartMarks));
	taken = palloc0(Max(nparts, 1) * sizeof(bool));
	for (int p = 0; p < nparts; p++)
	{
		SiftPart   *part = sift_part_open_desc(rel, &parts[p], (uint32) part_id);
		SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);

		pm[p].ngranules = part->hdr.ngranules;
		pm[p].grows = granule_rows(dir, part->hdr.ngranules);
		if (parts[p].dv_blk != InvalidBlockNumber)
			pm[p].base = sift_dv_unabort(sift_dv_read(rel, &parts[p], (uint32) part_id, pm[p].grows), &pm[p].unaborted);
		pm[p].marks = palloc(Max(n, 1) * sizeof(SiftDvMark));
		pm[p].nm = sift_del_locate(rel, part, dir, recs, n, done, pm[p].marks, pm[p].base, rm);
		resolved += pm[p].nm;
	}
	sift_cd_remap_free(rm);
	append_deltas(rel, part_id, parts, pm, nparts, recs, n, taken);
	for (int p = 0; p < nparts; p++)
		ncow += pm[p].nm > 0 && !taken[p];
	updated = palloc(Max(ncow, 1) * sizeof(SiftPartDesc));
	rel_ext = palloc((ncow + npend_pages + 1) * sizeof(SiftExtent));
	if (ncow > 0)
	{
		uint64		gen = sift_manifest_new_gen(rel, ncow);
		SiftDv	  **dvs = palloc(ncow * sizeof(SiftDv *));
		SiftPartDesc **descs = palloc(ncow * sizeof(SiftPartDesc *));
		int			k = 0;

		for (int p = 0; p < nparts; p++)
		{
			if (pm[p].nm == 0 || taken[p])
				continue;
			dvs[k] = sift_dv_merge(pm[p].base, pm[p].marks, pm[p].nm, parts[p].part_gen, gen + k, pm[p].ngranules, pm[p].grows);
			updated[k] = parts[p];
			descs[k] = &updated[k];
			if (parts[p].dv_blk != InvalidBlockNumber)
				rel_ext[nrel++] = (SiftExtent) {parts[p].dv_blk, parts[p].dv_nblocks};
			k++;
		}
		sift_dv_write_batch(rel, (uint32) part_id, (const SiftDv *const *) dvs, descs, ncow);
	}
	for (int i = 0; i < npend_pages; i++)
		rel_ext[nrel++] = (SiftExtent) {pend_pages[i], 1};
	e.update_parts = updated;
	e.nupdate = ncow;
	e.set_pend_del = npend_pages > 0;
	e.new_pend_del_blk = InvalidBlockNumber;
	e.release = rel_ext;
	e.nrelease = nrel;
	if (e.nupdate > 0 || e.set_pend_del)
		sift_manifest_commit(rel, &e);
	if (nkeep > 0)
		(void) sift_pend_del_append(rel, part_id, key_lo, keep, nkeep);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return resolved;
}

void
sift_del_route(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs, int n, const SiftPartSpec *spec)
{
	if (n >= SIFT_BULK_DEL_MIN)
		(void) sift_del_bulk(rel, part_id, key_lo, recs, n, true, spec);
	else if (sift_pend_del_append(rel, part_id, key_lo, recs, n) >= SIFT_PEND_DEL_FOLD_PAGES)
		(void) sift_del_bulk(rel, part_id, key_lo, NULL, 0, true, spec);
}
