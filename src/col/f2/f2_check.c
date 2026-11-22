/*
 * col/f2/f2_check.c - the key invariants of the parts of an index of format major 2: the sort cells of the rows of a part do not descend (rows that tie on them may be in any order of
 * their addresses: sift_rowbuf_cluster), the key range of a part in the manifest is exactly that of its rows (the least and the greatest), every granule's rows lie inside it and so does
 * their zone of the lead key column, and the part lies inside its partition.
 */
#include "postgres.h"

#include "catalog/pg_class.h"

#include "col/f2/f2.h"
#include "col/f2/f2_store.h"
#include "core/page_io.h"

static void
cells_of_row(const SiftPartSpec *spec, int nint, int64 *const *ints, bool *const *nulls, int r, SiftCell *cells)
{
	for (int k = 0; k < nint; k++)
		cells[spec->sort_cols[k]] = (SiftCell) {.isnull = nulls[k][r],.i = ints[k][r]};
}

/* The quantile sketches of a part: the section is well formed and no sketch holds more values than the part has rows */
static void
check_sketches(Relation rel, const SiftPartSpec *spec, const SiftPart *part, const SiftPartDesc *d)
{
	SiftF2KllSec sec;

	if (!sift_f2_kll_load(rel, part, &sec))
		return;
	for (int i = 0; spec->kll != NULL && i < spec->kll->n; i++)
	{
		const uint8 *blob;
		uint32		len;
		uint64		n;

		if (sift_kll_sec_find(sec.data, sec.len, spec->kll->col[i], SIFT_KLL_WHOLE_PART, &blob, &len, &n) && n > d->nrows)
			SIFT_CORRUPT(rel, "Part %llu: the quantile sketch of leaf %u holds %llu values and the part has %llu rows.", (unsigned long long) d->part_gen, (unsigned) spec->kll->col[i],
						 (unsigned long long) n, (unsigned long long) d->nrows);
	}
}

static void
check_part(Relation rel, const SiftPartSpec *spec, const SiftPartitionEntry *e, const SiftPartDesc *d, uint64 *granules, uint64 *rows)
{
	SiftPart   *part = sift_part_open_desc(rel, d, (uint32) e->part_id);
	SiftCell   *cells = palloc0(Max(spec->ncols, 1) * sizeof(SiftCell));
	int			nint = sift_spec_int_keys(spec);
	uint8		prev[SIFT_BOUND_BYTES],
				least[SIFT_BOUND_BYTES],
				greatest[SIFT_BOUND_BYTES] = {0},
				key[SIFT_BOUND_BYTES] = {0},
				prefix[SIFT_BOUND_BYTES];
	bool		have_prev = false;

	memset(least, 0xFF, sizeof(least));
	const SiftSecDirEntry *zs = sift_part_section(part, SEC_ZONES);

	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		bool	   *want = palloc0(Max(spec->ncols, 1));
		SiftGranuleDesc gd;
		char	   *rec;
		SiftGranuleView v;
		uint64	   *tids;
		int64	   *ints[SIFT_KEY_ORDER_MAX];
		bool	   *nulls[SIFT_KEY_ORDER_MAX];
		int64		zmin = PG_INT64_MAX,
					zmax = PG_INT64_MIN;

		for (int k = 0; k < nint; k++)
			want[spec->sort_cols[k]] = true;
		rec = sift_part_read_granule_cols(rel, part, g, want, &gd);
		v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);
		/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
		tids = palloc(gd.nrows * sizeof(uint64));
		sift_granule_tids(&v, tids);
		for (int k = 0; k < nint; k++)
		{
			/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
			ints[k] = palloc(gd.nrows * sizeof(int64));
			nulls[k] = palloc(gd.nrows);
			sift_granule_ints(&v, spec->sort_cols[k], ints[k], nulls[k]);
		}
		for (int r = 0; r < gd.nrows; r++)
		{
			cells_of_row(spec, nint, ints, nulls, r, cells);
			sift_row_key(spec, cells, tids[r], key);
			sift_row_key_prefix(spec, cells, prefix);
			if (have_prev && memcmp(prev, prefix, SIFT_BOUND_BYTES) > 0)
				SIFT_CORRUPT(rel, "Part %llu: the keys of its rows do not ascend at granule %u row %d.", (unsigned long long) d->part_gen, g, r);
			if (memcmp(key, least, SIFT_BOUND_BYTES) < 0)
				memcpy(least, key, SIFT_BOUND_BYTES);
			if (memcmp(key, greatest, SIFT_BOUND_BYTES) > 0)
				memcpy(greatest, key, SIFT_BOUND_BYTES);
			memcpy(prev, prefix, SIFT_BOUND_BYTES);
			have_prev = true;
			if (nint > 0 && !nulls[0][r])
			{
				zmin = Min(zmin, ints[0][r]);
				zmax = Max(zmax, ints[0][r]);
			}
			if (memcmp(key, d->key_lo, SIFT_BOUND_BYTES) < 0 || memcmp(key, d->key_hi, SIFT_BOUND_BYTES) > 0 || memcmp(key, e->key_lo, SIFT_BOUND_BYTES) < 0 ||
				(memcmp(e->key_hi, (uint8[SIFT_BOUND_BYTES]) {0}, SIFT_BOUND_BYTES) != 0 && memcmp(key, e->key_hi, SIFT_BOUND_BYTES) >= 0))
				SIFT_CORRUPT(rel, "Part %llu: a row of granule %u lies outside the key range of its part or of its partition.", (unsigned long long) d->part_gen, g);
		}
		if (nint > 0 && zs != NULL && zs->len == (uint64) part->hdr.ncols * part->hdr.ngranules * sizeof(SiftZone))
		{
			SiftZone	z;

			sift_part_read_bytes(rel, part, zs->off + ((uint64) spec->sort_cols[0] * part->hdr.ngranules + g) * sizeof(z), sizeof(z), &z);
			if (zmin <= zmax ? (z.flags & SIFT_ZONE_NONE) || z.min != zmin || z.max != zmax : !(z.flags & SIFT_ZONE_NONE))
				SIFT_CORRUPT(rel, "Part %llu: the zone of the lead key column of granule %u is not the range of its rows.", (unsigned long long) d->part_gen, g);
		}
		*rows += gd.nrows;
		(*granules)++;
	}
	if (have_prev && (memcmp(least, d->key_lo, SIFT_BOUND_BYTES) != 0 || memcmp(greatest, d->key_hi, SIFT_BOUND_BYTES) != 0))
		SIFT_CORRUPT(rel, "Part %llu: its key range in the manifest is not that of its least and greatest row.", (unsigned long long) d->part_gen);
	check_sketches(rel, spec, part, d);
}

char *
sift_f2_check_keys(Relation rel)
{
	const SiftPartSpec *spec = rel->rd_rel->relkind == RELKIND_INDEX ? &sift_f2_rel(rel)->spec : &sift_f2_store_open(rel)->spec;
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	uint64		parts = 0,
				granules = 0,
				rows = 0;

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int p = 0; p < np; p++)
			check_part(rel, spec, &mr.entries[i], &pd[p], &granules, &rows);
		parts += np;
	}
	return psprintf("parts=%llu granules=%llu rows=%llu exact", (unsigned long long) parts, (unsigned long long) granules, (unsigned long long) rows);
}
