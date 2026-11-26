/*
 * col/f2/pack.c - the maintenance step that packs tiny parts. A part whose every section is inline (SIFT_PART_FLAG_INLINE_STREAM) is a page of its own however few bytes it has; the
 * parts of level 1 and up that are at most SIFT_PACKED_SLOT_MAX_BYTES are written together as slots of sealed SIFT_PT_PACKED_PART pages, and the descriptor of each is swapped in the manifest to
 * name its slot, which releases the page the part had. A packed page with less than half of its bytes in use is packed again with the others. A page leaves with the commit that drops or swaps
 * its last part (sift_packed_unreferenced), so that no page is released while a descriptor names it.
 */
#include "core/host.h"

#include "port/pg_bitutils.h"
#include "storage/lmgr.h"

#include "col/f2/compact/compact.h"
#include "col/f2/part/format.h"
#include "core/cmp.h"
#include "core/f2/fsm/fsm.h"
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
		pfree(parts);
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

/* The room a bin has left for a part, 0 when it holds the most slots a page names */
static uint32
bin_room(const PackBin *b)
{
	uint32		used = MAXALIGN(b->used);

	return b->n < SIFT_PACKED_MAX_SLOTS && used < SIFT_PAGE_PAYLOAD ? SIFT_PAGE_PAYLOAD - used : 0;
}

/* First fit, the longest first: the bin of each candidate; a tree of the greatest room under each node finds the first bin a part fits in without looking at the others */
static int
assign_bins(PackCand *cands, int n, PackBin *bins)
{
	uint32		leaves = pg_nextpower2_32(Max(n, 1));
	uint32	   *room = palloc0(2 * leaves * sizeof(uint32));
	int			nbins = 0;

	qsort(cands, n, sizeof(PackCand), cand_len_desc);
	for (int i = 0; i < n; i++)
	{
		uint32		v = 1;
		int			b;

		if (room[1] < cands[i].len)
		{
			b = nbins++;
			bins[b] = (PackBin) {.used = sizeof(SiftPackedPageData)};
		}
		else
		{
			while (v < leaves)
				v = room[v << 1] >= cands[i].len ? v << 1 : (v << 1) | 1;
			b = (int) (v - leaves);
		}
		bins[b].used = MAXALIGN(bins[b].used) + cands[i].len;
		bins[b].n++;
		cands[i].bin = b;
		for (v = leaves + b, room[v] = bin_room(&bins[b]); v > 1; v >>= 1)
			room[v >> 1] = Max(room[v], room[v ^ 1]);
	}
	pfree(room);
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

/* The locks of a swap, innermost first: a compaction of the partition is held off (it frees the same header blocks); the locks of the splits is the batch's, taken once for the pages it judges */
static void
unlock_swap(Relation rel, uint64 part_id)
{
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
}

/* The parts of a batch that have left their packed pages, ascending by generation, and the slots of the manifest as it was read for the batch: what a page is judged by */
typedef struct PackJudge
{
	SiftLivePackedSet live;
	SiftPartRef left[SIFT_PACKED_MAX_SLOTS];
	int			nleft;
} PackJudge;

static void
left_add(PackJudge *j, SiftPartRef ref)
{
	int			at = j->nleft++;

	while (at > 0 && j->left[at - 1].part_gen > ref.part_gen)
	{
		j->left[at] = j->left[at - 1];
		at--;
	}
	j->left[at] = ref;
}

/* The parts of the batch that sit in the partition of the slots' first part still as the batch read them: the descriptors the commit expects and the ones it puts in their place */
static int
swap_prepare(Relation rel, const PackCand *c, const int *slots, int n, const SiftPartDesc *parts, int np, BlockNumber blk, PackJudge *judge, SiftPartDesc *upd, SiftPartDesc *expect,
			 SiftExtent *rel_ext, int *nrel)
{
	int			nupd = 0;

	for (int k = 0; k < n; k++)
	{
		const PackCand *cand = &c[slots[k]];
		int			p = 0;

		while (p < np && parts[p].part_gen != cand->d.part_gen)
			p++;
		if (p == np || parts[p].header_blk != cand->d.header_blk)
			continue;
		expect[nupd] = upd[nupd] = parts[p];
		upd[nupd].header_blk = blk;
		upd[nupd].flags = (upd[nupd].flags & ~(SIFT_PART_FLAG_PACKED_SLOT | SIFT_PART_PACK_SLOT_MASK)) | SIFT_PART_PACK_FLAGS(slots[k]);
		nupd++;
		if (cand->from == InvalidBlockNumber)
			rel_ext[(*nrel)++] = (SiftExtent) {cand->d.header_blk, 1};
		else
		{
			left_add(judge, (SiftPartRef) {(uint32) cand->part_id, cand->d.part_gen});
			if (sift_packed_unreferenced_in(rel, cand->from, judge->left, judge->nleft, &judge->live))
				rel_ext[(*nrel)++] = (SiftExtent) {cand->d.header_blk, 1};
		}
	}
	return nupd;
}

/* The slots of a batch that name parts of one partition */
typedef struct PackGroup
{
	uint64		part_id;
	int			slots[SIFT_PACKED_MAX_SLOTS];
	int			n;
	bool		locked;
} PackGroup;

static int
by_part_lock(const void *a, const void *b)
{
	return SIFT_CMP3(SIFT_PART_LOCK_BLKNO(((const PackGroup *) a)->part_id), SIFT_PART_LOCK_BLKNO(((const PackGroup *) b)->part_id));
}

/* The slots grouped by partition, in the order of the pages that lock the partitions: a backend that locks several takes them in one order */
static int
group_slots(const PackCand *c, int n, PackGroup *groups)
{
	bool		grouped[SIFT_PACKED_MAX_SLOTS] = {false};
	int			ng = 0;

	for (int i = 0; i < n; i++)
	{
		if (grouped[i])
			continue;
		groups[ng] = (PackGroup) {.part_id = c[i].part_id};
		for (int j = i; j < n; j++)
			if (!grouped[j] && c[j].part_id == c[i].part_id)
			{
				grouped[j] = true;
				groups[ng].slots[groups[ng].n++] = j;
			}
		ng++;
	}
	qsort(groups, ng, sizeof(PackGroup), by_part_lock);
	return ng;
}

/* The slots of a batch leave their pages in one commit of the manifest for all their partitions (those whose compaction is not running): the parts swapped */
static int
swap_to_slots(Relation rel, const PackCand *c, int n, BlockNumber blk, PackJudge *judge)
{
	PackGroup	groups[SIFT_PACKED_MAX_SLOTS];
	SiftManifestEdit edits[SIFT_PACKED_MAX_SLOTS];
	SiftPartDesc upd[SIFT_PACKED_MAX_SLOTS],
				expect[SIFT_PACKED_MAX_SLOTS];
	SiftExtent	rel_ext[SIFT_PACKED_MAX_SLOTS];
	int			ng = group_slots(c, n, groups),
				nedits = 0,
				nupd = 0,
				nrel = 0;

	for (int g = 0; g < ng; g++)
		groups[g].locked = ConditionalLockPage(rel, SIFT_COMPACT_LOCK_BLKNO(groups[g].part_id), ExclusiveLock);
	for (int g = 0; g < ng; g++)
		if (groups[g].locked)
			LockPage(rel, SIFT_PART_LOCK_BLKNO(groups[g].part_id), ExclusiveLock);
	for (int g = 0; g < ng; g++)
	{
		SiftManifestRead mr;
		SiftPartDesc *parts;
		int			np,
					k,
					r0 = nrel;

		if (!groups[g].locked)
			continue;
		mr = sift_manifest_read_range(rel, c[groups[g].slots[0]].key_lo, NULL);
		if (mr.n == 0 || mr.entries[0].part_id != groups[g].part_id)
			continue;
		parts = sift_manifest_read_parts(rel, &mr.entries[0], &np);
		k = swap_prepare(rel, c, groups[g].slots, groups[g].n, parts, np, blk, judge, upd + nupd, expect + nupd, rel_ext, &nrel);
		if (k > 0)
			edits[nedits++] = (SiftManifestEdit) {.part_id = groups[g].part_id,.key_lo = mr.entries[0].key_lo,.update_parts = upd + nupd,.expect_parts = expect + nupd,.nupdate = k,
			.release = rel_ext + r0,.nrelease = nrel - r0};
		nupd += k;
	}
	if (nedits > 0)
		sift_manifest_commit_many(rel, edits, nedits);
	for (int g = ng - 1; g >= 0; g--)
		if (groups[g].locked)
			unlock_swap(rel, groups[g].part_id);
	return nupd;
}

static int
pack_batch(Relation rel, const PackCand *c, int n)
{
	SiftPackItem items[SIFT_PACKED_MAX_SLOTS] = {0};
	PackJudge  *judge = palloc(sizeof(PackJudge));
	bool		packed_source = false;
	BlockNumber blk;
	int			swapped;

	for (int i = 0; i < n; i++)
	{
		uint32		len;
		char	   *image = sift_part_inline_image(rel, &c[i].d, (uint32) c[i].part_id, &len);

		items[i] = (SiftPackItem) {.part_gen = c[i].d.part_gen,.part_id = (uint32) c[i].part_id,.image = image,.len = (uint16) len};
		packed_source |= c[i].from != InvalidBlockNumber;
	}
	blk = sift_pack_write(rel, items, n);
	judge->nleft = 0;
	if (packed_source)
	{
		LockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
		sift_live_packed_set_build(rel, &judge->live);
	}
	swapped = swap_to_slots(rel, c, n, blk, judge);
	if (packed_source)
	{
		sift_live_packed_set_free(&judge->live);
		UnlockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
	}
	pfree(judge);
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
