/*
 * col/granule/point_decode.c - one value of a column of a granule, decoded without the other rows of it (col/granule/granule.h): the point reads of a row fetched by address, where the
 * decode of the whole column (up to 65,535 values) would cost a thousand times the value. A scratch of the decode (a symbol table) is allocated in the current context. A column that is one
 * compressed block (sift_granule_str_is_block) has no such read: the block is inflated whole, and the caller keeps what it inflated for the next row. A framed chunk of a sift_table inflates
 * the frame of 256 values the row lies in.
 */
#include "postgres.h"

#include "core/damage.h"

#include "col/am.h"
#include "col/fsst.h"
#include "col/granule/granule.h"
#include "col/granule/granule_filter.h"
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

/* The strings of ranks k[0..m) (ascending) of an FSST chunk: the coded lengths are read once for the chunk, the m strings are decoded one after the other into one buffer */
static void
fsst_sel(const SiftChunk *c, const uint32 *k, int m, const char **p, uint32 *len)
{
	SiftFsstChunk f;
	uint64		cap = SIFT_FSST_SLACK,
				at = 0;
	uint8	   *out;

	if (!sift_fsst_chunk_open(c, &f))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the symbol table of a string chunk is damaged"));
	sift_fsst_chunk_offsets(&f, c->nnonnull);
	for (int i = 0; i < m; i++)
		cap += 8 * (uint64) f.clen[k[i]];
	out = palloc(cap);
	for (int i = 0; i < m; i++)
	{
		uint32		n = sift_fsst_decode(f.t, f.coded + f.off[k[i]], (uint32) f.clen[k[i]], out + at);

		if (n == UINT32_MAX)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a coded string does not decode"));
		p[i] = (const char *) out + at;
		len[i] = n;
		at += n;
	}
	sift_fsst_chunk_close(&f);
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
			fsst_sel(&c, k, m, p, len);
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
sift_granule_point_str_codes(const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, uint32 *codes, bool *isnull, SiftGranuleDict *dict)
{
	SiftChunk	c;
	const char *blob;
	uint32		blob_len;
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	uint32	   *k = palloc(Max(nrows, 1) * sizeof(uint32));
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	int64	   *vals = palloc(Max(nrows, 1) * sizeof(int64));
	int			word = 0,
				rank = 0,
				m = 0;

	col_open(v, col, &c);
	if (c.enc != SIFT_CENC_DICT_CODE)
	{
		sift_chunk_release_strs(&c);
		return false;
	}
	sift_chunk_dict(&c, dict, &blob, &blob_len);
	for (int j = 0; j < nrows; j++)
	{
		isnull[j] = !sift_chunk_row_valid(&c, rows[j]);
		if (!isnull[j])
			k[m++] = sift_chunk_rank_advance(&c, &word, &rank, rows[j]);
	}
	if (m > 0)
	{
		SiftCc	   *cc = sift_cc_blob_open(blob, blob_len, c.nnonnull, NULL);

		sift_cc_decode_sel(cc, k, m, vals);
		sift_cc_close(cc);
	}
	for (int j = 0, i = 0; j < nrows; j++)
	{
		codes[j] = isnull[j] ? 0 : (uint32) vals[i++];
		if (!isnull[j] && codes[j] >= dict->n)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a dictionary code is past its dictionary"));
	}
	pfree(k);
	pfree(vals);
	return true;
}

bool
sift_granule_str_is_block(const SiftGranuleView *v, int col)
{
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	return (v->hdr->chunks[col + 1].enc & (SIFT_ENC_MASK | SIFT_ENC_FRAMED)) == SIFT_CENC_STR_BLOCK;
}

bool
sift_granule_frame_ref(const SiftGranuleView *v, int col, uint16 row, SiftFrameRef *ref)
{
	SiftChunk	c;
	bool		framed;

	col_open(v, col, &c);
	framed = sift_chunk_framed(&c) && sift_chunk_row_valid(&c, row);
	if (framed)
	{
		int			k = sift_chunk_rank(&c, row);

		*ref = (SiftFrameRef) {k / SIFT_FRAME_ROWS, (uint16) (k % SIFT_FRAME_ROWS)};
	}
	sift_chunk_release_strs(&c);
	return framed;
}

void
sift_granule_frame_str(const SiftGranuleView *v, int col, uint32 frame, uint32 **off, char **bytes)
{
	SiftChunk	c;

	col_open(v, col, &c);
	sift_frame_strs(&c, frame, off, bytes);
	sift_chunk_release_strs(&c);
}

void
sift_granule_frame_f8(const SiftGranuleView *v, int col, uint32 frame, double *out)
{
	SiftChunk	c;

	col_open(v, col, &c);
	sift_frames_f8(&c, (int) (frame * SIFT_FRAME_ROWS), Min((int) ((frame + 1) * SIFT_FRAME_ROWS), c.nnonnull), out);
	sift_chunk_release_strs(&c);
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
