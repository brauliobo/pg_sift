/*
 * col/seg_write/mirror.c - the copies of small ranges of the stream that the header page of the segment keeps (seg_int.h "The mirror"): taken as the structures are
 * written, the most useful kept when the page has no room for them all
 */
#include "col/seg_write/seg_write_int.h"

void
segw_mirror(SiftColSegWriter *w, MirrorPrio prio, uint64 off, const void *data, Size len)
{
	MirrorPiece *p;

	if (sift_limit(mirror_bytes, SEG_MIRROR_BYTES) == 0 || len == 0 || len > SIFT_PAGE_PAYLOAD)
		return;
	if (w->nmirror == w->capmirror)
	{
		w->capmirror = Max(64, 2 * w->capmirror);
		w->mirror = w->mirror ? repalloc(w->mirror, w->capmirror * sizeof(MirrorPiece)) : MemoryContextAlloc(w->mcxt, w->capmirror * sizeof(MirrorPiece));
	}
	if (w->nmirror == 0)
	{
		MemoryContext old = MemoryContextSwitchTo(w->mcxt);

		initStringInfo(&w->mirror_bytes);
		MemoryContextSwitchTo(old);
	}
	p = &w->mirror[w->nmirror++];
	*p = (MirrorPiece) {off, (uint32) len, (uint32) w->mirror_bytes.len, prio};
	appendBinaryStringInfo(&w->mirror_bytes, data, len);
}

void
segw_write_mirrored(SiftColSegWriter *w, MirrorPrio prio, const void *data, Size len)
{
	segw_mirror(w, prio, sift_sw_tell(w->sw), data, len);
	sift_sw_write(w->sw, data, len);
}

/* the size of the block of the pieces up to prio: the header, a range for each run of adjoining pieces, their bytes each to 8 */
static Size
block_size(const SiftColSegWriter *w, MirrorPrio prio)
{
	Size		size = sizeof(MirrorHeader);
	uint64		end = 0;
	bool		open = false;

	for (int i = 0; i < w->nmirror; i++)
	{
		const MirrorPiece *p = &w->mirror[i];

		if (p->prio > prio)
			continue;
		if (!open || p->off != end)
			size += sizeof(MirrorRange);
		open = true;
		end = p->off + p->len;
		size += p->len;
	}
	return size + 8 * w->nmirror;
}

void *
segw_mirror_block(SiftColSegWriter *w, uint32 nextents, uint32 *len)
{
	Size		room = Min((Size) sift_limit(mirror_bytes, SEG_MIRROR_BYTES), SIFT_PAGE_PAYLOAD - offsetof(SiftSegHeaderData, extents) - nextents * sizeof(SiftExtent));
	MirrorPrio	prio = MIR_HEAD;
	MirrorHeader h = {MIRROR_MAGIC, 0, 0, 0};
	MirrorRange *r;
	uint8	   *blk;
	Size		at;
	uint64		end = 0;

	*len = 0;
	if (w->nmirror == 0)
		return NULL;
	while (prio > 0 && block_size(w, prio) > room)
		prio--;
	if (prio == 0)
		return NULL;
	blk = palloc0(block_size(w, prio) + 8);
	r = (MirrorRange *) (blk + sizeof(MirrorHeader));
	for (int i = 0; i < w->nmirror; i++)
		if (w->mirror[i].prio <= prio)
			h.nranges += (h.nranges == 0 || w->mirror[i].off != end) ? 1 : 0, end = w->mirror[i].off + w->mirror[i].len;
	at = ALIGN8(sizeof(MirrorHeader) + h.nranges * sizeof(MirrorRange));
	end = 0;
	h.nranges = 0;
	for (int i = 0; i < w->nmirror; i++)
	{
		const MirrorPiece *p = &w->mirror[i];

		if (p->prio > prio)
			continue;
		if (h.nranges == 0 || p->off != end)
		{
			at = ALIGN8(at);
			r[h.nranges++] = (MirrorRange) {p->off, 0, (uint32) at};
		}
		memcpy(blk + at, w->mirror_bytes.data + p->at, p->len);
		r[h.nranges - 1].len += p->len;
		at += p->len;
		end = p->off + p->len;
	}
	h.len = ALIGN8(at);
	h.sum = seg_mirror_sum(blk + sizeof(MirrorHeader), h.len - sizeof(MirrorHeader));
	memcpy(blk, &h, sizeof(h));
	*len = (uint32) h.len;
	return blk;
}
