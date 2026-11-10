/*
 * col/seg/gorder_head.c - the header and the chunk directory of a global order block: opened by the 32 byte header, the directory read whole when a seek or a cursor first needs it, the
 * entry of a chunk checked as it is used
 */
#include "core/cmp.h"
#include "col/seg_int.h"

/* Does the segment have a block for the slot col? The point directory's records say so: the block is not read */
bool
seg_gord_present(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);

	seg_points_read(seg);
	return pv->go != NULL && pv->go[col].bytes != 0;
}

/* The block of leaf col with its header read (the directory is read when a seek or a cursor needs it, and each entry is checked when it is used, the whole directory by verify), NULL when the segment has none */
GOrdLeaf *
seg_gord_leaf(SiftColSegment *seg, int col)
{
	GOrdLeaf   *g;
	SiftCGOrdHdr h;
	uint8	   *raw;
	MemoryContext cx;
	SegGOrdHead *kept;

	if (!seg_gord_present(seg, col))
		return NULL;
	g = &PRIV(seg)->go[col];
	if (g->dir != NULL)
		return g;
	if (HEAD_CACHED() && (kept = sift_cseg_gordh_find(seg, col)) != NULL)
	{
		g->hdr = kept->hdr;
		g->gat = kept->gat;
		g->dir = kept->dir;
		g->ents = kept->ents;
		g->head = kept;
		return g;
	}
	cx = AllocSetContextCreate(seg->mcxt, "sift cache segment order head", ALLOCSET_SMALL_SIZES);
	raw = MemoryContextAlloc(cx, SIFT_CGORD_HDR);
	sift_sr_point(&seg->sr);
	seg_read(seg, g->at, SIFT_CGORD_HDR, raw);
	if (!sift_cgord_head(raw, g->bytes, &h) || h.entries != g->hdr.entries || h.values != g->hdr.values || h.chunks != g->hdr.chunks ||
		(h.gcol >= 0 && h.gcol >= seg->schema->ncols))
		corrupt(seg, "The global order of leaf %d has a malformed header.", col);
	g->hdr = h;
	g->dir = raw;
	g->head = MemoryContextAllocZero(cx, sizeof(SegGOrdHead));
	*g->head = (SegGOrdHead) {h, 0, raw, cx, NULL};
	if (HEAD_CACHED())
		sift_cseg_gordh_keep(seg, col, cx, g->head);
	return g;
}

/* Where the chunks of the block end, which is where its groups or strings begin: after the last chunk, which the last directory entry says, and the block ends with them */
uint64
seg_gord_gat(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	if (g->gat == 0)
	{
		uint64		gat = SIFT_CGORD_HDR;

		if (g->hdr.chunks > 0)
		{
			SiftCGOrdDir last = seg_gord_dirent(seg, g, col, g->hdr.chunks - 1);

			gat = (uint64) last.off + ALIGN8((uint64) last.klen) + ALIGN8((uint64) last.alen) + SIFT_CGORD_ANCHOR_BYTES(SIFT_CGORD_ANCHORS(last.pad));
		}
		if (gat + (g->hdr.strbytes != 0 ? (uint64) g->hdr.strbytes : (uint64) g->hdr.ngroups * SIFT_CGORD_GRPENT) != g->bytes)
			corrupt(seg, "The global order of leaf %d ends at %llu of %llu.", col, (unsigned long long) gat, (unsigned long long) g->bytes);
		g->gat = gat;
		g->head->gat = gat;
	}
	return g->gat;
}

/* The directory, whole: it is kept with the header for the backend's later statements; a seek bisects it and a cursor looks up the entry of its chunk in it, where an entry apiece would be a pin of the buffers each */
void
seg_gord_dir_read(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	Size		len = (Size) g->hdr.chunks * SIFT_CGORD_DIRENT;

	if (g->ents != NULL)
		return;
	if (g->head->ents == NULL)
	{
		g->head->ents = MemoryContextAlloc(g->head->cx, Max(len, 1));
		sift_sr_point(&seg->sr);
		seg_read(seg, g->at + SIFT_CGORD_HDR, len, g->head->ents);
		if (HEAD_CACHED())
			sift_cseg_gordh_grew(seg, col, len);
	}
	g->ents = g->head->ents;
}

/* the directory entry of chunk c, checked against the block */
SiftCGOrdDir
seg_gord_dirent(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c)
{
	SiftCGOrdDir d;
	uint8		one[SIFT_CGORD_DIRENT];
	const uint8 *ent = one;

	if (c >= g->hdr.chunks)
		corrupt(seg, "The global order of leaf %d has a malformed directory entry %u.", col, c);
	if (g->ents != NULL || (Size) g->hdr.chunks * SIFT_CGORD_DIRENT <= SIFT_CGORD_DIR_EAGER)
	{
		seg_gord_dir_read(seg, g, col);
		ent = g->ents + (uint64) c * SIFT_CGORD_DIRENT;
	}
	else
	{
		sift_sr_point(&seg->sr);
		seg_read(seg, g->at + SIFT_CGORD_HDR + (uint64) c * SIFT_CGORD_DIRENT, SIFT_CGORD_DIRENT, one);
	}
	if (!sift_cgord_dirent(ent, &g->hdr, g->bytes, c, &d))
		corrupt(seg, "The global order of leaf %d has a malformed directory entry %u.", col, c);
	return d;
}
