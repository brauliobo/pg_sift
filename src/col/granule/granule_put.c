/*
 * col/granule/granule_put.c - the record of a granule of a part of format major 2 written from rows: its header, the table of its chunks and the chunks (col/granule.h). The chunks are
 * encoded by col/granule/granule_enc.c and enc_str.c; reading one back is col/granule/granule.c.
 *
 * A chunk is encoded alone (sift_granule_chunk_put) and the record is laid from the chunks (sift_granule_assemble): the chunks of a granule do not depend on each other, so the compute
 * threads of the backend encode them (col/f2/f2_encode_pool.c) and the record is assembled in order. Nothing here reaches the server but through core/par/arena.h and core/par/sbuf.h
 * (and the checksum), so that a thread of the pool runs it in a scope.
 */
#include "core/host.h"

#include "col/granule/granule_int.h"
#include "core/page_io.h"
#include "core/par/sbuf.h"

void
sift_granule_chunk_put(StringInfo body, SiftChunkRef *ref, const SiftGranuleIn *in, int c)
{
	SiftChunkOpts opt = {.tier = in->tier,.alg = in->compress_alg,.framed = in->framed};

	if (in->nrows < 1 || in->nrows > SIFT_GRANULE_ROWS)
		SIFT_FAIL_INVALID("sift: a granule holds 1 to %d rows, not %d", SIFT_GRANULE_ROWS, in->nrows);
	*ref = (SiftChunkRef) {0};
	sift_sb_init(body);
	if (c == SIFT_COL_TID_IDX)
	{
		sift_chunk_put_tids(body, in->tid, in->nrows, ref, &opt);
		return;
	}
	{
		const SiftColIn *col = &in->cols[c - 1];
		int			n = in->nrows;

		if (col->group1 > 0)
		{
			const SiftColIn *g = &in->cols[col->group1 - 1];

			if (col->group1 - 1 > SIFT_CHUNK_GROUP_MAX)
				SIFT_FAIL_LIMIT("Keep fewer arrays of objects in the paths of the index.",
								"sift: column %d holds the lengths of arrays whose elements are kept, past the %d columns a granule can name", col->group1 - 1, SIFT_CHUNK_GROUP_MAX);
			n = g->eoff[in->nrows] - g->eoff[0];
		}
		opt.dict = in->dicts != NULL ? in->dicts[c - 1] : NULL;
		opt.fsst = in->part_fsst != NULL ? in->part_fsst[c - 1] : NULL;
		opt.cdict = in->part_cdict != NULL ? in->part_cdict[c - 1] : NULL;
		sift_chunk_put(body, col, n, ref, &opt);
		if (col->group1 > 0)
			ref->flags |= SIFT_CHUNK_ELEMS | ((col->group1 - 1) << SIFT_CHUNK_GROUP_SHIFT);
	}
}

SiftGranuleDesc
sift_granule_assemble(StringInfo out, const SiftGranuleIn *in, const StringInfoData *bodies, const SiftChunkRef *refs)
{
	int			nchunks = in->ncols + 1;
	uint32		hdrlen = offsetof(SiftGranuleHeader, chunks) + nchunks * sizeof(SiftChunkRef);
	uint64		start = out->len;
	SiftGranuleHeader *h;
	SiftGranuleDesc d = {0};
	bool		nulls = false;

	Assert(start % 8 == 0);
	sift_sb_spaces(out, hdrlen);
	memset(out->data + start, 0, hdrlen);
	for (int c = 0; c < nchunks; c++)
	{
		SiftChunkRef ref = refs[c];

		nulls |= ref.nnonnull != (uint32) in->nrows;
		ref.off = out->len - start;
		ref.len = bodies[c].len;
		sift_sb_append(out, bodies[c].data, bodies[c].len);
		pad8(out);
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

SiftGranuleDesc
sift_granule_encode(StringInfo out, const SiftGranuleIn *in)
{
	int			nchunks = in->ncols + 1;
	StringInfoData *bodies = sift_alloc(nchunks * sizeof(StringInfoData));
	SiftChunkRef *refs = sift_alloc(nchunks * sizeof(SiftChunkRef));
	SiftGranuleDesc d;

	for (int c = 0; c < nchunks; c++)
		sift_granule_chunk_put(&bodies[c], &refs[c], in, c);
	d = sift_granule_assemble(out, in, bodies, refs);
	for (int c = 0; c < nchunks; c++)
		sift_free(bodies[c].data);
	sift_free(bodies);
	sift_free(refs);
	return d;
}
