/*
 * col/f2/f2_pack.c - the maintenance step that packs tiny parts. A part whose every section is inline (SIFT_PART_FLAG_INLINE_STREAM) is a page of its own however few bytes it has; the
 * parts of level 1 and up that are at most SIFT_PACKED_SLOT_MAX_BYTES are written together as slots of sealed SIFT_PT_PACKED_PART pages, and the descriptor of each is swapped in the manifest to
 * name its slot, which releases the page the part had. A packed page with less than half of its bytes in use is packed again with the others. A page leaves with the commit that drops or swaps
 * its last part (sift_packed_unreferenced), so that no page is released while a descriptor names it.
 */
#include "core/host.h"

#include "storage/lmgr.h"

#include "col/f2/compact.h"
#include "col/f2/part_format.h"
#include "core/f2/fsm.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "utils/memutils.h"

typedef struct PackCand
{
	uint64		part_id;
	uint8		key_lo[SIFT_BOUND_BYTES];
	SiftPartDesc d;
	uint16		len;
	BlockNumber from;			/* the packed page the part is a slot of, InvalidBlockNumber: it has a page of its own */
	int			bin;
} PackCand;

typedef struct PackBin
{
	uint32		used;
	int			n;
} PackBin;

static int
cand_len_desc(const void *a, const void *b)
{
	return (int) ((const PackCand *) b)->len - (int) ((const PackCand *) a)->len;
}

static int
cand_bin_cmp(const void *a, const void *b)
{
	int			d = ((const PackCand *) a)->bin - ((const PackCand *) b)->bin;

	return d != 0 ? d : cand_len_desc(a, b);
}

static int
cand_from_cmp(const void *a, const void *b)
{
	BlockNumber x = ((const PackCand *) a)->from,
				y = ((const PackCand *) b)->from;

	return x < y ? -1 : x > y;
}

static void
survey_part(Relation rel, const SiftPartitionEntry *e, const SiftPartDesc *d, PackCand **cands, int *nc, int *cap)
{
	bool		packed = (d->flags & SIFT_PART_FLAG_PACKED_SLOT) != 0;
	uint32		len = 0;

	if ((d->ext_flags & SIFT_PART_EXT_REMOTE) || (!packed && (d->level < 1 || d->nbytes > SIFT_PACKED_SLOT_MAX_BYTES)))
		return;
	if (sift_part_inline_image(rel, d, (uint32) e->part_id, &len) == NULL || len > SIFT_PACKED_SLOT_MAX_BYTES)
		return;
	if (*nc == *cap)
		*cands = repalloc(*cands, (*cap *= 2) * sizeof(PackCand));
	(*cands)[(*nc)++] = (PackCand) {.part_id = e->part_id,.d = *d,.len = len,.from = packed ? d->header_blk : InvalidBlockNumber};
	memcpy((*cands)[*nc - 1].key_lo, e->key_lo, SIFT_BOUND_BYTES);
}

/* The standalone tiny parts, and the parts of the packed pages that are less than half in use */
static PackCand *
survey(Relation rel, int *n)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	MemoryContext tmp = sift_mem_allocset_create(CurrentMemoryContext, "sift pack survey");
	int			cap = 64,
				nc = 0,
				keep = 0;
	PackCand   *cands = palloc(cap * sizeof(PackCand));

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int p = 0; p < np; p++)
		{
			MemoryContext old = MemoryContextSwitchTo(tmp);

			survey_part(rel, &mr.entries[i], &parts[p], &cands, &nc, &cap);
			MemoryContextSwitchTo(old);
			MemoryContextReset(tmp);
		}
	}
	qsort(cands, nc, sizeof(PackCand), cand_from_cmp);
	for (int i = 0, j; i < nc; i = j)
	{
		uint32		live = sizeof(SiftPackedPageData);

		for (j = i; j < nc && cands[j].from == cands[i].from; j++)
			live += MAXALIGN(cands[j].len);
		if (cands[i].from == InvalidBlockNumber || live < SIFT_PAGE_PAYLOAD / 2)
			for (int k = i; k < j; k++)
				cands[keep++] = cands[k];
	}
	MemoryContextDelete(tmp);
	*n = keep;
	return cands;
}

/* First fit, the longest first: the bin of each candidate */
static int
assign_bins(PackCand *cands, int n, PackBin *bins)
{
	int			nbins = 0;

	qsort(cands, n, sizeof(PackCand), cand_len_desc);
	for (int i = 0; i < n; i++)
	{
		int			b = 0;

		while (b < nbins && !(bins[b].n < SIFT_PACKED_MAX_SLOTS && MAXALIGN(bins[b].used) + cands[i].len <= SIFT_PAGE_PAYLOAD))
			b++;
		if (b == nbins)
			bins[nbins++] = (PackBin) {.used = sizeof(SiftPackedPageData)};
		bins[b].used = MAXALIGN(bins[b].used) + cands[i].len;
		bins[b].n++;
		cands[i].bin = b;
	}
	qsort(cands, n, sizeof(PackCand), cand_bin_cmp);
	return nbins;
}

/* The pages a batch is read from: packing it is worth it when they are at least two */
static int
source_pages(const PackCand *c, int n)
{
	int			pages = 0;

	for (int i = 0; i < n; i++)
	{
		bool		seen = false;

		for (int k = 0; k < i && c[i].from != InvalidBlockNumber; k++)
			seen |= c[k].from == c[i].from;
		pages += !seen;
	}
	return pages;
}

/* The locks of a swap, innermost first: a compaction of the partition is held off (it frees the same header blocks), and a packed source page is judged and freed one backend at a time */
static void
unlock_swap(Relation rel, uint64 part_id, bool packed_source)
{
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	if (packed_source)
		UnlockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
	UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
}

static bool
swap_to_slot(Relation rel, const PackCand *c, BlockNumber blk, int slot)
{
	SiftManifestRead mr;
	SiftPartDesc *parts,
				upd;
	SiftExtent	rel_ext[1];
	SiftPartRef leaving = {(uint32) c->part_id, c->d.part_gen};
	SiftManifestEdit e;
	int			np,
				p = 0,
				nrel = 0;
	bool		packed_source;

	if (!ConditionalLockPage(rel, SIFT_COMPACT_LOCK_BLKNO(c->part_id), ExclusiveLock))
		return false;
	packed_source = c->from != InvalidBlockNumber;
	if (packed_source)
		LockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
	LockPage(rel, SIFT_PART_LOCK_BLKNO(c->part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, c->key_lo, NULL);
	parts = mr.n > 0 && mr.entries[0].part_id == c->part_id ? sift_manifest_read_parts(rel, &mr.entries[0], &np) : NULL;
	while (parts != NULL && p < np && parts[p].part_gen != c->d.part_gen)
		p++;
	if (parts == NULL || p == np || parts[p].header_blk != c->d.header_blk)
	{
		unlock_swap(rel, c->part_id, packed_source);
		return false;
	}
	upd = parts[p];
	upd.header_blk = blk;
	upd.flags = (upd.flags & ~(SIFT_PART_FLAG_PACKED_SLOT | SIFT_PART_PACK_SLOT_MASK)) | SIFT_PART_PACK_FLAGS(slot);
	if (c->from == InvalidBlockNumber || sift_packed_unreferenced(rel, c->from, &leaving, 1))
		rel_ext[nrel++] = (SiftExtent) {c->d.header_blk, 1};
	e = (SiftManifestEdit) {.part_id = c->part_id,.key_lo = mr.entries[0].key_lo,.update_parts = &upd,.expect_parts = &parts[p],.nupdate = 1,.release = rel_ext,.nrelease = nrel};
	sift_manifest_commit(rel, &e);
	unlock_swap(rel, c->part_id, packed_source);
	return true;
}

static int
pack_batch(Relation rel, const PackCand *c, int n)
{
	SiftPackItem items[SIFT_PACKED_MAX_SLOTS] = {0};
	BlockNumber blk;
	int			swapped = 0;

	for (int i = 0; i < n; i++)
	{
		uint32		len;
		char	   *image = sift_part_inline_image(rel, &c[i].d, (uint32) c[i].part_id, &len);

		items[i] = (SiftPackItem) {.part_gen = c[i].d.part_gen,.part_id = (uint32) c[i].part_id,.image = image,.len = (uint16) len};
	}
	blk = sift_pack_write(rel, items, n);
	for (int i = 0; i < n; i++)
		swapped += swap_to_slot(rel, &c[i], blk, i);
	if (swapped == 0)
		sift_fsm_release(rel, &(SiftExtent) {blk, 1}, 1, InvalidFullTransactionId);
	return swapped;
}

int
sift_compact_pack_tails(Relation rel)
{
	int			n,
				packed = 0;
	PackCand   *cands = survey(rel, &n);
	PackBin    *bins = palloc(Max(n, 1) * sizeof(PackBin));
	int			nbins = assign_bins(cands, n, bins);
	MemoryContext tmp = sift_mem_allocset_create(CurrentMemoryContext, "sift pack batch");

	for (int b = 0, at = 0; b < nbins; at += bins[b++].n)
		if (bins[b].n >= SIFT_PACKED_MIN_BATCH && source_pages(cands + at, bins[b].n) >= 2)
		{
			MemoryContext old = MemoryContextSwitchTo(tmp);

			packed += pack_batch(rel, cands + at, bins[b].n);
			MemoryContextSwitchTo(old);
			MemoryContextReset(tmp);
		}
	MemoryContextDelete(tmp);
	return packed;
}
