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

bool
sift_granule_point_str(const SiftGranuleView *v, int col, uint16 row, const char **p, uint32 *len)
{
	SiftChunk	c;
	bool		valid;

	col_open(v, col, &c);
	valid = sift_chunk_row_valid(&c, row);
	if (valid)
		sift_chunk_str_one(&c, sift_chunk_rank(&c, row), p, len);
	sift_chunk_release_strs(&c);
	return valid;
}

/* The strings of the ascending rows: the rank of each row is found from the one before it, and a few rows of a symbol-table chunk are decoded alone */
void
sift_granule_point_strs(const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, SiftStrCell *out)
{
	SiftChunk	c;
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	uint32	   *k = palloc(Max(nrows, 1) * sizeof(uint32));
	int			word = 0,
				rank = 0,
				m = 0;

	col_open(v, col, &c);
	for (int j = 0; j < nrows; j++)
	{
		out[j] = (SiftStrCell) {.isnull = !sift_chunk_row_valid(&c, rows[j])};
		if (!out[j].isnull)
			k[m++] = sift_chunk_rank_advance(&c, &word, &rank, rows[j]);
	}
	if (m > 0)
	{
		bool		sparse = c.enc == SIFT_CENC_STR_FSST && (uint32) m * 4 < k[m - 1] - k[0] + 1;
		int			base = sparse ? 0 : k[0];
		int			span = sparse ? m : k[m - 1] - k[0] + 1;
		/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
		const char **p = palloc(span * sizeof(char *));
		uint32	   *len = palloc(span * sizeof(uint32));

		if (sparse)
			for (int i = 0; i < m; i++)
				sift_chunk_str_one(&c, k[i], &p[i], &len[i]);
		else
			sift_chunk_strs(&c, k[0], k[m - 1] + 1, p, len);
		for (int j = 0, i = 0; j < nrows; j++)
			if (!out[j].isnull)
			{
				int			at = sparse ? i : k[i] - base;

				out[j].p = p[at];
				out[j].len = len[at];
				i++;
			}
		pfree(p);
		pfree(len);
	}
	pfree(k);
	sift_chunk_release_strs(&c);
}

bool
sift_granule_str_is_block(const SiftGranuleView *v, int col)
{
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	return (v->hdr->chunks[col + 1].enc & SIFT_ENC_MASK) == SIFT_CENC_STR_BLOCK;
}

uint8
sift_granule_point_tag(const SiftGranuleView *v, int col, uint16 row)
{
	SiftExcReader er;
	uint8		tag;

	sift_exc_reader_open(v, col, &er);
	tag = sift_exc_reader_tag(&er, row);
	sift_exc_reader_close(&er);
	return tag;
}
