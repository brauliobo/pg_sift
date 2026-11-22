/*
 * col/granule/point_decode.c - one value of a column of a granule, decoded without the other rows of it (col/granule/granule.h): the point reads of a row fetched by address, where the
 * decode of the whole column (up to 65,535 values) would cost a thousand times the value. A scratch of the decode (a symbol table) is allocated in the current context. A column that is one
 * compressed block (sift_granule_str_is_block) has no such read: the block is inflated whole, and the caller keeps what it inflated for the next row.
 */
#include "postgres.h"

#include "col/granule/granule.h"
#include "col/granule/granule_int.h"

static void
col_open(const SiftGranuleView *v, int col, SiftChunk *c)
{
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	sift_chunk_open(v, col + 1, c);
}

static bool
point_open(const SiftGranuleView *v, int col, uint16 row, SiftChunk *c, int *k)
{
	col_open(v, col, c);
	if (!sift_chunk_row_valid(c, row))
		return false;
	*k = sift_chunk_rank(c, row);
	return true;
}

bool
sift_granule_point_str(const SiftGranuleView *v, int col, uint16 row, const char **p, uint32 *len)
{
	SiftChunk	c;
	int			k;

	if (!point_open(v, col, row, &c, &k))
		return false;
	sift_chunk_strs(&c, k, k + 1, p, len);
	return true;
}

/* The strings of the ascending rows: one run of the chunk, from the value of the first row to that of the last */
void
sift_granule_point_strs(const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, SiftStrCell *out)
{
	SiftChunk	c;
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	int		   *k = palloc(Max(nrows, 1) * sizeof(int));
	int			first = -1,
				last = -1;

	col_open(v, col, &c);
	for (int j = 0; j < nrows; j++)
	{
		out[j] = (SiftStrCell) {.isnull = !sift_chunk_row_valid(&c, rows[j])};
		if (out[j].isnull)
			continue;
		k[j] = sift_chunk_rank(&c, rows[j]);
		first = first < 0 ? k[j] : first;
		last = k[j];
	}
	if (first >= 0)
	{
		const char **p = palloc((last - first + 1) * sizeof(char *));
		uint32	   *len = palloc((last - first + 1) * sizeof(uint32));

		sift_chunk_strs(&c, first, last + 1, p, len);
		for (int j = 0; j < nrows; j++)
			if (!out[j].isnull)
			{
				out[j].p = p[k[j] - first];
				out[j].len = len[k[j] - first];
			}
		pfree(p);
		pfree(len);
	}
	pfree(k);
}

bool
sift_granule_str_is_block(const SiftGranuleView *v, int col)
{
	SiftChunk	c;

	sift_chunk_open(v, col + 1, &c);
	return c.enc == SIFT_CENC_STR_BLOCK;
}

uint8
sift_granule_point_tag(const SiftGranuleView *v, int col, uint16 row)
{
	SiftChunk	c;

	sift_chunk_open(v, col + 1, &c);
	if (c.exc != NULL)
	{
		const SiftVariantExcEntry *e = sift_exc_entries(c.exc);

		for (int i = 0; i < c.exc->n_entries; i++)
			if (e[i].row_idx == row)
				return e[i].reserved;
	}
	return 0;
}
