/*
 * col/granule/codes.c - the codes of a dictionary coded string column of a granule, for a consumer that groups by them and needs a string once for each distinct value.
 */
#include "core/host.h"

#include "col/granule/int.h"
#include "core/damage.h"

const SiftPartDict *
sift_part_dict_find(const SiftPartDicts *ds, int col, SiftDictKind kind)
{
	for (int i = 0; ds != NULL && i < ds->n; i++)
		if (ds->d[i].col == col && ds->d[i].kind == kind)
			return &ds->d[i];
	return NULL;
}

bool
sift_granule_str_codes(const SiftGranuleView *v, int col, uint32 *codes, bool *isnull, SiftGranuleDict *dict)
{
	SiftChunk	c;
	const char *blob;
	uint32		blob_len;
	int64	   *vals;
	int			k = 0;

	sift_chunk_open(v, col + 1, &c);
	if (c.enc != SIFT_CENC_DICT_CODE)
		return false;
	sift_chunk_dict(&c, dict, &blob, &blob_len);
	vals = sift_alloc(Max(c.nnonnull, 1) * sizeof(int64));
	sift_cc_blob_ints(blob, blob_len, c.nnonnull, 0, c.nnonnull, vals);
	for (int r = 0; r < c.nrows; r++)
	{
		isnull[r] = !sift_chunk_row_valid(&c, r);
		codes[r] = isnull[r] ? 0 : (uint32) vals[k++];
		if (codes[r] >= dict->n && !isnull[r])
			SIFT_FAIL_CORRUPT("sift: a dictionary code is past its dictionary");
	}
	sift_free(vals);
	return true;
}
