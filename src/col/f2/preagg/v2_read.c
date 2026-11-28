/*
 * col/f2/preagg/v2_read.c - SEC_PREAGG version 2 as the reader and the join of parts see it (col/f2/preagg/v2.h): the header and cubes of a part, the cells of one granule, the sets of
 * distinct values, and the section of parts joined end to end.
 */
#include "core/host.h"

#include "lib/stringinfo.h"

#include "col/f2/part/format.h"
#include "col/f2/preagg/v2.h"
#include "core/page_io.h"

#define CORRUPT(rel, part, what) SIFT_SEG_CORRUPT(rel, (part)->hdr.part_gen, "The pre-aggregate section of a part is damaged: %s.", what)

/* The cubes and the sets of distinct values the header names are inside the len bytes of the section */
static void
check_places(Relation rel, const SiftPart *part, const SiftPreaggSecHdr *hdr, uint64 len)
{
	if (hdr->ncubes < 1 || hdr->ncubes > SIFT_PREAGG_MAX_CUBES || hdr->cubes_off > len || (uint64) hdr->ncubes * sizeof(SiftPreaggCubeDesc) > len - hdr->cubes_off)
		CORRUPT(rel, part, "its cubes");
	if (hdr->dist_off > len)
		CORRUPT(rel, part, "its distinct values");
}

static void
check_cubes(Relation rel, const SiftPart *part, const SiftPreaggSecHdr *hdr, const SiftPreaggCubeDesc *cube, uint64 len)
{
	if (hdr->magic != SIFT_PREAGG_MAGIC || hdr->version != SIFT_PREAGG_VERSION || hdr->ncubes < 1 || hdr->ncubes > SIFT_PREAGG_MAX_CUBES || hdr->ngranules != part->hdr.ngranules)
		CORRUPT(rel, part, "its header");
	for (int k = 0; k < hdr->ncubes; k++)
	{
		const SiftPreaggCubeDesc *cd = &cube[k];

		if (cd->ndims > SIFT_PREAGG_MAX_DIMS || cd->nmeasures > SIFT_PREAGG_MAX_MEAS || cd->ndistinct > SIFT_PREAGG_MAX_DIST ||
			(uint64) cd->gran_dir_off + ((uint64) hdr->ngranules + 1) * sizeof(uint32) > len || cd->cells_off > len)
			CORRUPT(rel, part, "a cube");
	}
}

SiftPreaggPart *
sift_preagg_part_open(Relation rel, const SiftPart *part)
{
	const SiftSecDirEntry *sec = sift_part_section(part, SEC_PREAGG);
	SiftPreaggPart *pp;

	if (sec == NULL)
		return NULL;
	pp = palloc0(sizeof(SiftPreaggPart));
	pp->part = part;
	pp->sec_len = sec->len;
	if (sec->len < sizeof(pp->hdr))
		CORRUPT(rel, part, "its size");
	sift_part_read_section_bytes(rel, part, sec, 0, sizeof(pp->hdr), &pp->hdr);
	check_places(rel, part, &pp->hdr, sec->len);
	sift_part_read_section_bytes(rel, part, sec, pp->hdr.cubes_off, pp->hdr.ncubes * sizeof(SiftPreaggCubeDesc), pp->cube);
	check_cubes(rel, part, &pp->hdr, pp->cube, sec->len);
	return pp;
}

void
sift_preagg_gran_read(Relation rel, const SiftPreaggPart *pp, int cube, uint32 g, SiftPreaggGran *out)
{
	const SiftPreaggCubeDesc *cd = &pp->cube[cube];
	const SiftSecDirEntry *sec = sift_part_section(pp->part, SEC_PREAGG);
	const uint32 size = sift_pag_cell_size(cd);
	uint32		span[2];

	if (g >= pp->hdr.ngranules)
		CORRUPT(rel, pp->part, "a granule number");
	sift_part_read_section_bytes(rel, pp->part, sec, cd->gran_dir_off + (uint64) g * sizeof(uint32), sizeof(span), span);
	if (span[0] > span[1] || cd->cells_off + (uint64) span[1] * size > pp->sec_len)
		CORRUPT(rel, pp->part, "the cells of a granule");
	out->ncells = span[1] - span[0];
	out->cells = palloc(Max(out->ncells, 1) * (uint64) size);
	sift_part_read_section_bytes(rel, pp->part, sec, cd->cells_off + (uint64) span[0] * size, (uint64) out->ncells * size, out->cells);
}

uint8 *
sift_preagg_dist_read(Relation rel, const SiftPreaggPart *pp, const SiftPreaggDistRef *ref)
{
	uint8	   *out;

	if (ref->len == 0)
		return NULL;
	if (pp->hdr.dist_off == 0 || pp->hdr.dist_off + ref->off + ref->len > pp->sec_len)
		CORRUPT(rel, pp->part, "a set of distinct values");
	out = palloc(ref->len);
	sift_part_read_section_bytes(rel, pp->part, sift_part_section(pp->part, SEC_PREAGG), pp->hdr.dist_off + ref->off, ref->len, out);
	return out;
}

uint32
sift_preagg_dist_values(const SiftPreaggDistRef *ref, const uint8 *bytes, int64 *out, uint32 cap)
{
	uint32		n = 0;

	switch (ref->mode)
	{
		case SIFT_PDIST_U32_LIST:
			for (uint32 i = 0; i < ref->len / sizeof(uint32); i++)
			{
				uint32		v;

				memcpy(&v, bytes + i * sizeof(v), sizeof(v));
				if (n == cap)
					return cap + 1;
				out[n++] = v;
			}
			break;
		case SIFT_PDIST_I64_LIST:
			for (uint32 i = 0; i < ref->len / sizeof(int64); i++)
			{
				if (n == cap)
					return cap + 1;
				memcpy(&out[n++], bytes + i * sizeof(int64), sizeof(int64));
			}
			break;
		case SIFT_PDIST_DICT_BM:
			for (uint32 i = 0; i < ref->len; i++)
				for (uint32 bits = bytes[i]; bits != 0; bits &= bits - 1)
				{
					if (n == cap)
						return cap + 1;
					out[n++] = (int64) i * 8 + __builtin_ctz(bits);
				}
			break;
		default:
			break;
	}
	return n;
}

static bool
same_cube(const SiftPreaggCubeDesc *a, const SiftPreaggCubeDesc *b)
{
	return ((a->flags ^ b->flags) & SIFT_PCUBE_KIND) == 0 && a->ndims == b->ndims && a->nmeasures == b->nmeasures && a->ndistinct == b->ndistinct && a->max_groups_per_gran == b->max_groups_per_gran &&
		memcmp(a->dim_leaf, b->dim_leaf, sizeof(a->dim_leaf)) == 0 && memcmp(a->dim_kind, b->dim_kind, sizeof(a->dim_kind)) == 0 &&
		memcmp(a->meas_kind, b->meas_kind, sizeof(a->meas_kind)) == 0 && memcmp(a->meas_leaf, b->meas_leaf, sizeof(a->meas_leaf)) == 0 &&
		memcmp(a->dist_leaf, b->dist_leaf, sizeof(a->dist_leaf)) == 0;
}

/* The cells of granule g of a cube with the sets of distinct values they name, appended to cells and dist: the bytes they take in the section */
static uint64
copy_cells(Relation rel, const SiftPart *part, const char *sec, uint64 slen, const SiftPreaggCubeDesc *cd, const uint32 *pf, uint32 g, StringInfo cells, StringInfo dist)
{
	const SiftPreaggSecHdr *hdr = (const SiftPreaggSecHdr *) sec;
	const uint32 size = sift_pag_cell_size(cd);
	const uint64 before = (uint64) cells->len + dist->len;

	for (uint32 i = pf[g]; i < pf[g + 1]; i++)
	{
		SiftPreaggDistRef *ref;

		appendBinaryStringInfo(cells, sec + cd->cells_off + (uint64) i * size, size);
		ref = (SiftPreaggDistRef *) (cells->data + cells->len - cd->ndistinct * sizeof(SiftPreaggDistRef));
		for (int d = 0; d < cd->ndistinct; d++)
		{
			const uint32 off = ref[d].off;

			if (ref[d].len > 0 && (hdr->dist_off == 0 || hdr->dist_off + off + ref[d].len > slen))
				CORRUPT(rel, part, "a set of distinct values");
			ref[d].off = (uint32) dist->len;
			appendBinaryStringInfo(dist, sec + hdr->dist_off + off, ref[d].len);
		}
	}
	return (uint64) cells->len + dist->len - before;
}

/*
 * The cells of the first part are kept as they are; those of the parts after it are kept while the cubes of the joined part stay within the budget of a part (SiftPreaggBudget) over the bytes of
 * its granules so far, as if it had been written whole: a granule a cube leaves out is marked in dir (SIFT_GDESC_PREAGG_SKIP)
 */
void *
sift_preagg_v2_join(Relation rel, SiftPart *const *src, int n, SiftGranuleDesc *dir, const SiftPreaggDecl *decl, uint64 *len)
{
	SiftPreaggSecHdr h0 = {0};
	SiftPreaggCubeDesc cube[SIFT_PREAGG_MAX_CUBES];
	StringInfoData cells[SIFT_PREAGG_MAX_CUBES],
				dist;
	uint32	   *first[SIFT_PREAGG_MAX_CUBES];
	uint32		ng = 0,
				goff = 0,
				flags = 0;
	SiftPreaggBudget budget = {.slack = SIFT_PAG_BUDGET_SLACK};
	void	   *out = NULL;

	for (int i = 0; i < n; i++)
		ng += src[i]->hdr.ngranules;
	initStringInfo(&dist);
	for (int i = 0; i < n; i++)
	{
		uint64		slen;
		char	   *sec = sift_part_read_section(rel, src[i], SEC_PREAGG, &slen);
		const SiftPreaggSecHdr *hdr = (const SiftPreaggSecHdr *) sec;
		const SiftPreaggCubeDesc *cd;

		if (sec == NULL || slen < sizeof(*hdr))
			CORRUPT(rel, src[i], "its size");
		check_places(rel, src[i], hdr, slen);
		cd = (const SiftPreaggCubeDesc *) (sec + hdr->cubes_off);
		check_cubes(rel, src[i], hdr, cd, slen);
		if (i == 0)
		{
			h0 = *hdr;
			memcpy(cube, cd, hdr->ncubes * sizeof(*cd));
			for (int k = 0; k < hdr->ncubes; k++)
			{
				initStringInfo(&cells[k]);
				first[k] = palloc0((ng + 1) * sizeof(uint32));
			}
		}
		else if (hdr->ncubes != h0.ncubes)
			goto done;
		flags |= hdr->flags;
		for (int k = 0; k < h0.ncubes; k++)
		{
			const uint32 *pf = (const uint32 *) (sec + cd[k].gran_dir_off);

			if (!same_cube(&cd[k], &cube[k]))
				goto done;
			if (cd[k].cells_off + (uint64) pf[hdr->ngranules] * sift_pag_cell_size(&cd[k]) > slen)
				CORRUPT(rel, src[i], "the cells of a cube");
			cube[k].flags |= cd[k].flags;
		}
		for (uint32 g = 0; g < hdr->ngranules; g++)
		{
			budget.data += dir[goff + g].data_len;
			for (int k = 0; k < h0.ncubes; k++)
			{
				const uint32 *pf = (const uint32 *) (sec + cd[k].gran_dir_off);
				const bool	elements = (cd[k].flags & SIFT_PCUBE_ELEMENTS) != 0;
				const uint32 at = cells[k].len,
							dat = dist.len;
				uint64		bytes;

				first[k][goff + g] = at / sift_pag_cell_size(&cd[k]);
				if (pf[g] == pf[g + 1])
					continue;
				bytes = copy_cells(rel, src[i], sec, slen, &cd[k], pf, g, &cells[k], &dist);
				if (i > 0 && !sift_preagg_cube_declared(decl, &cd[k]) && !sift_preagg_v2_fits(&budget, elements, bytes))
				{
					cells[k].len = at;
					dist.len = dat;
					dir[goff + g].flags |= SIFT_GDESC_PREAGG_SKIP;
				}
				else
					budget.used[elements] += bytes;
			}
		}
		goff += hdr->ngranules;
		pfree(sec);
	}
	for (int k = 0; k < h0.ncubes; k++)
		first[k][ng] = cells[k].len / sift_pag_cell_size(&cube[k]);
	out = sift_preagg_v2_assemble(h0.ncubes, cube, ng, flags, first, cells, &dist, len);
done:
	return out;
}
