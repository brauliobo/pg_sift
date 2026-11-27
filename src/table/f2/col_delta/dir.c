/*
 * table/f2/col_delta/dir.c - the directory of a long column delta chain: its pages, the entries that tell a reader which data pages to read, and their I/O (f2_col_delta_dir.h).
 */
#include "postgres.h"

#include "storage/bufmgr.h"

#include "col/f2/meta.h"
#include "core/f2/fsm/fsm.h"
#include "core/page_io.h"
#include "table/f2/col_delta/dir.h"

/* ---- the entries ---- */

bool
sift_cddir_entry_wanted(const SiftColDeltaDirEntry *e, const SiftColDeltaPrune *prune)
{
	if (prune == NULL)
		return true;
	if (prune->part_gen != 0 && e->part_gen != 0 && e->part_gen != prune->part_gen)
		return false;
	if (prune->by_granule && (e->granule_hi < prune->granule_lo || e->granule_lo > prune->granule_hi))
		return false;
	return prune->attnum_bloom == 0 || (e->attnum_bloom & prune->attnum_bloom) != 0;
}

SiftColDeltaDirEntry
sift_cddir_entry_of_batch(BlockNumber blk, const SiftColDeltaBatch *b)
{
	return (SiftColDeltaDirEntry) {
		.blkno = blk,.nrecs = b->nentries,.part_gen = b->part_gen,.granule_lo = b->granule_lo,.granule_hi = b->granule_hi,.attnum_bloom = b->attnum_bloom,
		.flags = (uint16) (b->dir_flags | ((b->page_flags & SIFT_CDPG_LEAVES) == 0 ? SIFT_CDD_FLAG_TID_ONLY : 0)),
	};
}

SiftColDeltaDirEntry
sift_cddir_entry_of_page(BlockNumber blk, const char *page)
{
	SiftColDeltaBatch seen;

	sift_col_delta_page_summary(page, &seen);
	return sift_cddir_entry_of_batch(blk, &seen);
}

static void
entry_widen(SiftColDeltaDirEntry *e, const SiftColDeltaBatch *b)
{
	e->part_gen = e->part_gen == b->part_gen ? e->part_gen : 0;
	e->granule_lo = Min(e->granule_lo, b->granule_lo);
	e->granule_hi = Max(e->granule_hi, b->granule_hi);
	e->attnum_bloom |= b->attnum_bloom;
	e->nrecs += b->nentries;
	e->flags = (uint16) ((e->flags | b->dir_flags) & ~((b->page_flags & SIFT_CDPG_LEAVES) != 0 ? SIFT_CDD_FLAG_TID_ONLY : 0));
}

/* ---- the pages ---- */

void
sift_cddir_page_init(Page page, uint64 gen, uint32 part_id, BlockNumber next_dir_blk, uint32 pages_before)
{
	SiftColDeltaDirHdr *h;

	sift_page_init_part(page, SIFT_PT_COL_DELTA_DIR, gen, part_id);
	h = (SiftColDeltaDirHdr *) SiftPageGetData(page);
	memset(h, 0, sizeof(*h));
	h->magic = SIFT_CDDIR_MAGIC;
	h->version = SIFT_CDDIR_VER;
	h->part_id = part_id;
	h->total_delta_pages = pages_before;
	h->next_dir_blk = next_dir_blk;
	h->prev_dir_blk = InvalidBlockNumber;
	h->min_xid = h->max_xid = InvalidFullTransactionId;
	SiftPageSetUsed(page, sizeof(*h));
	sift_page_seal(page);
}

static void
page_widen_xids(SiftColDeltaDirHdr *h, FullTransactionId lo, FullTransactionId hi)
{
	if (!FullTransactionIdIsValid(lo))
		return;
	if (!FullTransactionIdIsValid(h->min_xid) || FullTransactionIdPrecedes(lo, h->min_xid))
		h->min_xid = lo;
	h->max_xid = sift_fxid_max(h->max_xid, hi);
}

bool
sift_cddir_page_add(Page page, const SiftColDeltaDirEntry *e, FullTransactionId min_xid, FullTransactionId max_xid)
{
	SiftColDeltaDirHdr *h = (SiftColDeltaDirHdr *) SiftPageGetData(page);

	if (h->nentries >= SIFT_CDDIR_MAX_ENTRIES)
		return false;
	memcpy(SiftPageGetData(page) + sizeof(*h) + (Size) h->nentries * sizeof(*e), e, sizeof(*e));
	h->nentries++;
	h->total_delta_pages++;
	page_widen_xids(h, min_xid, max_xid);
	SiftPageSetUsed(page, sizeof(*h) + (Size) h->nentries * sizeof(*e));
	sift_page_seal(page);
	return true;
}

void
sift_cddir_page_widen_last(Page page, const SiftColDeltaBatch *b)
{
	SiftColDeltaDirHdr *h = (SiftColDeltaDirHdr *) SiftPageGetData(page);
	SiftColDeltaDirEntry *last = (SiftColDeltaDirEntry *) (SiftPageGetData(page) + sizeof(*h)) + (h->nentries - 1);

	entry_widen(last, b);
	if (b->nentries > 0)
		page_widen_xids(h, b->min_xid, b->max_xid);
	sift_page_seal(page);
}

const char *
sift_cddir_page_check(const char *page)
{
	const SiftColDeltaDirHdr *h;

	if (!sift_page_layout_ok(page))
		return "a page of the directory is not laid out as a page of the index";
	if (SiftPageGetOpaque((Page) page)->type != SIFT_PT_COL_DELTA_DIR || SiftPageGetOpaque((Page) page)->flags != 0)
		return "a page of the directory is not a column delta directory page";
	if (SiftPageGetUsed(page) < sizeof(*h) || !sift_page_sum_ok((Page) page))
		return "a page of the directory does not match its checksum";
	h = sift_cddir_page_hdr(page);
	if (h->magic != SIFT_CDDIR_MAGIC || h->version != SIFT_CDDIR_VER || h->flags != 0 || h->prev_dir_blk != InvalidBlockNumber)
		return "a page of the directory has a header this format does not have";
	if (h->nentries < 1 || h->nentries > SIFT_CDDIR_MAX_ENTRIES || h->nentries > h->total_delta_pages || SiftPageGetUsed(page) != sizeof(*h) + (Size) h->nentries * sizeof(SiftColDeltaDirEntry))
		return "a page of the directory holds a count of entries it does not have room for";
	if ((h->total_delta_pages == h->nentries) != (h->next_dir_blk == InvalidBlockNumber))
		return "a page of the directory is not as deep as its links say";
	for (uint32 i = 0; i < h->nentries; i++)
	{
		const SiftColDeltaDirEntry *e = &sift_cddir_page_entries(page)[i];

		if (e->blkno == InvalidBlockNumber || (e->flags & ~SIFT_CDD_FLAGS_KNOWN) != 0 || e->granule_lo > e->granule_hi)
			return "an entry of the directory is not one of this format";
	}
	return NULL;
}

/* ---- reading ---- */

/* A vector of blocks that holds 4 and doubles when n (a power of two from 4 on) are in it */
static BlockNumber *
grow_blks(BlockNumber *v, int n)
{
	if (v == NULL)
		return palloc(sizeof(BlockNumber) * 4);
	return n >= 4 && (n & (n - 1)) == 0 ? repalloc(v, sizeof(BlockNumber) * 2 * n) : v;
}

const char *
sift_cddir_chain_entries(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, SiftColDeltaDirEntry **entries, BlockNumber **dir_blks, int *ndir)
{
	char	   *buf = palloc(BLCKSZ);
	BlockNumber blk = anchor->delta_head_blkno;
	const char *why = NULL;
	int			filled = anchor->npages;
	SiftColDeltaDirEntry *ent = palloc(sizeof(SiftColDeltaDirEntry) * filled);

	*dir_blks = NULL;
	*ndir = 0;
	for (bool first = true; filled > 0 && why == NULL; first = false)
	{
		const SiftColDeltaDirHdr *h;
		int			take;

		if (blk == InvalidBlockNumber)
			why = "the directory of a chain is shorter than its anchor says";
		if (why == NULL)
			why = reader->read_dir(reader->ctx, blk, buf);
		if (why == NULL)
			why = sift_cddir_page_check(buf);
		if (why != NULL)
			break;
		h = sift_cddir_page_hdr(buf);
		take = first ? (int) h->nentries - ((int) h->total_delta_pages - filled) : (int) h->nentries;
		if ((first ? (int) h->total_delta_pages < filled : (int) h->total_delta_pages != filled) || take < 1 || take > filled)
		{
			why = "a page of the directory does not hold the pages its anchor counts";
			break;
		}
		memcpy(ent + (filled - take), sift_cddir_page_entries(buf), sizeof(*ent) * take);
		*dir_blks = grow_blks(*dir_blks, *ndir);
		(*dir_blks)[(*ndir)++] = blk;
		filled -= take;
		blk = h->next_dir_blk;
	}
	pfree(buf);
	if (why == NULL && blk != InvalidBlockNumber)
		why = "the directory of a chain is longer than its anchor says";
	if (why != NULL)
	{
		pfree(ent);
		if (*dir_blks != NULL)
			pfree(*dir_blks);
		*dir_blks = NULL;
		*ndir = 0;
		return why;
	}
	*entries = ent;
	return NULL;
}

const char *
sift_cddir_chain_read(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, const SiftColDeltaPrune *prune, SiftColDeltaChain *out)
{
	SiftColDeltaDirEntry *ent;
	const char *why = sift_cddir_chain_entries(anchor, reader, &ent, &out->dir_blks, &out->ndir);
	int			n = anchor->npages,
				keep = 0;

	if (why != NULL)
		return why;

	for (int i = 0; i < n; i++)
		keep += sift_cddir_entry_wanted(&ent[i], prune);
	/* MEM[MEM-TBL-15]: caller-scoped col_delta chain arena (the pages the prune keeps * 8 KB; freed by chain_free) */
	out->arena = palloc((Size) Max(keep, 1) * BLCKSZ);
	out->blks = palloc(sizeof(BlockNumber) * Max(keep, 1));
	out->nchain = n;
	for (int i = 0; i < n && why == NULL; i++)
	{
		char	   *dst = out->arena + (Size) out->npages * BLCKSZ;

		if (!sift_cddir_entry_wanted(&ent[i], prune))
			continue;
		why = reader->read(reader->ctx, ent[i].blkno, dst);
		if (why == NULL)
			why = sift_col_delta_page_check(dst);
		if (why == NULL)
			why = sift_col_delta_check_link(dst, i + 1, i == 0 ? InvalidBlockNumber : ent[i - 1].blkno);
		out->blks[out->npages++] = ent[i].blkno;
	}
	pfree(ent);
	return why;
}

SiftExtent *
sift_col_delta_chain_extents(const SiftColDeltaChain *c, int *n)
{
	SiftExtent *x = palloc(sizeof(SiftExtent) * Max(c->npages + c->ndir, 1));

	Assert(c->npages == c->nchain);
	for (int i = 0; i < c->npages; i++)
		x[i] = (SiftExtent) {c->blks[i], 1};
	for (int i = 0; i < c->ndir; i++)
		x[c->npages + i] = (SiftExtent) {c->dir_blks[i], 1};
	*n = c->npages + c->ndir;
	return x;
}

const char *
sift_cddir_promote_entries(BlockNumber head, int n, const SiftColDeltaPageReader *reader, SiftColDeltaDirEntry *entries, FullTransactionId *min_xid, FullTransactionId *max_xid)
{
	char	   *buf = palloc(BLCKSZ);
	const char *why = NULL;
	BlockNumber blk = head;

	for (int i = n - 1; i >= 0 && why == NULL; i--)
	{
		why = reader->read(reader->ctx, blk, buf);
		if (why == NULL)
			why = sift_col_delta_page_check(buf);
		if (why == NULL)
			why = sift_col_delta_check_link(buf, i + 1, i == 0 ? InvalidBlockNumber : sift_col_delta_page_hdr(buf)->prev_blk);
		if (why == NULL)
		{
			entries[i] = sift_cddir_entry_of_page(blk, buf);
			if (!FullTransactionIdIsValid(*min_xid) || FullTransactionIdPrecedes(sift_col_delta_page_hdr(buf)->min_xid, *min_xid))
				*min_xid = sift_col_delta_page_hdr(buf)->min_xid;
			*max_xid = sift_fxid_max(*max_xid, sift_col_delta_page_hdr(buf)->max_xid);
		}
		blk = sift_col_delta_page_hdr(buf)->prev_blk;
	}
	pfree(buf);
	return why;
}

/* ---- the writer of a chain on disk ---- */

static void
disk_commit_anchor(void *ctx, const SiftColDeltaAnchor *a)
{
	SiftColDeltaDiskChain  *d = ctx;
	SiftManifestEdit edit = {.part_id = d->entry->part_id,.key_lo = d->entry->key_lo,.set_col_delta = true,.new_col_delta = *a};

	if (d->capture != NULL)
		*d->capture = *a;
	else
		sift_manifest_commit(d->rel, &edit);
}

/* The records of b that fit the newest page, appended to it in place; with whole, all of them or none */
static bool
append_in_place(SiftColDeltaDiskChain *d, BlockNumber head, BlockNumber dir_head, SiftColDeltaBatch *b, FullTransactionId next, bool whole)
{
	Relation	rel = d->rel;
	uint32		part_id = (uint32) d->entry->part_id;
	Buffer		buf = sift_blk_read(rel, head, BUFFER_LOCK_EXCLUSIVE),
				dbuf = InvalidBuffer;
	Page		page = BufferGetPage(buf);
	SiftColDeltaBatch part;
	uint32		room;
	SiftXlog	x;

	sift_page_expect(rel, page, head, SIFT_PT_COL_DELTA, 0, part_id);
	if (!sift_page_sum_ok(page))
		SIFT_CORRUPT(rel, "Block %u of the column delta chain does not match its checksum.", head);
	room = (sift_col_delta_page_hdr(page)->flags & SIFT_CDPG_SEALED) != 0 ? 0 : SIFT_PAGE_PAYLOAD - SiftPageGetUsed(page);
	if (whole && b->nbytes > room)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	sift_col_delta_batch_split(b, room, next, &part);
	if (part.nentries == 0)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	if (dir_head != InvalidBlockNumber)
		dbuf = sift_blk_read(rel, dir_head, BUFFER_LOCK_EXCLUSIVE);
	x = sift_xlog_start(rel);
	SIFT_INVARIANT(sift_col_delta_page_append(sift_xlog_register(&x, buf, 0), &part), "a column delta batch that fit the page did not");
	if (dbuf != InvalidBuffer)
		sift_cddir_page_widen_last(sift_xlog_register(&x, dbuf, 0), &part);
	sift_xlog_done(&x);
	sift_col_delta_batch_free(&part);
	return true;
}

static bool
disk_append_head(void *ctx, BlockNumber head, BlockNumber dir_head, SiftColDeltaBatch *b, FullTransactionId next)
{
	return append_in_place(ctx, head, dir_head, b, next, false);
}

static BlockNumber
disk_dir_data_head(void *ctx, BlockNumber dir_head)
{
	SiftColDeltaDiskChain  *d = ctx;

	return sift_cddir_io_newest_data(d->rel, (uint32) d->entry->part_id, dir_head);
}

bool
sift_col_delta_disk_append_in_place(SiftColDeltaDiskChain *d, const SiftColDeltaAnchor *a, SiftColDeltaBatch *b, FullTransactionId next)
{
	bool		indexed = (a->flags & SIFT_CDLA_INDEXED_DIR) != 0;
	uint16		flags = a->flags | sift_col_delta_anchor_flags_of(b->page_flags);

	if ((b->page_flags & SIFT_CDPG_LEAVES) != 0)
		flags &= (uint16) ~SIFT_CDLA_TID_ONLY;
	if (!sift_col_delta_anchor_has_chain(a) || flags != a->flags)
		return false;
	return append_in_place(d, indexed ? disk_dir_data_head(d, a->delta_head_blkno) : a->delta_head_blkno, indexed ? a->delta_head_blkno : InvalidBlockNumber, b, next, true);
}

static BlockNumber
disk_dir_promote(void *ctx, BlockNumber data_head, int npages)
{
	SiftColDeltaDiskChain  *d = ctx;

	return sift_cddir_io_promote(d->rel, (uint32) d->entry->part_id, data_head, npages);
}

static BlockNumber
disk_dir_add(void *ctx, BlockNumber dir_head, const BlockNumber *blks, const SiftColDeltaBatch *parts, int n)
{
	SiftColDeltaDiskChain  *d = ctx;

	return sift_cddir_io_add(d->rel, (uint32) d->entry->part_id, dir_head, blks, parts, n);
}

static uint64
disk_reserve_gens(void *ctx, uint32 n)
{
	SiftColDeltaDiskChain  *d = ctx;
	uint64		first;

	if (d->gen_left < n)
		return sift_manifest_new_gen(d->rel, n);
	first = d->gen_next;
	d->gen_next += n;
	d->gen_left -= n;
	return first;
}

static BlockNumber
disk_new_page(void *ctx, const SiftColDeltaBatch *b, BlockNumber prev, uint16 depth, uint64 gen)
{
	SiftColDeltaDiskChain  *d = ctx;
	Relation	rel = d->rel;
	uint32		part_id = (uint32) d->entry->part_id;
	BlockNumber blk = sift_fsm_alloc_meta_page(rel);
	SiftXlog	x = sift_xlog_start(rel);
	Page		page = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);

	sift_col_delta_page_init(page, gen, part_id, prev, depth);
	SIFT_INVARIANT(sift_col_delta_page_append(page, b), "a column delta batch of one page did not fit an empty page");
	sift_xlog_done(&x);
	return blk;
}

SiftColDeltaWriter
sift_col_delta_disk_writer(SiftColDeltaDiskChain *d)
{
	return (SiftColDeltaWriter) {.ctx = d,.append_head = disk_append_head,.dir_data_head = disk_dir_data_head,.dir_promote = disk_dir_promote,.dir_add = disk_dir_add,
								 .reserve_gens = disk_reserve_gens,.new_page = disk_new_page,.commit_anchor = disk_commit_anchor};
}

/* ---- the directory pages on disk ---- */

static Buffer
dir_buffer(Relation rel, BlockNumber blk, uint32 part_id, int lockmode)
{
	Buffer		buf = sift_blk_read(rel, blk, lockmode);

	sift_page_expect(rel, BufferGetPage(buf), blk, SIFT_PT_COL_DELTA_DIR, 0, part_id);
	if (!sift_page_sum_ok(BufferGetPage(buf)))
		SIFT_CORRUPT(rel, "Block %u of the column delta directory does not match its checksum.", blk);
	return buf;
}

BlockNumber
sift_cddir_io_newest_data(Relation rel, uint32 part_id, BlockNumber dir_head)
{
	Buffer		buf = dir_buffer(rel, dir_head, part_id, BUFFER_LOCK_SHARE);
	const SiftColDeltaDirHdr *h = sift_cddir_page_hdr((const char *) BufferGetPage(buf));
	BlockNumber blk = sift_cddir_page_entries((const char *) BufferGetPage(buf))[h->nentries - 1].blkno;

	UnlockReleaseBuffer(buf);
	return blk;
}

static BlockNumber
new_dir_page(Relation rel, uint32 part_id, BlockNumber next, uint32 pages_before, const SiftColDeltaDirEntry *first, int nfirst, FullTransactionId min_xid, FullTransactionId max_xid)
{
	BlockNumber blk = sift_fsm_alloc_meta_page(rel);
	SiftXlog	x = sift_xlog_start(rel);
	Page		page = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);

	sift_cddir_page_init(page, sift_manifest_new_gen(rel, 1), part_id, next, pages_before);
	for (int i = 0; i < nfirst; i++)
		SIFT_INVARIANT(sift_cddir_page_add(page, &first[i], min_xid, max_xid), "the entries of a new directory page did not fit it");
	sift_xlog_done(&x);
	return blk;
}

BlockNumber
sift_cddir_io_promote(Relation rel, uint32 part_id, BlockNumber data_head, int npages)
{
	SiftColDeltaDirEntry ent[SIFT_CDDIR_FLAT_PAGES];
	SiftColDeltaDiskReader d = {.rel = rel,.part_id = part_id};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&d);
	const char *why;
	FullTransactionId lo = InvalidFullTransactionId,
				hi = InvalidFullTransactionId;

	SIFT_INVARIANT(npages >= 1 && npages <= SIFT_CDDIR_FLAT_PAGES, "a chain the directory is made for is not a short flat chain");
	why = sift_cddir_promote_entries(data_head, npages, &reader, ent, &lo, &hi);
	if (why != NULL)
		SIFT_CORRUPT(rel, "%s (partition %u).", why, part_id);
	return new_dir_page(rel, part_id, InvalidBlockNumber, 0, ent, npages, lo, hi);
}

/* The writers of the pages of the parts as one range */
static void
parts_xid_range(const SiftColDeltaBatch *parts, int n, FullTransactionId *lo, FullTransactionId *hi)
{
	*lo = *hi = InvalidFullTransactionId;
	for (int i = 0; i < n; i++)
	{
		if (!FullTransactionIdIsValid(parts[i].min_xid))
			continue;
		if (!FullTransactionIdIsValid(*lo) || FullTransactionIdPrecedes(parts[i].min_xid, *lo))
			*lo = parts[i].min_xid;
		*hi = sift_fxid_max(*hi, parts[i].max_xid);
	}
}

/* The entries that fit the newest directory page go to it in one record; the rest make directory pages of their own, each a full page of entries but the last */
BlockNumber
sift_cddir_io_add(Relation rel, uint32 part_id, BlockNumber dir_head, const BlockNumber *blks, const SiftColDeltaBatch *parts, int n)
{
	Buffer		buf = dir_buffer(rel, dir_head, part_id, BUFFER_LOCK_EXCLUSIVE);
	Page		page = BufferGetPage(buf);
	uint32		total = sift_cddir_page_hdr((const char *) page)->total_delta_pages;
	int			fit = Min(n, SIFT_CDDIR_MAX_ENTRIES - (int) sift_cddir_page_hdr((const char *) page)->nentries);
	SiftColDeltaDirEntry *ents = palloc(Max(n, 1) * sizeof(SiftColDeltaDirEntry));
	BlockNumber head = dir_head;

	for (int i = 0; i < n; i++)
		ents[i] = sift_cddir_entry_of_batch(blks[i], &parts[i]);
	if (fit > 0)
	{
		SiftXlog	x = sift_xlog_start(rel);

		page = sift_xlog_register(&x, buf, 0);
		for (int i = 0; i < fit; i++)
			SIFT_INVARIANT(sift_cddir_page_add(page, &ents[i], parts[i].min_xid, parts[i].max_xid), "an entry that fit the directory page did not");
		sift_xlog_done(&x);
		total += (uint32) fit;
	}
	else
		UnlockReleaseBuffer(buf);
	for (int at = fit; at < n; at += SIFT_CDDIR_MAX_ENTRIES)
	{
		int			take = Min(n - at, SIFT_CDDIR_MAX_ENTRIES);
		FullTransactionId lo,
					hi;

		parts_xid_range(parts + at, take, &lo, &hi);
		head = new_dir_page(rel, part_id, head, total, ents + at, take, lo, hi);
		total += (uint32) take;
	}
	pfree(ents);
	return head;
}
