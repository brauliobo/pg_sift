/*
 * table/f2/f2_blob.c - the wide values of a sift_table (f2_blob.h): written when their stage is, read when a row asks for them, their extents given back with the rows that die.
 */
#include "postgres.h"

#include "col/codec.h"
#include "col/compress.h"
#include "core/damage.h"
#include "core/f2/bulk_io.h"
#include "core/f2/meta.h"
#include "core/page_io.h"

#include "table/f2/f2_tbl.h"

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
	Size		n;
	char	   *buf;

	*stored_len = raw_len;
	if (!sift_colz_available(SIFT_CC_ALG_LZ4))
		return raw;
	buf = palloc(sift_colz_bound(SIFT_CC_ALG_LZ4, raw_len));
	n = sift_colz_compress(SIFT_CC_ALG_LZ4, 0, raw, raw_len, buf, sift_colz_bound(SIFT_CC_ALG_LZ4, raw_len));
	if (n == 0 || n >= (Size) raw_len / 8 * 7)
	{
		pfree(buf);
		return raw;
	}
	*stored_len = (uint32) n;
	*flags |= SIFT_BLOB_FLAG_LZ4;
	return buf;
}

static void
write_pages(Relation rel, const SiftTblBlobRef *ref, const char *stored)
{
	SiftPartBulkWriter w;
	uint64		done = 0;

	sift_bulk_extend(rel, ref->ext, ref->nextents);
	sift_bulk_begin(&w, rel);
	for (int e = 0; e < ref->nextents; e++)
		for (uint32 b = 0; b < ref->ext[e].nblocks && done < ref->stored_len; b++)
		{
			Page		pg = sift_bulk_page(&w, ref->ext[e].start + b);
			uint32		n = (uint32) Min(ref->stored_len - done, (uint64) SIFT_PAGE_PAYLOAD);

			sift_page_init_part(pg, SIFT_PT_DATA, ref->gen, 0);
			memcpy(SiftPageGetData(pg), stored + done, n);
			SiftPageSetUsed(pg, n);
			done += n;
		}
	sift_bulk_end(&w);
}

static SiftTblBlobRef *
write_blob(Relation rel, const char *raw, uint32 raw_len, uint64 gen)
{
	uint16		flags = 0;
	uint32		stored_len;
	const char *stored = compressed(raw, raw_len, &stored_len, &flags);
	uint64		npages = SIFT_PAYLOAD_PAGES(stored_len);
	int			max = (int) (npages / 1024) + 32;
	SiftExtent *ext = palloc(sizeof(SiftExtent) * max);
	int			next = sift_fsm_alloc_stream(rel, npages, ext, max);
	SiftTblBlobRef *ref = palloc0(SIFT_TBL_BLOB_REF_SIZE(next));

	ref->magic = SIFT_TBL_BLOB_MAGIC;
	ref->raw_len = raw_len;
	ref->stored_len = stored_len;
	ref->crc32c = sift_crc32c(stored, stored_len);
	ref->flags = flags;
	ref->nextents = (uint16) next;
	ref->gen = gen;
	memcpy(ref->ext, ext, sizeof(SiftExtent) * next);
	write_pages(rel, ref, stored);
	if (stored != raw)
		pfree((char *) stored);
	pfree(ext);
	return ref;
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
	uint64		gen = nwide > 0 ? sift_manifest_new_gen(rel, nwide) : 0;

	for (int c = 0; c < rb->ncols; c++)
	{
		if (rb->tag[c] == NULL)
			continue;
		for (int r = 0; r < rb->n; r++)
		{
			SiftTblBlobRef *ref;

			if (rb->tag[c][r] != SIFT_COLTAG_WIDE_RAW)
				continue;
			ref = write_blob(rel, rb->xpay[c][r], rb->xplen[c][r], gen++);
			rb->xpay[c][r] = (const char *) ref;
			rb->xplen[c][r] = (uint32) SIFT_TBL_BLOB_REF_SIZE(ref->nextents);
			rb->tag[c][r] = SIFT_COLTAG_WIDE_BLOB;
		}
	}
}

static const SiftTblBlobRef *
checked_ref(Relation rel, const char *bytes, uint32 len)
{
	const SiftTblBlobRef *ref = (const SiftTblBlobRef *) bytes;

	if (len < sizeof(SiftTblBlobRef) || ref->magic != SIFT_TBL_BLOB_MAGIC || len != SIFT_TBL_BLOB_REF_SIZE(ref->nextents))
		SIFT_DAMAGE_OTHER_DATA(errmsg("table \"%s\" holds a damaged reference to a wide value", RelationGetRelationName(rel)));
	return ref;
}

static char *
read_pages(Relation rel, const SiftTblBlobRef *ref)
{
	char	   *stored = palloc(ref->stored_len);
	uint64		done = 0;

	for (int e = 0; e < ref->nextents; e++)
		for (uint32 b = 0; b < ref->ext[e].nblocks && done < ref->stored_len; b++)
		{
			BlockNumber blk = ref->ext[e].start + b;
			Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
			uint32		n = (uint32) Min(ref->stored_len - done, (uint64) SIFT_PAGE_PAYLOAD);

			sift_page_expect(rel, BufferGetPage(buf), blk, SIFT_PT_DATA, ref->gen, 0);
			memcpy(stored + done, SiftPageGetData(BufferGetPage(buf)), n);
			UnlockReleaseBuffer(buf);
			done += n;
		}
	if (done != ref->stored_len || sift_crc32c(stored, ref->stored_len) != ref->crc32c)
		SIFT_DAMAGE_OTHER_DATA(errmsg("a wide value of table \"%s\" fails its checksum", RelationGetRelationName(rel)));
	return stored;
}

Datum
sift_tbl_blob_read(Relation rel, const char *ref_bytes, uint32 ref_len, Oid typid pg_attribute_unused(), int16 typlen pg_attribute_unused(), MemoryContext dst_cx)
{
	const SiftTblBlobRef *ref = checked_ref(rel, ref_bytes, ref_len);
	char	   *stored = read_pages(rel, ref);
	char	   *raw = MemoryContextAlloc(dst_cx, ref->raw_len);

	if ((ref->flags & SIFT_BLOB_FLAG_LZ4) == 0)
		memcpy(raw, stored, ref->raw_len);
	else if (!sift_colz_decompress(SIFT_CC_ALG_LZ4, stored, ref->stored_len, raw, ref->raw_len))
		SIFT_DAMAGE_OTHER_DATA(errmsg("a wide value of table \"%s\" does not decompress", RelationGetRelationName(rel)));
	pfree(stored);
	return PointerGetDatum(raw);
}

void
sift_tbl_blob_release_ref(Relation rel, const char *ref_bytes, uint32 ref_len, SiftExtVec *release)
{
	const SiftTblBlobRef *ref = checked_ref(rel, ref_bytes, ref_len);

	for (int e = 0; e < ref->nextents; e++)
		sift_ext_vec_add(release, &ref->ext[e]);
}

void
sift_tbl_blob_reclaim(Relation rel, const SiftF2Store *store, const SiftGranuleView *v, const uint64 *dropped_mask, SiftExtVec *release)
{
	uint16		nrows = v->hdr->nrows;
	uint8	   *tag = palloc(nrows);

	for (int c = 0; c < store->ncols; c++)
	{
		if (store->kinds[c] != SIFT_CELL_STR)
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
