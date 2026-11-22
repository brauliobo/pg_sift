/*
 * col/preagg_delta.c - the pre-aggregate of a granule and its correction for deleted rows (col/preagg_delta.h).
 */
#include "core/host.h"

#include "col/f2/part_format.h"
#include "col/f2/preagg_delta.h"
#include "core/hash.h"
#include "core/page_io.h"
#include "port/pg_bitutils.h"

static int
cell_cmp(const void *a, const void *b)
{
	int64		x = ((const SiftPreaggCell *) a)->key,
				y = ((const SiftPreaggCell *) b)->key;

	return x < y ? -1 : x > y;
}

/* The cell of a key in cells (first seen order) through the table of slots, each a cell number plus one: the cells that are there are found without a walk of them all */
static SiftPreaggCell *
cell_slot(SiftPreaggCell *cells, uint32 *slots, uint32 mask, int64 key, uint32 **slot)
{
	uint32		i = (uint32) (sift_fmix64((uint64) key) & mask);

	while (slots[i] != 0 && cells[slots[i] - 1].key != key)
		i = (i + 1) & mask;
	*slot = &slots[i];
	return slots[i] != 0 ? &cells[slots[i] - 1] : NULL;
}

SiftPreaggCell *
sift_preagg_build(const SiftGranuleView *v, int dim_col, int measure_col, int *ncells)
{
	int			n = v->hdr->nrows;
	int64	   *dim = palloc(n * sizeof(int64)),
			   *val = palloc(n * sizeof(int64));
	bool	   *dnull = palloc(n * sizeof(bool)),
			   *vnull = palloc(n * sizeof(bool));
	SiftPreaggCell *cells = palloc(Max(n, 1) * sizeof(SiftPreaggCell));
	uint32		mask = pg_nextpower2_32(Max(2 * (uint32) n, 16)) - 1;
	uint32	   *slots = palloc0((mask + 1) * sizeof(uint32));
	int			nc = 0;

	sift_granule_ints(v, dim_col, dim, dnull);
	sift_granule_ints(v, measure_col, val, vnull);
	for (int r = 0; r < n; r++)
	{
		uint32	   *slot;
		SiftPreaggCell *c;

		if (dnull[r])
			continue;
		c = cell_slot(cells, slots, mask, dim[r], &slot);
		if (c == NULL)
		{
			c = &cells[nc++];
			*slot = nc;
			*c = (SiftPreaggCell) {.key = dim[r],.min = PG_INT64_MAX,.max = PG_INT64_MIN};
		}
		if (vnull[r])
			continue;
		c->count++;
		c->sum += val[r];
		c->sumsq += (double) val[r] * (double) val[r];
		c->min = Min(c->min, val[r]);
		c->max = Max(c->max, val[r]);
	}
	pfree(slots);
	qsort(cells, nc, sizeof(SiftPreaggCell), cell_cmp);
	*ncells = nc;
	return cells;
}

/* The cell of the key in the cells (ascending by key), or NULL */
static SiftPreaggCell *
cell_of(SiftPreaggCell *cells, int nc, int64 key)
{
	SiftPreaggCell probe = {.key = key};

	return bsearch(&probe, cells, nc, sizeof(SiftPreaggCell), cell_cmp);
}

bool
sift_preagg_subtract_rows(const SiftGranuleView *v, int dim_col, int measure_col, const uint16 *excl_rows, uint16 nexcl, SiftPreaggCell *cells, int *ncells)
{
	int64	   *dim = palloc(Max(nexcl, 1) * sizeof(int64)),
			   *val = palloc(Max(nexcl, 1) * sizeof(int64));
	bool	   *dnull = palloc(Max(nexcl, 1) * sizeof(bool)),
			   *vnull = palloc(Max(nexcl, 1) * sizeof(bool));
	int			nc = *ncells,
				out = 0;
	SiftPreaggCell *work = palloc(Max(nc, 1) * sizeof(SiftPreaggCell));

	sift_granule_point_ints(v, dim_col, excl_rows, nexcl, dim, dnull);
	sift_granule_point_ints(v, measure_col, excl_rows, nexcl, val, vnull);
	memcpy(work, cells, nc * sizeof(SiftPreaggCell));
	for (int i = 0; i < nexcl; i++)
	{
		SiftPreaggCell *c;

		if (dnull[i])
			continue;
		c = cell_of(work, nc, dim[i]);
		if (c == NULL)
			return false;
		if (vnull[i])
			continue;
		if (c->count > 1 && (val[i] == c->min || val[i] == c->max))
			return false;
		c->count--;
		c->sum -= val[i];
		c->sumsq -= (double) val[i] * (double) val[i];
	}
	for (int k = 0; k < nc; k++)
		if (work[k].count > 0 || work[k].min == PG_INT64_MAX)
			cells[out++] = work[k];
	*ncells = out;
	return true;
}

bool
sift_preagg_subtract(const SiftGranuleView *v, int dim_col, int measure_col, const SiftDvGran *dead, SiftPreaggCell *cells, int *ncells)
{
	return sift_preagg_delta_ok(v->hdr->nrows, dead->ndead) && sift_preagg_subtract_rows(v, dim_col, measure_col, dead->rows, dead->ndead, cells, ncells);
}

uint16
sift_excl_union_u16(const uint16 *a, uint16 na, const uint16 *b, uint16 nb, uint16 *out, uint16 cap)
{
	uint32		i = 0,
				j = 0,
				n = 0;

	while (i < na || j < nb)
	{
		uint16		next;

		if (j == nb || (i < na && a[i] < b[j]))
			next = a[i++];
		else if (i == na || b[j] < a[i])
			next = b[j++];
		else
		{
			next = a[i++];
			j++;
		}
		if (n == cap)
			return cap + 1;
		out[n++] = next;
	}
	return n;
}

/* The cells of granule g of the section (a first[ngranules + 1] of cell numbers, padded, then the cells): the two numbers of the granule and its cells are read, not the section */
SiftPreaggCell *
sift_part_read_preagg(Relation rel, const struct SiftPart *part, uint32 g, int *ncells)
{
	const SiftSecDirEntry *sec = sift_part_section(part, SEC_PREAGG);
	uint32		ng = part->hdr.ngranules;
	uint64		head = MAXALIGN((ng + 1) * sizeof(uint32));
	uint32		span[2];
	SiftPreaggCell *out;

	if (sec == NULL)
		return NULL;
	if (sec->len < head || g >= ng)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The pre-aggregate section holds %llu bytes for %u granules.", (unsigned long long) sec->len, ng);
	sift_part_read_section_bytes(rel, part, sec, g * sizeof(uint32), sizeof(span), span);
	if (span[0] > span[1] || head + (uint64) span[1] * sizeof(SiftPreaggCell) > sec->len)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The pre-aggregate section holds %llu bytes for %u granules.", (unsigned long long) sec->len, ng);
	*ncells = span[1] - span[0];
	out = palloc(Max(*ncells, 1) * sizeof(SiftPreaggCell));
	sift_part_read_section_bytes(rel, part, sec, head + (uint64) span[0] * sizeof(SiftPreaggCell), (uint64) *ncells * sizeof(SiftPreaggCell), out);
	return out;
}
