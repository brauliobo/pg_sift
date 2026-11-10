/*
 * col/seg/mirror.c - the mirror of a segment: the copy in its header page of the small ranges of the stream that a plan and the first read of the segment open
 * (seg_int.h "The mirror"), taken in the place of a read of the stream, and checked against it by sift.col_verify
 */
#include "core/cmp.h"
#include "col/seg_int.h"

/* A multiplicative hash of the words of the block: a byte of it that is damaged changes the sum */
uint64
seg_mirror_sum(const void *data, Size len)
{
	const uint8 *p = data;
	uint64		h = UINT64CONST(0xcbf29ce484222325),
				w;

	for (; len >= sizeof(w); len -= sizeof(w), p += sizeof(w))
	{
		memcpy(&w, p, sizeof(w));
		h = (h ^ w) * UINT64CONST(0x9e3779b97f4a7c15);
		h ^= h >> 29;
	}
	w = 0;
	memcpy(&w, p, len);
	return (h ^ w) * UINT64CONST(0x9e3779b97f4a7c15);
}

/* The block follows the extents of the header page at any byte: its structures are read by copy */
static MirrorRange
mirror_range(const SegPriv *pv, uint32 i)
{
	MirrorRange r;

	memcpy(&r, pv->mirror + sizeof(MirrorHeader) + (Size) i * sizeof(r), sizeof(r));
	return r;
}

static MirrorHeader
mirror_header(const SegPriv *pv)
{
	MirrorHeader h;

	memcpy(&h, pv->mirror, sizeof(h));
	return h;
}

static void
mirror_check_sum(const SiftColSegment *seg)
{
	MirrorHeader h = mirror_header(PRIV(seg));

	if (h.sum != seg_mirror_sum(PRIV(seg)->mirror + sizeof(h), h.len - sizeof(h)))
		corrupt(seg, "The mirror in the header page (%llu bytes, %u ranges) is damaged.", ULL(h.len), h.nranges);
}

/* Finds the mirror in the copy of the header page (used bytes), checks it against the stream and keeps it for seg_read */
void
seg_mirror_attach(SiftColSegment *seg, Size used)
{
	SegPriv    *pv = PRIV(seg);
	const uint8 *blk = (const uint8 *) seg->hdr + SEG_HDRLEN(seg->hdr);
	Size		room = used > SEG_HDRLEN(seg->hdr) ? used - SEG_HDRLEN(seg->hdr) : 0;
	MirrorHeader h;
	uint64		size = (uint64) seg->sr.npages * SIFT_PAGE_PAYLOAD;

	if (room < sizeof(h))
		return;
	memcpy(&h, blk, sizeof(h));
	if (h.magic != MIRROR_MAGIC)
		return;
	if (h.len > room || h.len < sizeof(h) + (uint64) h.nranges * sizeof(MirrorRange))
		corrupt(seg, "The mirror in the header page (%llu bytes, %u ranges) is damaged.", ULL(h.len), h.nranges);
	pv->mirror = blk;
	for (uint32 i = 0; i < h.nranges; i++)
	{
		MirrorRange r = mirror_range(pv, i),
					prev = i > 0 ? mirror_range(pv, i - 1) : (MirrorRange) {0, 0, 0};

		if (r.len == 0 || (uint64) r.at + r.len > h.len || r.off + r.len > size || r.off < prev.off + prev.len)
			corrupt(seg, "Range %u of the mirror in the header page lies outside the stream or the block.", i);
	}
}

/* Is [off, off + len) whole in a range of the mirror: then dst has its bytes */
bool
seg_mirror_copy(const SiftColSegment *seg, uint64 off, Size len, void *dst)
{
	const SegPriv *pv = PRIV(seg);
	uint32		lo = 0,
				hi;
	MirrorRange r = {0};

	if (pv->mirror == NULL)
		return false;
	hi = mirror_header(pv).nranges;
	while (lo < hi)
	{
		uint32		mid = (lo + hi) / 2;

		r = mirror_range(pv, mid);
		if (r.off + r.len <= off)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == mirror_header(pv).nranges)
		return false;
	r = mirror_range(pv, lo);
	if (r.off > off || off + len > r.off + r.len)
		return false;
	/* the checksum is checked when the mirror is first taken from: a statement whose segments are open from the backend's caches never pays it */
	if (!pv->mirror_sound)
	{
		mirror_check_sum(seg);
		((SegPriv *) pv)->mirror_sound = true;
		if (pv->head != NULL)
			pv->head->mirror_sound = true;
	}
	memcpy(dst, pv->mirror + r.at + (off - r.off), len);
	return true;
}

/* The mirror is what the stream holds at its ranges (the stream is read, the mirror is not used for it) */
void
sift_cseg_verify_mirror(SiftColSegment *seg)
{
	const SegPriv *pv = PRIV(seg);
	uint8	   *buf = NULL;
	Size		cap = 0;

	if (pv->mirror == NULL)
		return;
	mirror_check_sum(seg);
	for (uint32 i = 0; i < mirror_header(pv).nranges; i++)
	{
		MirrorRange r = mirror_range(pv, i);

		if (r.len > cap)
			buf = buf ? repalloc(buf, cap = r.len) : palloc(cap = r.len);
		seg_read_stream(seg, r.off, r.len, buf);
		if (memcmp(buf, pv->mirror + r.at, r.len) != 0)
			corrupt(seg, "The mirror of the header page differs from the stream at %llu (%u bytes).", ULL(r.off), r.len);
	}
}

/* The mirror as the header page of the segment would keep it with nextents extents, NULL when there is none or it no longer fits (the segment is then read from its stream) */
const void *
sift_cseg_mirror_for(const SiftColSegment *seg, uint32 nextents, uint32 *len)
{
	const SegPriv *pv = PRIV(seg);

	*len = 0;
	if (pv->mirror == NULL)
		return NULL;
	*len = (uint32) mirror_header(pv).len;
	if (offsetof(SiftSegHeaderData, extents) + nextents * sizeof(SiftExtent) + *len > SIFT_PAGE_PAYLOAD)
	{
		*len = 0;
		return NULL;
	}
	return pv->mirror;
}

#ifdef SIFT_TESTING
/* tests: range i of the mirror (stream offset and length), and the bytes of the whole block; false past the last range or without a mirror */
bool
sift_cseg_mirror_range(const SiftColSegment *seg, uint32 i, uint64 *off, uint32 *len, uint32 *block)
{
	const SegPriv *pv = PRIV(seg);
	MirrorRange r;

	if (pv->mirror == NULL || i >= mirror_header(pv).nranges)
		return false;
	r = mirror_range(pv, i);
	*off = r.off;
	*len = r.len;
	*block = (uint32) mirror_header(pv).len;
	return true;
}
#endif
