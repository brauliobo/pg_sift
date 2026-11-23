/*
 * col/f2/part_read.c - the bytes of an opened part of format major 2: its sections, its pages and the ranges of its stream (col/f2/part_format.h).
 */
#include "core/host.h"

#include "col/f2/part_format.h"
#include "core/f2/bulk_io.h"
#include "core/f2/extent.h"
#include "core/f2/io_stream.h"
#include "core/page_io.h"

#define PHDR(p)		((SiftPartHeaderData *) SiftPageGetData(p))

const SiftSecDirEntry *
sift_part_section(const SiftPart *part, uint16 tag)
{
	int			lo = 0,
				hi = part->hdr.nsec - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (part->secdir[mid].tag == tag)
			return &part->secdir[mid];
		if (part->secdir[mid].tag < tag)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* The block of page p of the stream */
BlockNumber
sift_part_page_block(const SiftPart *part, uint64 page)
{
	uint32		lo = 0,
				hi = part->hdr.total_extents - 1;

	while (lo < hi)
	{
		uint32		mid = (lo + hi + 1) / 2;

		if (part->pagestart[mid] <= page)
			lo = mid;
		else
			hi = mid - 1;
	}
	return part->extents[lo].start + (page - part->pagestart[lo]);
}

static bool
is_inline(const SiftPart *part)
{
	return (part->hdr.flags & SIFT_PART_FLAG_INLINE_STREAM) != 0;
}

static void
check_range(Relation rel, const SiftPart *part, uint64 off, uint64 len)
{
	uint64		limit = is_inline(part) ? part->inline_len : part->hdr.stream_bytes;

	if (off + len > limit)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "A read of %llu bytes at %llu is past the %llu bytes of the stream.", (unsigned long long) len, (unsigned long long) off,
						 (unsigned long long) limit);
}

/* The bytes of one page of the stream */
static void
read_in_page(Relation rel, const SiftPart *part, uint64 page, uint64 in_page, uint64 n, char *out)
{
	BlockNumber blk = sift_part_page_block(part, page);
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);

	sift_page_expect(rel, BufferGetPage(buf), blk, SIFT_PT_DATA, part->hdr.part_gen, part->hdr.part_id);
	memcpy(out, SiftPageGetData(BufferGetPage(buf)) + in_page, n);
	UnlockReleaseBuffer(buf);
}

void
sift_part_read_bytes(Relation rel, const SiftPart *part, uint64 off, uint64 len, void *dst)
{
	char	   *out = dst;
	uint64		first = SIFT_PAYLOAD_PAGE(off),
				last;
	SiftExtentReadStream ios;
	Buffer		buf;

	check_range(rel, part, off, len);
	if (len == 0)
		return;
	if (part->remote != NULL)
	{
		sift_tier_host_read(rel, part->remote, off, len, dst);
		return;
	}
	if (is_inline(part))
	{
		memcpy(dst, part->inline_payload + off, len);
		return;
	}
	last = SIFT_PAYLOAD_PAGE(off + len - 1);
	if (first == last)
	{
		read_in_page(rel, part, first, SIFT_PAYLOAD_OFF(off), len, out);
		return;
	}
	sift_ios_begin_range(&ios, rel, part->extents, part->pagestart, part->hdr.total_extents, first, last + 1, SIFT_PT_DATA, part->hdr.part_gen, part->hdr.part_id, part->io_mode);
	while (BufferIsValid(buf = sift_ios_next(&ios, NULL)))
	{
		uint64		in_page = SIFT_PAYLOAD_OFF(off),
					n = Min(len, SIFT_PAGE_PAYLOAD - in_page);

		memcpy(out, SiftPageGetData(BufferGetPage(buf)) + in_page, n);
		UnlockReleaseBuffer(buf);
		out += n;
		off += n;
		len -= n;
	}
	sift_ios_end(&ios);
}

static int
byte_range_cmp(const void *a, const void *b)
{
	uint64		x = ((const SiftByteRange *) a)->off,
				y = ((const SiftByteRange *) b)->off;

	return x < y ? -1 : x > y;
}

void
sift_part_read_ranges(Relation rel, const SiftPart *part, const SiftByteRange *ranges, int n, char *dst_base, SiftIoMode mode)
{
	SiftByteRange *sorted;
	uint64		cap = 0;
	SiftBlockReq *reqs;
	SiftCopySlice *slices;
	uint32		nreqs = 0,
				nslices = 0;

	if (is_inline(part) || part->remote != NULL)
	{
		for (int i = 0; i < n; i++)
			sift_part_read_bytes(rel, part, ranges[i].off, ranges[i].len, dst_base + ranges[i].dst_off);
		return;
	}
	sorted = palloc(Max(n, 1) * sizeof(SiftByteRange));
	memcpy(sorted, ranges, n * sizeof(SiftByteRange));
	qsort(sorted, n, sizeof(SiftByteRange), byte_range_cmp);
	for (int i = 0; i < n; i++)
	{
		check_range(rel, part, sorted[i].off, sorted[i].len);
		if (sorted[i].len > 0)
			cap += SIFT_PAYLOAD_PAGE(sorted[i].off + sorted[i].len - 1) - SIFT_PAYLOAD_PAGE(sorted[i].off) + 1;
	}
	reqs = palloc(Max(cap, 1) * sizeof(SiftBlockReq));
	slices = palloc(Max(cap, 1) * sizeof(SiftCopySlice));
	for (int i = 0; i < n; i++)
	{
		uint64		off = sorted[i].off,
					len = sorted[i].len;
		uint32		done = 0;

		while (len > 0)
		{
			uint64		page = SIFT_PAYLOAD_PAGE(off),
						in_page = SIFT_PAYLOAD_OFF(off),
						k = Min(len, SIFT_PAGE_PAYLOAD - in_page);

			if (nreqs == 0 || reqs[nreqs - 1].logical_page != page)
				reqs[nreqs++] = (SiftBlockReq) {.blkno = sift_part_page_block(part, page),.logical_page = (uint32) page,.slice_first = nslices};
			reqs[nreqs - 1].slice_count++;
			slices[nslices++] = (SiftCopySlice) {.page_off = in_page,.len = k,.dst_off = sorted[i].dst_off + done};
			off += k;
			len -= k;
			done += k;
		}
	}
	if (nreqs == 1)
	{
		Buffer		buf = sift_blk_read(rel, reqs[0].blkno, BUFFER_LOCK_SHARE);

		sift_page_expect(rel, BufferGetPage(buf), reqs[0].blkno, SIFT_PT_DATA, part->hdr.part_gen, part->hdr.part_id);
		for (uint32 s = 0; s < nslices; s++)
			memcpy(dst_base + slices[s].dst_off, SiftPageGetData(BufferGetPage(buf)) + slices[s].page_off, slices[s].len);
		UnlockReleaseBuffer(buf);
	}
	else if (nreqs > 1)
		sift_ios_exec_slices(rel, reqs, nreqs, slices, dst_base, SIFT_PT_DATA, part->hdr.part_gen, part->hdr.part_id, mode);
	pfree(sorted);
	pfree(reqs);
	pfree(slices);
}

void *
sift_part_read_section(Relation rel, const SiftPart *part, uint16 tag, uint64 *len)
{
	const SiftSecDirEntry *s = sift_part_section(part, tag);
	char	   *out;

	if (s == NULL)
		return NULL;
	out = palloc(Max(s->len, 1));
	if (s->flags & SIFT_SEC_FLAG_INLINE)
		memcpy(out, part->inline_payload + s->off, s->len);
	else
		sift_part_read_bytes(rel, part, s->off, s->len, out);
	if (!(s->flags & SIFT_SEC_FLAG_NO_CRC) && s->crc32c != sift_crc32c(out, s->len))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Section 0x%04x of the part does not match its checksum.", tag);
	*len = s->len;
	return out;
}

void
sift_part_read_section_bytes(Relation rel, const SiftPart *part, const SiftSecDirEntry *sec, uint64 off, uint64 len, void *dst)
{
	Assert(off + len <= sec->len);
	if (sec->flags & SIFT_SEC_FLAG_INLINE)
		memcpy(dst, part->inline_payload + sec->off + off, len);
	else
		sift_part_read_bytes(rel, part, sec->off + off, len, dst);
}

SiftExtent *
sift_part_blocks(const SiftPart *part, int *n)
{
	int			total = 1 + part->nextlist_pages + part->hdr.total_extents;
	/* check-memory: allow the extents of one part, which the size of the part bounds */
	SiftExtent *out = palloc(total * sizeof(SiftExtent));
	int			k = 0;

	if (part->is_packed || part->remote != NULL)
	{
		*n = 0;
		return out;
	}
	out[k++] = (SiftExtent) {part->header_blk, 1};
	for (int i = 0; i < part->nextlist_pages; i++)
		out[k++] = (SiftExtent) {part->extlist_pages[i], 1};
	memcpy(out + k, part->extents, part->hdr.total_extents * sizeof(SiftExtent));
	*n = total;
	return out;
}
