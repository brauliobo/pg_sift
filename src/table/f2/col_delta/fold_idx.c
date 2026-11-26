/*
 * table/f2/col_delta/fold_idx.c - the patches of a chain as the columns hold them now: the ones a compaction folds, gathered by part and granule in the one pass that classifies the chain
 * (table/f2/col_delta/fold.c), so that a granule finds its folds without reading the chain again; and a patch a numeric column took as a scaled integer before ALTER TABLE .. TYPE widened it,
 * as the numeric the column keeps now.
 */
#include "postgres.h"

#include "col/f2/gbuf/gbuf.h"
#include "col/f2/store/schema.h"
#include "table/f2/col_delta/col_delta.h"

struct SiftColDeltaFoldIdx
{
	int			ngens;
	const uint64 *gens;			/* the parts rewritten, ascending */
	SiftColDeltaCollector **c;	/* of each part, NULL until a record of it folds */
	uint32	   *ngranules;		/* of each part: one past the last granule a record folds into */
	SiftColDeltaPartMap **map;	/* of each part once sealed, NULL for one no record folds into */
};

SiftColDeltaFoldIdx *
sift_col_delta_fold_idx_new(const uint64 *gens, int ngens)
{
	SiftColDeltaFoldIdx *x = palloc0(sizeof(SiftColDeltaFoldIdx));

	x->ngens = ngens;
	x->gens = gens;
	x->c = palloc0(sizeof(SiftColDeltaCollector *) * Max(ngens, 1));
	x->ngranules = palloc0(sizeof(uint32) * Max(ngens, 1));
	x->map = palloc0(sizeof(SiftColDeltaPartMap *) * Max(ngens, 1));
	return x;
}

void
sift_col_delta_fold_idx_add(SiftColDeltaFoldIdx *x, int gen, const uint8 *rec, const SiftColDeltaEntryHdr *h)
{
	if (x->c[gen] == NULL)
		x->c[gen] = sift_col_delta_collector_new();
	sift_col_delta_collect_record(x->c[gen], rec, h);
	x->ngranules[gen] = Max(x->ngranules[gen], h->granule_id + 1);
}

void
sift_col_delta_fold_idx_seal(SiftColDeltaFoldIdx *x)
{
	for (int g = 0; g < x->ngens; g++)
	{
		CHECK_FOR_INTERRUPTS();
		if (x->c[g] == NULL)
			continue;
		x->map[g] = sift_col_delta_part_map_merge(NULL, x->c[g], x->gens[g], x->ngranules[g]);
		sift_col_delta_collector_free(x->c[g]);
		x->c[g] = NULL;
	}
}

const SiftColDeltaPartMap *
sift_col_delta_fold_idx_part(const SiftColDeltaFoldIdx *x, int gen)
{
	return x->map[gen];
}

void
sift_col_delta_patch_widen(SiftColDeltaPatch *p, uint8 scale)
{
	bool		isnull = (p->flags & SIFT_CDCOL_IS_NULL) != 0;

	if (SIFT_CDCOL_KIND(p->flags) != SIFT_CELL_INT)
		return;
	if (!isnull)
	{
		const struct varlena *v = (const struct varlena *) DatumGetPointer(sift_f2_num64_datum(p->val.i, scale));

		p->val.s = (const char *) v;
		p->vlen = (uint16) VARSIZE_ANY(v);
	}
	p->flags = (uint8) ((p->flags & ~SIFT_CDCOL_KIND_MASK) | SIFT_CDCOL_PACK_FLAGS(false, SIFT_CELL_STR));
}

void
sift_col_delta_patch_widened(const SiftF2GranuleBuf *gb, int16 attnum, uint8 scale, bool *isnull, const char **s, uint32 *slen)
{
	const SiftColDeltaGranMap *gm = gb->patch != NULL ? gb->patch_arg : NULL;
	uint32		lo,
				hi;

	sift_col_delta_patch_slice(gm, attnum, &lo, &hi);
	for (uint32 k = lo; k < hi; k++)
	{
		SiftColDeltaPatch p = gm->patches[k];
		uint16		r = p.row_ordinal;

		sift_col_delta_patch_widen(&p, scale);
		isnull[r] = (p.flags & SIFT_CDCOL_IS_NULL) != 0;
		s[r] = isnull[r] ? NULL : memcpy(palloc(p.vlen), p.val.s, p.vlen);
		slen[r] = isnull[r] ? 0 : p.vlen;
	}
}
