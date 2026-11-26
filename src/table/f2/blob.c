/*
 * table/f2/blob.c - the wide values of a sift_table (f2_blob.h): written when their stage is, read when a row asks for them, their extents given back with the rows that die.
 */
#include "postgres.h"

#include "col/codec/codec.h"
#include "col/compress.h"
#include "core/damage.h"
#include "core/extlock.h"
#include "core/f2/bulk_io.h"
#include "core/f2/meta.h"
#include "core/page_io.h"

#include "table/f2/tbl.h"

void
sift_ext_vec_add(SiftExtVec *v, const SiftExtent *e)
{
	if (v->n == v->cap)
	{
		v->cap = Max(16, v->cap * 2);
		v->ext = v->ext == NULL ? palloc(sizeof(SiftExtent) * v->cap) : repalloc(v->ext, sizeof(SiftExtent) * v->cap);
	}
	v->ext[v->n++] = *e;
}

static const char *
compressed(const char *raw, uint32 raw_len, uint32 *stored_len, uint16 *flags)
{
	Size		n,
				bound;
	char	   *buf;

	*stored_len = raw_len;
	if (!sift_colz_available(SIFT_CC_ALG_LZ4))
		return raw;
	bound = sift_colz_bound(SIFT_CC_ALG_LZ4, raw_len);
	if (bound == 0 || bound > MaxAllocSize)
		return raw;
	buf = palloc(bound);
	n = sift_colz_compress(SIFT_CC_ALG_LZ4, 0, raw, raw_len, buf, bound);
	if (n == 0 || n >= (Size) raw_len / 8 * 7)
	{
		pfree(buf);
		return raw;
	}
	*stored_len = (uint32) n;
	*flags |= SIFT_BLOB_FLAG_LZ4;
	return buf;
}

/* The pages of one value: those past the end of the file at the start of the spill go through the run, the reused ones through the buffers */
static void
write_pages(SiftBulkRun *run, SiftPartBulkWriter *w, const SiftTblBlobRef *ref, const char *stored)
{
	uint64		done = 0;

	for (int e = 0; e < ref->nextents; e++)
		for (uint32 b = 0; b < ref->ext[e].nblocks && done < ref->stored_len; b++)
		{
			BlockNumber blk = ref->ext[e].start + b;
			bool		fresh = run != NULL && sift_bulk_run_fresh(run, blk);
			Page		pg = fresh ? sift_bulk_run_page(run) : sift_bulk_page(w, blk);
			uint32		n = (uint32) Min(ref->stored_len - done, (uint64) SIFT_PAGE_PAYLOAD);

			sift_page_init_part(pg, SIFT_PT_DATA, ref->gen, 0);
			memcpy(SiftPageGetData(pg), stored + done, n);
			SiftPageSetUsed(pg, n);
			if (fresh)
				sift_bulk_run_write(run, blk, pg);
			done += n;
		}
}

static bool
is_inline(const SiftTblBlobRef *ref)
{
	return (ref->flags & SIFT_BLOB_FLAG_INLINE) != 0;
}

static uint32
ref_size(const SiftTblBlobRef *ref)
{
	return (uint32) SIFT_TBL_BLOB_REF_SIZE(ref->nextents) + (is_inline(ref) ? ref->stored_len : 0);
}

static SiftTblBlobRef *
new_ref(uint32 raw_len, const char *stored, uint32 stored_len, uint16 flags, uint16 next, uint64 gen)
{
	SiftTblBlobRef *ref = palloc0(SIFT_TBL_BLOB_REF_SIZE(next) + ((flags & SIFT_BLOB_FLAG_INLINE) ? stored_len : 0));

	ref->magic = SIFT_TBL_BLOB_MAGIC;
	ref->raw_len = raw_len;
	ref->stored_len = stored_len;
	ref->crc32c = sift_crc32c(stored, stored_len);
	ref->flags = flags;
	ref->nextents = next;
	ref->gen = gen;
	return ref;
}

/* A wide value of the stage on its way to its reference: compressed, then given its pages when it does not fit in the reference */
typedef struct WideValue
{
	int			col;
	int			row;
	uint64		gen;
	const char *stored;
	uint32		stored_len;
	uint16		flags;
	SiftTblBlobRef *ref;
} WideValue;

static bool
out_of_line(const WideValue *v)
{
	return v->stored_len > SIFT_TBL_WIDE_THRESHOLD;
}

/* The next npages blocks of the stream, from where the last value ended (*e, *off), as the extents of one value in out: how many */
static int
take_pages(const SiftExtent *ext, int *e, uint32 *off, uint64 npages, SiftExtent *out)
{
	int			n = 0;

	while (npages > 0)
	{
		uint32		k = (uint32) Min(npages, (uint64) (ext[*e].nblocks - *off));

		out[n++] = (SiftExtent) {.start = ext[*e].start + *off,.nblocks = k};
		npages -= k;
		*off += k;
		if (*off == ext[*e].nblocks)
		{
			(*e)++;
			*off = 0;
		}
	}
	return n;
}

/* The values that do not fit in their reference, in one stream of extents taken at once and cut in their order: one allocation and full batches of pages for the whole stage */
static void
write_values(Relation rel, const SiftRowBuf *rb, WideValue *vals, int nvals, uint64 npages)
{
	const int	max = (int) (npages / 1024) + 32;
	SiftExtent *ext = palloc(sizeof(SiftExtent) * max),
			   *cut = palloc(sizeof(SiftExtent) * max);
	const int	next = sift_fsm_alloc_stream(rel, npages, ext, max);
	SiftBulkRun *run = sift_bulk_run_begin(rel, ext, next);
	int			e = 0;
	uint32		off = 0;
	SiftPartBulkWriter w;

	PG_TRY();
	{
		if (run == NULL)
			sift_bulk_extend(rel, ext, next);
		sift_bulk_begin(&w, rel);
		for (WideValue *v = vals; v < vals + nvals; v++)
		{
			int			k;

			if (!out_of_line(v))
				continue;
			k = take_pages(ext, &e, &off, SIFT_PAYLOAD_PAGES(v->stored_len), cut);
			v->ref = new_ref(rb->xplen[v->col][v->row], v->stored, v->stored_len, v->flags, (uint16) k, v->gen);
			memcpy(v->ref->ext, cut, sizeof(SiftExtent) * k);
			write_pages(run, &w, v->ref, v->stored);
		}
		sift_bulk_end(&w);
		if (run != NULL)
			sift_bulk_run_end(run);
	}
	PG_CATCH();
	{
		if (run != NULL)
			sift_bulk_run_abort(run);
		PG_RE_THROW();
	}
	PG_END_TRY();
	pfree(ext);
	pfree(cut);
}

static uint32
count_wide(const SiftRowBuf *rb)
{
	uint32		n = 0;

	for (int c = 0; c < rb->ncols; c++)
		if (rb->tag[c] != NULL)
			for (int r = 0; r < rb->n; r++)
				n += rb->tag[c][r] == SIFT_COLTAG_WIDE_RAW;
	return n;
}

void
sift_tbl_blob_spill_rows(Relation rel, SiftRowBuf *rb)
{
	uint32		nwide = count_wide(rb);
	WideValue  *vals;
	uint64		gen,
				npages = 0;
	int			n = 0;

	if (nwide == 0)
		return;
	vals = palloc(sizeof(WideValue) * nwide);
	gen = sift_manifest_new_gen(rel, nwide);
	for (int c = 0; c < rb->ncols; c++)
		for (int r = 0; rb->tag[c] != NULL && r < rb->n; r++)
			if (rb->tag[c][r] == SIFT_COLTAG_WIDE_RAW)
			{
				WideValue  *v = &vals[n++];

				*v = (WideValue) {.col = c,.row = r,.gen = gen++};
				v->stored = compressed(rb->xpay[c][r], rb->xplen[c][r], &v->stored_len, &v->flags);
				npages += out_of_line(v) ? SIFT_PAYLOAD_PAGES(v->stored_len) : 0;
			}
	if (npages > 0)
		write_values(rel, rb, vals, n, npages);
	for (WideValue *v = vals; v < vals + n; v++)
	{
		if (!out_of_line(v))
		{
			v->ref = new_ref(rb->xplen[v->col][v->row], v->stored, v->stored_len, v->flags | SIFT_BLOB_FLAG_INLINE, 0, v->gen);
			memcpy(v->ref->ext, v->stored, v->stored_len);
		}
		if (v->stored != rb->xpay[v->col][v->row])
			pfree((char *) v->stored);
		rb->xpay[v->col][v->row] = (const char *) v->ref;
		rb->xplen[v->col][v->row] = ref_size(v->ref);
		rb->tag[v->col][v->row] = SIFT_COLTAG_WIDE_BLOB;
	}
	pfree(vals);
}

/* A copy of the reference in memory of its own alignment, whose lengths are those a writer makes: a value no longer than a palloc, stored as it is exactly when it is not compressed */
static SiftTblBlobRef *
checked_ref(Relation rel, const char *bytes, uint32 len)
{
	SiftTblBlobRef *ref;

	if (len < sizeof(SiftTblBlobRef))
		SIFT_DAMAGE_OTHER_DATA(errmsg("table \"%s\" holds a damaged reference to a wide value", RelationGetRelationName(rel)));
	ref = palloc(len);
	memcpy(ref, bytes, len);
	if (ref->magic != SIFT_TBL_BLOB_MAGIC || ref->raw_len == 0 || ref->raw_len > SIFT_TBL_BLOB_MAX || ref->stored_len == 0 || ref->stored_len > SIFT_TBL_BLOB_MAX ||
		(ref->flags & ~(SIFT_BLOB_FLAG_LZ4 | SIFT_BLOB_FLAG_INLINE)) != 0 || ((ref->flags & SIFT_BLOB_FLAG_LZ4) == 0 && ref->raw_len != ref->stored_len) ||
		len != ref_size(ref) || (is_inline(ref) && (ref->nextents != 0 || ref->stored_len > SIFT_TBL_WIDE_THRESHOLD)))
		SIFT_DAMAGE_OTHER_DATA(errmsg("table \"%s\" holds a damaged reference to a wide value", RelationGetRelationName(rel)));
	return ref;
}

static uint64
read_extents(Relation rel, const SiftTblBlobRef *ref, char *stored)
{
	uint64		done = 0;

	for (int e = 0; e < ref->nextents; e++)
		for (uint32 b = 0; b < ref->ext[e].nblocks && done < ref->stored_len; b++)
		{
			BlockNumber blk = ref->ext[e].start + b;
			Buffer		buf;
			uint32		n = (uint32) Min(ref->stored_len - done, (uint64) SIFT_PAGE_PAYLOAD);

			if (ref->ext[e].nblocks > InvalidBlockNumber - ref->ext[e].start)
				SIFT_DAMAGE_OTHER_DATA(errmsg("a wide value of table \"%s\" has an extent past the last block", RelationGetRelationName(rel)));
			buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
			sift_page_expect(rel, BufferGetPage(buf), blk, SIFT_PT_DATA, ref->gen, 0);
			if (SiftPageGetUsed(BufferGetPage(buf)) < n)
				SIFT_DAMAGE_OTHER_DATA(errmsg("a wide value of table \"%s\" is longer than its page", RelationGetRelationName(rel)));
			memcpy(stored + done, SiftPageGetData(BufferGetPage(buf)), n);
			UnlockReleaseBuffer(buf);
			done += n;
		}
	return done;
}

static char *
read_stored(Relation rel, const SiftTblBlobRef *ref)
{
	char	   *stored = palloc(ref->stored_len);
	uint64		done = ref->stored_len;

	if (is_inline(ref))
		memcpy(stored, ref->ext, ref->stored_len);
	else
		done = read_extents(rel, ref, stored);
	if (done != ref->stored_len || sift_crc32c(stored, ref->stored_len) != ref->crc32c)
		SIFT_DAMAGE_OTHER_DATA(errmsg("a wide value of table \"%s\" fails its checksum", RelationGetRelationName(rel)));
	return stored;
}

Datum
sift_tbl_blob_read(Relation rel, const char *ref_bytes, uint32 ref_len, Oid typid pg_attribute_unused(), int16 typlen pg_attribute_unused(), MemoryContext dst_cx)
{
	SiftTblBlobRef *ref = checked_ref(rel, ref_bytes, ref_len);
	char	   *stored = read_stored(rel, ref);
	char	   *raw = MemoryContextAlloc(dst_cx, ref->raw_len);

	if ((ref->flags & SIFT_BLOB_FLAG_LZ4) == 0)
		memcpy(raw, stored, ref->raw_len);
	else if (!sift_colz_decompress(SIFT_CC_ALG_LZ4, stored, ref->stored_len, raw, ref->raw_len))
		SIFT_DAMAGE_OTHER_DATA(errmsg("a wide value of table \"%s\" does not decompress", RelationGetRelationName(rel)));
	pfree(stored);
	pfree(ref);
	return PointerGetDatum(raw);
}

void
sift_tbl_blob_release_ref(Relation rel, const char *ref_bytes, uint32 ref_len, SiftExtVec *release)
{
	SiftTblBlobRef *ref = checked_ref(rel, ref_bytes, ref_len);

	for (int e = 0; e < ref->nextents; e++)
		sift_ext_vec_add(release, &ref->ext[e]);
	pfree(ref);
}

/* The columns are those the granule was written with: a column dropped since keeps its values' pages until the rows go, and one added since has no chunk */
void
sift_tbl_blob_reclaim(Relation rel, const SiftF2Store *store pg_attribute_unused(), const SiftGranuleView *v, const uint64 *dropped_mask, SiftExtVec *release)
{
	uint16		nrows = v->hdr->nrows;
	uint8	   *tag = palloc(nrows);

	for (int c = 0; c < v->hdr->nchunks - 1; c++)
	{
		if (v->hdr->chunks[c + 1].flags & SIFT_CHUNK_ELEMS)
			continue;
		sift_granule_tags(v, c, tag);
		for (uint16 r = 0; r < nrows; r++)
		{
			uint32		len;
			const char *bytes;

			if (tag[r] != SIFT_COLTAG_WIDE_BLOB || (dropped_mask[r / 64] & (UINT64CONST(1) << (r % 64))) == 0)
				continue;
			bytes = sift_granule_exc_payload(v, c, r, &len);
			sift_tbl_blob_release_ref(rel, bytes, len, release);
		}
	}
	pfree(tag);
}
