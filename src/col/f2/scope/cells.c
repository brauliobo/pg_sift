/*
 * col/f2/scope/cells.c - the cells of the rows of a scope's ring, read from the granule of the entity part that holds them (f2_scope_ent.c orders the rows by granule).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/f2/scope/int.h"

/* The cells of the rows of one granule, decoded once for all of them */
void
sift_f2s_granule_cells(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const SiftScopeNewest *const *rows, int nrows, const SiftScopeNewest *base, const int *leaves, int nleaves, SiftF2ScopeCell *cells, MemoryContext out)
{
	SiftPart   *part = sift_f2s_slot_part(p, s);
	bool	   *want = palloc0(part->hdr.ncols);
	SiftGranuleDesc gd;
	char	   *rec;
	SiftGranuleView v;

	for (int j = 0; j < nleaves; j++)
		want[leaves[j]] = true;
	rec = sift_part_read_granule_cols(p->rel, part, g, want, &gd);
	v = sift_granule_open_part(p->rel, part, rec, gd.data_len, &gd);
	for (int j = 0; j < nleaves; j++)
	{
		const SiftColDef *d = &sift_col_rel(p->rel)->schema->cols[leaves[j]];
		const int	n = gd.nrows;
		int64	   *iv = d->store == SIFT_COLS_INT ? palloc(n * sizeof(int64)) : NULL;
		double	   *fv = d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8 ? palloc(n * sizeof(double)) : NULL;
		SiftStrCell *sv = d->store == SIFT_COLS_STR ? palloc(n * sizeof(SiftStrCell)) : NULL;
		bool	   *nl = palloc(n);
		uint8	   *tag = palloc(n);

		if (iv != NULL)
			sift_granule_ints(&v, leaves[j], iv, nl);
		else if (fv != NULL)
			sift_granule_f8(&v, leaves[j], fv, nl);
		else
			sift_granule_strs(&v, leaves[j], sv);
		sift_granule_tags(&v, leaves[j], tag);
		for (int k = 0; k < nrows; k++)
		{
			const uint32 r = rows[k]->row_ord;
			SiftF2ScopeCell *c = &cells[(rows[k] - base) * nleaves + j];

			*c = (SiftF2ScopeCell) {.isnull = sv != NULL ? sv[r].isnull : nl[r],.tag = tag[r]};
			if (c->isnull)
				continue;
			if (iv != NULL)
				c->i = iv[r];
			else if (fv != NULL)
				c->f = fv[r];
			else
			{
				char	   *copy = MemoryContextAlloc(out, sv[r].len + 1);

				memcpy(copy, sv[r].p, sv[r].len);
				copy[sv[r].len] = '\0';
				c->s = copy;
				c->len = sv[r].len;
			}
		}
	}
}
