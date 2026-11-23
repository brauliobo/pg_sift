/*
 * col/granule.c - encode and decode the granules of a part of format major 2, and write a part from rows (col/granule.h). The encoders of the chunks are in col/granule_enc.c,
 * their decoders in col/granule_dec.c and the vector decoder in col/granule_vec.c; the parts built and read from granules are in col/granule/.
 */
#include "core/host.h"

#include "access/transam.h"
#include "lib/stringinfo.h"

#include "col/granule/granule_int.h"
#include "col/f2/preagg_delta.h"
#include "core/cmp.h"
#include "core/page_io.h"

SiftGranuleDesc
sift_granule_encode(StringInfo out, const SiftGranuleIn *in)
{
	int			nchunks = in->ncols + 1;
	uint32		hdrlen = offsetof(SiftGranuleHeader, chunks) + nchunks * sizeof(SiftChunkRef);
	uint64		start = out->len;
	SiftGranuleHeader *h;
	SiftGranuleDesc d = {0};
	bool		nulls = false;
	SiftChunkOpts opt = {.tier = in->tier,.alg = in->compress_alg};

	if (in->nrows < 1 || in->nrows > SIFT_GRANULE_ROWS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: a granule holds 1 to %d rows, not %d", SIFT_GRANULE_ROWS, in->nrows)));
	Assert(start % 8 == 0);
	appendStringInfoSpaces(out, hdrlen);
	memset(out->data + start, 0, hdrlen);
	for (int c = 0; c < nchunks; c++)
	{
		SiftChunkRef ref = {0};
		StringInfoData body;

		initStringInfo(&body);
		if (c == SIFT_COL_TID_IDX)
			sift_chunk_put_tids(&body, in->tid, in->nrows, &ref, &opt);
		else
		{
			const SiftColIn *col = &in->cols[c - 1];
			int			n = in->nrows;

			if (col->group1 > 0)
			{
				const SiftColIn *g = &in->cols[col->group1 - 1];

				if (col->group1 - 1 > SIFT_CHUNK_GROUP_MAX)
					ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: column %d holds the lengths of arrays whose elements are kept, past the %d columns a granule can name", col->group1 - 1, SIFT_CHUNK_GROUP_MAX),
							errhint("Keep fewer arrays of objects in the paths of the index.")));
				n = g->eoff[in->nrows] - g->eoff[0];
			}
			opt.dict = in->dicts != NULL ? in->dicts[c - 1] : NULL;
			opt.fsst = in->part_fsst != NULL ? in->part_fsst[c - 1] : NULL;
			opt.cdict = in->part_cdict != NULL ? in->part_cdict[c - 1] : NULL;
				sift_chunk_put(&body, col, n, &ref, &opt);
			if (col->group1 > 0)
				ref.flags |= SIFT_CHUNK_ELEMS | ((col->group1 - 1) << SIFT_CHUNK_GROUP_SHIFT);
		}
		nulls |= ref.nnonnull != (uint32) in->nrows;
		ref.off = out->len - start;
		ref.len = body.len;
		appendBinaryStringInfo(out, body.data, body.len);
		pad8(out);
		pfree(body.data);
		memcpy(out->data + start + offsetof(SiftGranuleHeader, chunks) + c * sizeof(SiftChunkRef), &ref, sizeof(ref));
	}
	h = (SiftGranuleHeader *) (out->data + start);
	h->magic = SIFT_GRANULE_MAGIC;
	h->nrows = in->nrows;
	h->nchunks = nchunks;
	d.data_off = start;
	d.data_len = out->len - start;
	d.nrows = in->nrows;
	d.flags = (in->all_frozen ? SIFT_GDESC_ALL_FROZEN : 0) | (nulls ? SIFT_GDESC_HAS_NULLS : 0);
	d.crc32c = sift_crc32c(h, hdrlen);
	d.min_xid = in->min_xid;
	d.max_xid = in->max_xid;
	d.tid_min = d.tid_max = in->tid[0];
	for (int i = 1; i < in->nrows; i++)
	{
		d.tid_min = Min(d.tid_min, in->tid[i]);
		d.tid_max = Max(d.tid_max, in->tid[i]);
	}
	d.row_base = in->row_base;
	return d;
}

SiftGranuleView
sift_granule_open(Relation rel, const char *rec, uint32 len, const SiftGranuleDesc *desc)
{
	const SiftGranuleHeader *h = (const SiftGranuleHeader *) rec;
	SiftGranuleView v = {.rec = rec,.len = len,.hdr = h};
	uint32		hdrlen;

	if (len < sizeof(SiftGranuleHeader) || h->magic != SIFT_GRANULE_MAGIC || h->nrows == 0 || h->nrows > SIFT_GRANULE_ROWS || (desc != NULL && h->nrows != desc->nrows))
		SIFT_CORRUPT(rel, "A granule does not start with its header.");
	hdrlen = offsetof(SiftGranuleHeader, chunks) + h->nchunks * sizeof(SiftChunkRef);
	if (hdrlen > len || (desc != NULL && sift_crc32c(rec, hdrlen) != desc->crc32c))
		SIFT_CORRUPT(rel, "The header or the chunk table of a granule does not match its checksum.");
	for (int c = 0; c < h->nchunks; c++)
		if (h->chunks[c].off % 8 != 0 || (uint64) h->chunks[c].off + h->chunks[c].len > len || h->chunks[c].off < hdrlen)
			SIFT_CORRUPT(rel, "Chunk %d of a granule lies outside it.", c);
	return v;
}

SiftGranuleView
sift_granule_open_part(Relation rel, const SiftPart *part, const char *rec, uint32 len, const SiftGranuleDesc *desc)
{
	SiftGranuleView v = sift_granule_open(rel, rec, len, desc);

	v.dicts = sift_part_dicts(rel, part);
	return v;
}

void
sift_granule_tids(const SiftGranuleView *v, uint64 *out)
{
	SiftChunk	c;

	sift_chunk_open(v, SIFT_COL_TID_IDX, &c);
	sift_chunk_tids(&c, v->hdr->nrows, out);
	sift_chunk_close(&c);
}

static void
col_open(const SiftGranuleView *v, int col, SiftChunk *c)
{
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	sift_chunk_open(v, col + 1, c);
}

void
sift_granule_ints(const SiftGranuleView *v, int col, int64 *out, bool *isnull)
{
	SiftChunk	c;
	int			n,
				k = 0;
	int64	   *vals;

	col_open(v, col, &c);
	n = c.nrows;
	vals = palloc(Max(c.nnonnull, 1) * sizeof(int64));
	sift_chunk_ints(&c, 0, c.nnonnull, vals);
	for (int i = 0; i < n; i++)
	{
		isnull[i] = !sift_chunk_row_valid(&c, i);
		out[i] = isnull[i] ? 0 : vals[k++];
	}
	pfree(vals);
	sift_chunk_close(&c);
}

/* The nonnull index of each of the ascending rows that holds a value; false in isnull[j] for it, true for a row that holds none; returns how many hold one */
static int
valid_ranks(const SiftChunk *c, const uint16 *rows, uint16 nrows, uint32 *idx, bool *isnull)
{
	int			word = 0,
				rank = 0,
				m = 0;

	for (int j = 0; j < nrows; j++)
	{
		isnull[j] = !sift_chunk_row_valid(c, rows[j]);
		if (!isnull[j])
			idx[m++] = (uint32) sift_chunk_rank_advance(c, &word, &rank, rows[j]);
	}
	return m;
}

void
sift_granule_point_ints(const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, int64 *out, bool *isnull)
{
	SiftChunk	c;
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	uint32	   *idx = palloc(Max(nrows, 1) * sizeof(uint32));
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	int64	   *vals = palloc(Max(nrows, 1) * sizeof(int64));
	int			m;

	col_open(v, col, &c);
	m = valid_ranks(&c, rows, nrows, idx, isnull);
	sift_chunk_ints_sel(&c, idx, m, vals);
	for (int j = 0, k = 0; j < nrows; j++)
		out[j] = isnull[j] ? 0 : vals[k++];
	pfree(idx);
	pfree(vals);
	sift_chunk_close(&c);
}

void
sift_granule_point_f8(const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, double *out, bool *isnull)
{
	SiftChunk	c;
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	uint32	   *idx = palloc(Max(nrows, 1) * sizeof(uint32));
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	double	   *vals = palloc(Max(nrows, 1) * sizeof(double));
	int			m;

	col_open(v, col, &c);
	m = valid_ranks(&c, rows, nrows, idx, isnull);
	sift_chunk_f8_sel(&c, idx, m, vals);
	for (int j = 0, k = 0; j < nrows; j++)
		out[j] = isnull[j] ? 0 : vals[k++];
	pfree(idx);
	pfree(vals);
	sift_chunk_close(&c);
}

void
sift_granule_f8(const SiftGranuleView *v, int col, double *out, bool *isnull)
{
	SiftChunk	c;
	int			n,
				k = 0;
	double	   *vals;

	col_open(v, col, &c);
	n = c.nrows;
	vals = palloc(Max(c.nnonnull, 1) * sizeof(double));
	sift_chunk_f8(&c, 0, c.nnonnull, vals);
	for (int i = 0; i < n; i++)
	{
		isnull[i] = !sift_chunk_row_valid(&c, i);
		out[i] = isnull[i] ? 0 : vals[k++];
	}
	pfree(vals);
	sift_chunk_close(&c);
}

void
sift_granule_strs(const SiftGranuleView *v, int col, SiftStrCell *out)
{
	SiftChunk	c;
	int			n,
				k = 0;
	const char **p;
	uint32	   *len;

	col_open(v, col, &c);
	n = c.nrows;
	p = palloc(Max(c.nnonnull, 1) * sizeof(char *));
	len = palloc(Max(c.nnonnull, 1) * sizeof(uint32));
	sift_chunk_strs(&c, 0, c.nnonnull, p, len);
	for (int i = 0; i < n; i++)
	{
		out[i].isnull = !sift_chunk_row_valid(&c, i);
		out[i].p = out[i].isnull ? NULL : p[k];
		out[i].len = out[i].isnull ? 0 : len[k];
		k += !out[i].isnull;
	}
	pfree(p);
	pfree(len);
	sift_chunk_release_strs(&c);
}

void
sift_granule_dict_codes(const SiftGranuleView *v, int col, const uint32 *map, uint32 nmap, int64 *out, bool *isnull)
{
	SiftChunk	c;
	int64	   *codes;
	int			k = 0;

	col_open(v, col, &c);
	if (c.enc != SIFT_CENC_ALL_NULL && !(c.enc == SIFT_CENC_DICT_CODE && (c.ref->enc & SIFT_ENC_PART_DICT)))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: column %d is not coded by the dictionary of its part", col));
	codes = palloc(Max(c.nnonnull, 1) * sizeof(int64));
	if (c.enc == SIFT_CENC_DICT_CODE)
		sift_cc_blob_ints(c.body, c.body_len, c.nnonnull, 0, c.nnonnull, codes);
	for (int i = 0; i < c.nrows; i++)
	{
		isnull[i] = !sift_chunk_row_valid(&c, i);
		if (isnull[i])
			out[i] = 0;
		else if ((uint64) codes[k] >= nmap)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a dictionary code is past its dictionary"));
		else
			out[i] = map[codes[k++]];
	}
	pfree(codes);
	sift_chunk_close(&c);
}

void
sift_granule_tags(const SiftGranuleView *v, int col, uint8 *tag)
{
	SiftChunk	c;

	col_open(v, col, &c);
	memset(tag, 0, c.nrows);
	if (c.exc != NULL)
	{
		const SiftVariantExcEntry *e = sift_exc_entries(c.exc);

		for (int i = 0; i < c.exc->n_entries; i++)
			if (e[i].row_idx < c.nrows)
				tag[e[i].row_idx] = e[i].reserved;
	}
	sift_chunk_close(&c);
}

int
sift_granule_chunk_len(const SiftGranuleView *v, int col)
{
	SiftChunk	c;
	int			n;

	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	if (!(v->hdr->chunks[col + 1].flags & SIFT_CHUNK_ELEMS))
		return v->hdr->nrows;
	sift_chunk_open(v, col + 1, &c);
	n = c.nrows;
	sift_chunk_close(&c);
	return n;
}

static const uint64 *
exc_mask_of(const SiftExcHeader *h)
{
	return (h->flags & SIFT_EXC_MASKED) ? (const uint64 *) ((const char *) h + sizeof(SiftExcHeader)) : NULL;
}

static bool
exc_row_maybe(const uint64 *mask, uint32 row)
{
	return mask == NULL || (row < SIFT_GRANULE_ROWS && ((mask[row >> 6] >> (row & 63)) & 1));
}

void
sift_exc_reader_open(const SiftGranuleView *v, int col, SiftExcReader *er)
{
	*er = (SiftExcReader) {0};
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	if (!(v->hdr->chunks[col + 1].flags & (SIFT_CHUNK_HAS_OVERFLOW | SIFT_CHUNK_HAS_KINDS)))
		return;
	sift_chunk_open(v, col + 1, &er->chunk);
	er->open = true;
	if (er->chunk.exc == NULL)
		return;
	er->entries = sift_exc_entries(er->chunk.exc);
	er->n_entries = er->chunk.exc->n_entries;
	er->payloads = (const char *) (er->entries + er->n_entries + 1);
	er->mask = exc_mask_of(er->chunk.exc);
}

/* The entry of a row, found from the last one the reader found (a gallop ahead of it, a bisection behind it), or NULL */
static const SiftVariantExcEntry *
exc_seek(SiftExcReader *er, uint32 row)
{
	const SiftVariantExcEntry *e = er->entries;
	uint32		n = er->n_entries,
				lo = 0,
				hi = n;

	if (e == NULL || n == 0 || !exc_row_maybe(er->mask, row))
		return NULL;
	if (er->cursor < n && e[er->cursor].row_idx < row)
	{
		uint32		step = 1;

		for (lo = er->cursor + 1; lo + step - 1 < n && e[lo + step - 1].row_idx < row; step <<= 1)
			lo += step;
		hi = Min(n, lo + step);
	}
	else if (er->cursor < n)
		hi = er->cursor + 1;
	SIFT_BISECT(uint32, m, lo, hi, e[m].row_idx < row);
	er->cursor = lo;
	if (lo >= n || e[lo].row_idx != row)
		return NULL;
	er->cursor = lo + 1;
	return &e[lo];
}

const char *
sift_exc_reader_at(SiftExcReader *er, uint32 row, uint32 *len)
{
	const SiftVariantExcEntry *e = exc_seek(er, row);

	*len = e == NULL ? 0 : e[1].payload_off - e[0].payload_off;
	return *len == 0 ? NULL : er->payloads + e->payload_off;
}

uint8
sift_exc_reader_tag(SiftExcReader *er, uint32 row)
{
	const SiftVariantExcEntry *e = exc_seek(er, row);

	return e == NULL ? 0 : e->reserved;
}

void
sift_exc_reader_close(SiftExcReader *er)
{
	if (er->open)
		sift_chunk_close(&er->chunk);
	*er = (SiftExcReader) {0};
}

/* The exceptions of a chunk stored uncompressed, read in place of the record: no allocation and no decompression */
static const char *
exc_payload_stored(const SiftGranuleView *v, const SiftChunkRef *ref, int row, uint32 *len)
{
	uint32		vlen = sift_chunk_validity_len(ref, v->hdr->nrows);
	const SiftExcHeader *h = (const SiftExcHeader *) (v->rec + ref->off + vlen);
	const SiftVariantExcEntry *e = sift_exc_entries(h);
	uint32		lo = 0,
				hi;

	if ((uint64) vlen + sizeof(SiftExcHeader) > ref->len || h->n_entries > SIFT_GRANULE_ROWS || (uint64) vlen + SIFT_EXC_BLOCK_LEN(h) > ref->len)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the exceptions of a chunk do not fit it"));
	hi = h->n_entries;
	if ((h->flags & SIFT_EXC_KINDS_PACKED) || !exc_row_maybe(exc_mask_of(h), row))
		return NULL;
	SIFT_BISECT(uint32, m, lo, hi, e[m].row_idx < (uint32) row);
	if (lo >= h->n_entries || e[lo].row_idx != row)
		return NULL;
	*len = e[lo + 1].payload_off - e[lo].payload_off;
	return *len == 0 ? NULL : (const char *) (e + h->n_entries + 1) + e[lo].payload_off;
}

const char *
sift_granule_exc_payload(const SiftGranuleView *v, int col, int i, uint32 *len)
{
	const SiftChunkRef *ref;
	SiftExcReader er;
	const char *p;

	*len = 0;
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	ref = &v->hdr->chunks[col + 1];
	if (!(ref->flags & (SIFT_CHUNK_HAS_OVERFLOW | SIFT_CHUNK_HAS_KINDS)))
		return NULL;
	if (!(ref->flags & (SIFT_CHUNK_COMPRESSED | SIFT_CHUNK_ELEMS)))
		return exc_payload_stored(v, ref, i, len);
	sift_exc_reader_open(v, col, &er);
	p = sift_exc_reader_at(&er, i, len);
	if (p != NULL && (er.chunk.owned & SIFT_CHUNK_OWN_RAW))
	{
		char	   *copy = palloc(*len);

		memcpy(copy, p, *len);
		p = copy;
	}
	sift_exc_reader_close(&er);
	return p;
}

/* Writing a part from rows */

FullTransactionId
sift_xid_to_fxid(TransactionId xid)
{
	if (!TransactionIdIsNormal(xid))
		return FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);
	return FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), xid);
}

SiftColIn
sift_colin_slice(const SiftColIn *c, int from)
{
	SiftColIn s = *c;

	s.i = c->i ? c->i + from : NULL;
	s.f = c->f ? c->f + from : NULL;
	s.s = c->s ? c->s + from : NULL;
	s.slen = c->slen ? c->slen + from : NULL;
	s.dcode = c->dcode ? c->dcode + from : NULL;
	s.isnull = c->isnull ? c->isnull + from : NULL;
	s.tag = c->tag ? c->tag + from : NULL;
	s.eoff = c->eoff ? c->eoff + from : NULL;
	s.xpay = c->xpay ? c->xpay + from : NULL;
	s.xplen = c->xplen ? c->xplen + from : NULL;
	return s;
}
