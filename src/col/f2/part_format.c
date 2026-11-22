/*
 * col/part_format.c - write and open an immutable part of format major 2 (col/part_format.h).
 */
#include "core/host.h"

#include "access/xact.h"

#include "col/f2/part_format.h"
#include "col/granule/granule.h"
#include "core/extlock.h"
#include "core/f2/bulk_io.h"
#include "core/f2/extent.h"
#include "core/f2/fsm.h"
#include "core/f2/io_stream.h"
#include "col/f2/f2_tier.h"
#include "core/page_io.h"

#define PHDR(p)		((SiftPartHeaderData *) SiftPageGetData(p))

bool
sift_section_known(uint16 tag)
{
	return (tag >= SEC_SCHEMA && tag <= SEC_DICTS) || (tag >= SEC_VERS && tag <= SEC_DELVEC) || (tag >= SEC_GRANULE_TSTRIDE && tag <= SEC_VARIANT_EXC) || (tag >= SEC_ZONES && tag <= SEC_POINT) || tag == SEC_SKIP_INDEX || tag == SEC_KLL || tag == SEC_KEY_MARKS || tag == SEC_COL_STATS ||
		(tag >= SEC_LEAF_STATS && tag <= SEC_SCHEMA_DEFAULTS && tag != 0x0302) || (tag >= SEC_SCOPE_HDR && tag <= SEC_SCOPE_ENT_ROUTE);
}

static int
secin_cmp(const void *a, const void *b)
{
	return (int) ((const SiftSecInput *) a)->tag - (int) ((const SiftSecInput *) b)->tag;
}

/* The extents the next writes of this backend take their blocks from before the allocator is asked (sift_part_prefer_extents): of one relation file, for the length of the move that asked */
static SiftExtent *preferred = NULL;
static int	npreferred = 0;
static RelFileLocator preferred_locator;
static bool preferred_callback_registered;

static void
preferred_clear(void)
{
	preferred = NULL;
	npreferred = 0;
}

static bool
preferred_of(Relation rel)
{
	return npreferred > 0 && RelFileLocatorEquals(preferred_locator, rel->rd_locator);
}

/* The extents live in the memory of the transaction that took them: at its end they are gone whatever the move did */
static void
preferred_xact_end(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT)
		preferred_clear();
}

void
sift_part_prefer_extents(Relation rel, const SiftExtent *ext, int n)
{
	if (!preferred_callback_registered)
	{
		RegisterXactCallback(preferred_xact_end, NULL);
		preferred_callback_registered = true;
	}
	preferred = MemoryContextAlloc(TopTransactionContext, Max(n, 1) * sizeof(SiftExtent));
	memcpy(preferred, ext, n * sizeof(SiftExtent));
	npreferred = n;
	preferred_locator = rel->rd_locator;
}

void
sift_part_discard_preferred(const RelFileLocator *locator)
{
	if (npreferred > 0 && RelFileLocatorEquals(preferred_locator, *locator))
		preferred_clear();
}

void
sift_part_release_preferred(Relation rel)
{
	if (preferred_of(rel))
	{
		SiftExtent *held = preferred;
		int			n = npreferred;

		preferred_clear();
		sift_fsm_release(rel, held, n, InvalidFullTransactionId);
	}
}

/* Blocks of the preferred extents for up to want blocks: the rest of an extent that is not all used stays preferred; returns the extents written */
static int
take_preferred(Relation rel, uint64 *want, uint64 chunk, SiftExtent *out)
{
	int			n = 0;

	while (*want > 0 && preferred_of(rel))
	{
		uint64		take = Min(Min((uint64) preferred[0].nblocks, *want), chunk);

		out[n++] = (SiftExtent) {preferred[0].start, (uint32) take};
		*want -= take;
		preferred[0].start += take;
		preferred[0].nblocks -= take;
		if (preferred[0].nblocks == 0)
			memmove(preferred, preferred + 1, --npreferred * sizeof(SiftExtent));
	}
	return n;
}

bool
sift_part_take_preferred_page(Relation rel, BlockNumber *blk)
{
	uint64		want = 1;
	SiftExtent	one;

	if (!preferred_of(rel) || take_preferred(rel, &want, 1, &one) == 0)
		return false;
	*blk = one.start;
	return true;
}

/* The stream as pages: each is made from the sections that cover it (a section that has a fill is asked for its bytes, the checksum of its whole made as they go by) */
static void
write_stream(Relation rel, const SiftPartIn *in, const SiftSecInput *secs, SiftSecDirEntry *dir, int nsec, uint64 stream_bytes, const SiftExtent *ext, int next)
{
	uint64		npages = SIFT_PAYLOAD_PAGES(stream_bytes),
				g = 0;
	pg_crc32c  *crc = palloc(Max(nsec, 1) * sizeof(pg_crc32c));
	SiftPartBulkWriter w;
	SiftBulkRun *run = in->bulk_tail ? sift_bulk_run_begin(rel) : NULL;

	for (int i = 0; i < nsec; i++)
		INIT_CRC32C(crc[i]);
	if (run == NULL)
		sift_bulk_extend(rel, ext, next);
	sift_bulk_begin(&w, rel);
	for (int e = 0; e < next && g < npages; e++)
		for (uint32 b = 0; b < ext[e].nblocks && g < npages; b++, g++)
		{
			BlockNumber blk = ext[e].start + b;
			bool		fresh = run != NULL && sift_bulk_run_fresh(run, blk);
			Page		pg = fresh ? sift_bulk_run_page(run) : sift_bulk_page(&w, blk);
			uint64		from = SIFT_PAYLOAD_BYTES(g),
						n = Min(stream_bytes - from, (uint64) SIFT_PAGE_PAYLOAD);

			sift_page_init_part(pg, SIFT_PT_DATA, in->part_gen, in->part_id);
			memset(SiftPageGetData(pg), 0, n);
			for (int i = 0; i < nsec; i++)
			{
				uint64		lo = Max(from, (uint64) dir[i].off),
							hi = Min(from + n, (uint64) dir[i].off + secs[i].len);
				char	   *to = SiftPageGetData(pg) + (lo - from);

				if (secs[i].inline_in_header || lo >= hi)
					continue;
				if (secs[i].fill == NULL)
					memcpy(to, (const char *) secs[i].data + (lo - dir[i].off), hi - lo);
				else
				{
					secs[i].fill(secs[i].fill_arg, lo - dir[i].off, hi - lo, to);
					COMP_CRC32C(crc[i], to, hi - lo);
				}
			}
			SiftPageSetUsed(pg, n);
			if (fresh)
				sift_bulk_run_write(run, blk, pg);
		}
	sift_bulk_end(&w);
	if (run != NULL)
		sift_bulk_run_end(run);
	for (int i = 0; i < nsec; i++)
		if (secs[i].fill != NULL)
		{
			FIN_CRC32C(crc[i]);
			dir[i].crc32c = (secs[i].flags & SIFT_SEC_FLAG_NO_CRC) ? 0 : crc[i];
		}
	pfree(crc);
}

/* Whether the part is its header page alone: every section is in memory and the table of sections with all of them fits the page */
static bool
fits_header_page(const SiftSecInput *secs, int nsec)
{
	uint64		bytes = sizeof(SiftPartHeaderData) + nsec * sizeof(SiftSecDirEntry);

	for (int i = 0; i < nsec; i++)
	{
		if (secs[i].fill != NULL)
			return false;
		bytes += MAXALIGN(secs[i].len);
	}
	return bytes <= SIFT_PAGE_PAYLOAD;
}

/* The envelope of the addresses of the rows of the part, from its SEC_TID_INDEX: what lets a lookup by address skip the parts that cannot hold it */
static void
tid_envelope(const SiftPartIn *in, SiftPartDesc *d)
{
	d->tid_min = UINT64_MAX;
	d->tid_max = 0;
	for (int i = 0; i < in->nsections; i++)
	{
		const SiftTidIndexEntry *tix = in->sections[i].data;

		if (in->sections[i].tag != SEC_TID_INDEX)
			continue;
		for (uint64 g = 0; g < in->sections[i].len / sizeof(SiftTidIndexEntry); g++)
		{
			d->tid_min = Min(d->tid_min, tix[g].tid_min);
			d->tid_max = Max(d->tid_max, tix[g].tid_max);
		}
	}
}

SiftPartDesc
sift_part_write(Relation rel, const SiftPartIn *in)
{
	int			nsec = in->nsections;
	SiftSecInput *secs = palloc(Max(nsec, 1) * sizeof(SiftSecInput));
	SiftSecDirEntry *dir = palloc0(Max(nsec, 1) * sizeof(SiftSecDirEntry));
	uint64		stream_bytes = 0;
	uint32		inline_bytes = 0;
	bool		inline_stream;
	uint64		npages,
				remaining;
	SiftExtent *ext;
	int			next = 0;
	uint64		chunk = in->max_extent_blocks > 0 ? in->max_extent_blocks : SIFT_FSM_MAX_EXTENT;
	PGAlignedBlock img;
	Page		page = img.data;
	SiftPartHeaderData *h;
	uint32		ninline,
				fixed;
	BlockNumber extlist = InvalidBlockNumber;
	SiftXlog	x;
	SiftPartDesc d = {0};
	BlockNumber hdr_blk;

	memcpy(secs, in->sections, nsec * sizeof(SiftSecInput));
	qsort(secs, nsec, sizeof(SiftSecInput), secin_cmp);
	inline_stream = fits_header_page(secs, nsec);
	for (int i = 0; i < nsec; i++)
	{
		secs[i].inline_in_header |= inline_stream;
		if (i > 0 && secs[i].tag == secs[i - 1].tag)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: section 0x%04x given twice", secs[i].tag)));
		dir[i].tag = secs[i].tag;
		dir[i].flags = secs[i].flags | (secs[i].inline_in_header ? SIFT_SEC_FLAG_INLINE : 0);
		dir[i].len = secs[i].len;
		dir[i].crc32c = (secs[i].flags & SIFT_SEC_FLAG_NO_CRC) || secs[i].fill != NULL ? 0 : sift_crc32c(secs[i].data, secs[i].len);
		if (secs[i].inline_in_header)
		{
			dir[i].off = inline_bytes;
			inline_bytes += MAXALIGN(secs[i].len);
		}
		else
		{
			stream_bytes = MAXALIGN(stream_bytes);
			dir[i].off = stream_bytes;
			stream_bytes += secs[i].len;
		}
	}
	npages = SIFT_PAYLOAD_PAGES(stream_bytes);
	remaining = npages;
	ext = palloc((npages + 1) * sizeof(SiftExtent));
	while (remaining > 0 && preferred_of(rel))
	{
		uint64		want = remaining;

		next += take_preferred(rel, &want, chunk, ext + next);
		remaining = want;
	}
	while (remaining > 0)
	{
		uint64		want = Min(remaining, chunk);

		next += sift_fsm_alloc_stream(rel, want, ext + next, (int) (npages + 1) - next);
		remaining -= want;
	}
	if (npages > 0)
		write_stream(rel, in, secs, dir, nsec, stream_bytes, ext, next);

	fixed = sizeof(SiftPartHeaderData) + nsec * sizeof(SiftSecDirEntry);
	if (fixed + inline_bytes > SIFT_PAGE_PAYLOAD)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: the inline sections and the section table of a part take %u bytes, a header page holds %zu",
																		fixed + inline_bytes, (size_t) SIFT_PAGE_PAYLOAD),
						errhint("Write the larger sections as data extents, not inline.")));
	ninline = (SIFT_PAGE_PAYLOAD - fixed - inline_bytes) / sizeof(SiftExtent);
	ninline = Min(ninline, (uint32) next);
	if (in->inline_extent_cap >= 0)
		ninline = Min(ninline, (uint32) in->inline_extent_cap);
	if ((int) ninline < next)
		extlist = sift_extlist_write(rel, in->part_gen, in->part_id, ext + ninline, next - ninline);

	sift_page_init_part(page, SIFT_PT_PARTHDR, in->part_gen, in->part_id);
	h = PHDR(page);
	h->magic = SIFT_PARTHDR_MAGIC;
	h->version = SIFT_PARTHDR_VERSION;
	h->nsec = nsec;
	h->flags = in->flags | (inline_stream ? SIFT_PART_FLAG_INLINE_STREAM : 0);
	h->part_gen = in->part_gen;
	h->part_id = in->part_id;
	h->level = in->level;
	h->ncols = in->ncols;
	h->nrows = in->nrows;
	h->stream_bytes = stream_bytes;
	h->ngranules = in->ngranules;
	h->granule_rows = in->granule_rows;
	h->min_xid = in->min_xid;
	h->max_xid = in->max_xid;
	h->total_extents = next;
	h->ninline_extents = ninline;
	h->extlist_blk = extlist;
	h->secdir_off = sizeof(SiftPartHeaderData);
	h->extents_off = fixed;
	h->inline_meta_off = inline_bytes > 0 ? fixed + ninline * sizeof(SiftExtent) : 0;
	h->inline_meta_len = inline_bytes;
	for (int i = 0; i < nsec; i++)
		if (secs[i].inline_in_header)
			dir[i].off += h->inline_meta_off;
	memcpy(SiftPageGetData(page) + h->secdir_off, dir, nsec * sizeof(SiftSecDirEntry));
	memcpy(SiftPageGetData(page) + h->extents_off, ext, ninline * sizeof(SiftExtent));
	for (int i = 0; i < nsec; i++)
		if (secs[i].inline_in_header)
			memcpy(SiftPageGetData(page) + dir[i].off, secs[i].data, secs[i].len);
	SiftPageSetUsed(page, fixed + ninline * sizeof(SiftExtent) + inline_bytes);
	sift_page_seal(page);

	hdr_blk = sift_fsm_alloc_meta_page(rel);
	x = sift_xlog_start(rel);
	memcpy(sift_xlog_register(&x, sift_blk_new(rel, hdr_blk), GENERIC_XLOG_FULL_IMAGE), page, BLCKSZ);
	sift_xlog_done(&x);

	d.part_gen = in->part_gen;
	d.level = in->level;
	d.flags = (uint16) in->flags;
	d.header_blk = hdr_blk;
	d.dv_blk = InvalidBlockNumber;
	d.col_delta_blk = InvalidBlockNumber;
	tid_envelope(in, &d);
	d.min_xid = in->min_xid;
	d.max_xid = in->max_xid;
	d.dv_max_xid = InvalidFullTransactionId;
	d.nrows = in->nrows;
	d.nbytes = inline_stream ? fixed + inline_bytes : stream_bytes;
	return d;
}

static Page
read_page_copy(Relation rel, BlockNumber blk)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	Page		page = palloc(BLCKSZ);

	memcpy(page, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return page;
}

/* The header at the start of payload, its table of sections and the extents of the stream, checked against the bytes in use; the payload stays in the part */
static SiftPart *
parse_part(Relation rel, char *payload, uint32 used, BlockNumber header_blk, uint64 part_gen, uint32 part_id, bool remote)
{
	SiftPart   *p = palloc0(sizeof(SiftPart));
	const SiftPartHeaderData *h = (const SiftPartHeaderData *) payload;
	uint64		pages = 0;

	if (used < sizeof(SiftPartHeaderData) || h->magic != SIFT_PARTHDR_MAGIC)
		SIFT_SEG_CORRUPT(rel, part_gen, "Block %u: the header of the part has the wrong magic.", header_blk);
	if (h->version != SIFT_PARTHDR_VERSION)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift index \"%s\": part %llu has header version %u, this build reads %u", RelationGetRelationName(rel),
																	   (unsigned long long) part_gen, h->version, SIFT_PARTHDR_VERSION),
						errhint("Upgrade pg_sift, or REINDEX the index with this version.")));
	if (h->part_gen != part_gen || h->part_id != part_id || h->secdir_off != sizeof(SiftPartHeaderData) ||
		h->extents_off != h->secdir_off + h->nsec * sizeof(SiftSecDirEntry) || h->ninline_extents > h->total_extents ||
		h->extents_off + h->ninline_extents * sizeof(SiftExtent) > used || h->inline_meta_off + h->inline_meta_len > used ||
		(h->total_extents > h->ninline_extents) != (h->extlist_blk != InvalidBlockNumber) || (remote && h->total_extents != 0) ||
		((h->flags & SIFT_PART_FLAG_INLINE_STREAM) && (h->total_extents != 0 || h->stream_bytes != 0)))
		SIFT_SEG_CORRUPT(rel, part_gen, "Block %u: the header of the part is inconsistent.", header_blk);

	p->hdr = *h;
	p->header_blk = header_blk;
	p->inline_payload = payload;
	p->inline_len = used;
	p->secdir = (SiftSecDirEntry *) (payload + h->secdir_off);
	for (int i = 0; i < h->nsec; i++)
	{
		const SiftSecDirEntry *s = &p->secdir[i];
		bool		inl = (s->flags & SIFT_SEC_FLAG_INLINE) != 0;

		if (i > 0 && s->tag <= p->secdir[i - 1].tag)
			SIFT_SEG_CORRUPT(rel, part_gen, "The sections of the part are not in strict tag order at 0x%04x.", s->tag);
		if (!sift_section_known(s->tag) && (s->flags & SIFT_SEC_FLAG_REQUIRED))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift index \"%s\": part %llu has a required section 0x%04x this pg_sift does not know",
																		   RelationGetRelationName(rel), (unsigned long long) part_gen, s->tag),
							errhint("Install the pg_sift release that wrote the index.")));
		if (inl ? (s->off < h->inline_meta_off || s->off + s->len > h->inline_meta_off + h->inline_meta_len)
			: ((h->flags & SIFT_PART_FLAG_INLINE_STREAM) || s->off % 8 != 0 || s->off + s->len > h->stream_bytes))
			SIFT_SEG_CORRUPT(rel, part_gen, "Section 0x%04x of the part lies outside its bytes.", s->tag);
	}
	p->extents = palloc(Max(h->total_extents, 1) * sizeof(SiftExtent));
	memcpy(p->extents, payload + h->extents_off, h->ninline_extents * sizeof(SiftExtent));
	if (h->total_extents > h->ninline_extents)
	{
		SiftExtent *rest = sift_extlist_read(rel, h->extlist_blk, part_gen, part_id, h->total_extents - h->ninline_extents, &p->extlist_pages, &p->nextlist_pages);

		memcpy(p->extents + h->ninline_extents, rest, (h->total_extents - h->ninline_extents) * sizeof(SiftExtent));
	}
	p->pagestart = palloc(Max(h->total_extents, 1) * sizeof(uint64));
	for (uint32 i = 0; i < h->total_extents; i++)
	{
		p->pagestart[i] = pages;
		pages += p->extents[i].nblocks;
	}
	if (!remote && SIFT_PAYLOAD_BYTES(pages) < h->stream_bytes)
		SIFT_SEG_CORRUPT(rel, part_gen, "The extents of the part hold %llu pages, its stream needs %llu bytes.", (unsigned long long) pages, (unsigned long long) h->stream_bytes);
	return p;
}

/* The slot of the part in a packed page: the one the descriptor names (hint < 0: any) */
static SiftPart *
open_packed(Relation rel, Page page, BlockNumber blk, uint64 part_gen, uint32 part_id, int hint)
{
	const SiftPackedPageData *pp = (const SiftPackedPageData *) SiftPageGetData(page);
	uint32		used = SiftPageGetUsed(page);
	int			at = -1;
	const SiftPackedSlot *s;
	char	   *payload;
	SiftPart   *p;

	sift_page_expect(rel, page, blk, SIFT_PT_PACKED_PART, 0, 0);
	if (used < sizeof(SiftPackedPageData) || pp->magic != SIFT_PACKED_MAGIC || !sift_page_sum_ok(page) || pp->version != SIFT_PACKED_VERSION || pp->nslots == 0 ||
		pp->nslots > SIFT_PACKED_MAX_SLOTS || pp->used_end > used)
		SIFT_SEG_CORRUPT(rel, part_gen, "Block %u: the packed page has the wrong magic, version, slot count or checksum.", blk);
	for (int i = 0; i < pp->nslots && at < 0; i++)
		if ((hint < 0 || hint == i) && pp->slots[i].part_gen == part_gen)
			at = i;
	if (at < 0)
		SIFT_SEG_CORRUPT(rel, part_gen, "Block %u: the packed page has no slot for part %llu.", blk, (unsigned long long) part_gen);
	s = &pp->slots[at];
	if (s->off < sizeof(SiftPackedPageData) || s->off % MAXIMUM_ALIGNOF != 0 || (uint32) s->off + s->len > pp->used_end)
		SIFT_SEG_CORRUPT(rel, part_gen, "Block %u: slot %d of the packed page lies outside its bytes.", blk, at);
	payload = palloc(s->len);
	memcpy(payload, SiftPageGetData(page) + s->off, s->len);
	p = parse_part(rel, payload, s->len, blk, part_gen, s->part_id, false);
	if (!(p->hdr.flags & SIFT_PART_FLAG_INLINE_STREAM))
		SIFT_SEG_CORRUPT(rel, part_gen, "Block %u: slot %d of the packed page holds a part with a data page.", blk, at);
	p->is_packed = true;
	p->pack_slot = at;
	return p;
}

static SiftPart *
open_at(Relation rel, BlockNumber header_blk, uint64 part_gen, uint32 part_id, int slot_hint)
{
	Page		page = read_page_copy(rel, header_blk);

	if (sift_page_layout_ok(page) && SiftPageGetOpaque(page)->type == SIFT_PT_PACKED_PART)
		return open_packed(rel, page, header_blk, part_gen, part_id, slot_hint);
	if (slot_hint >= 0)
		SIFT_SEG_CORRUPT(rel, part_gen, "Block %u: the descriptor names a packed slot, the page is not a packed page.", header_blk);
	sift_page_expect(rel, page, header_blk, SIFT_PT_PARTHDR, part_gen, SIFT_PART_ID_ANY);
	if (!sift_page_sum_ok(page))
		SIFT_SEG_CORRUPT(rel, part_gen, "Block %u: the header of the part has the wrong checksum.", header_blk);
	return parse_part(rel, SiftPageGetData(page), SiftPageGetUsed(page), header_blk, part_gen, SiftPageGetOpaque(page)->part_id, false);
}

SiftPart *
sift_part_open(Relation rel, BlockNumber header_blk, uint64 part_gen, uint32 part_id)
{
	return open_at(rel, header_blk, part_gen, part_id, -1);
}

/* The partition the object was written for is the one in its header: a merge or a split of partitions since then does not change it */
static SiftPart *
open_remote(Relation rel, const SiftPartDesc *d)
{
	char	   *image;
	uint32		len;
	struct SiftTierRemote *remote;
	SiftPart   *p;

	sift_tier_host_open(rel, d, &image, &len, &remote);
	p = parse_part(rel, image, len, InvalidBlockNumber, d->part_gen, remote->ref.part_id, true);
	p->remote = remote;
	return p;
}

SiftPart *
sift_part_open_desc(Relation rel, const SiftPartDesc *d, uint32 part_id)
{
	if (d->ext_flags & SIFT_PART_EXT_REMOTE)
		return open_remote(rel, d);
	return open_at(rel, d->header_blk, d->part_gen, part_id, (d->flags & SIFT_PART_FLAG_PACKED_SLOT) ? (int) SIFT_PART_PACK_SLOT(d->flags) : -1);
}

char *
sift_part_inline_image(Relation rel, const SiftPartDesc *d, uint32 part_id, uint32 *len)
{
	SiftPart   *p = sift_part_open_desc(rel, d, part_id);

	if (!(p->hdr.flags & SIFT_PART_FLAG_INLINE_STREAM))
		return NULL;
	*len = p->inline_len;
	return p->inline_payload;
}

bool
sift_packed_unreferenced(Relation rel, BlockNumber blk, const SiftPartRef *leaving, int nleaving)
{
	Page		page = read_page_copy(rel, blk);
	const SiftPackedPageData *pp = (const SiftPackedPageData *) SiftPageGetData(page);
	SiftManifestRead mr;

	sift_page_expect(rel, page, blk, SIFT_PT_PACKED_PART, 0, 0);
	if (pp->magic != SIFT_PACKED_MAGIC || !sift_page_sum_ok(page) || pp->nslots == 0 || pp->nslots > SIFT_PACKED_MAX_SLOTS)
		SIFT_CORRUPT(rel, "Block %u: the packed page has the wrong magic, slot count or checksum.", blk);
	mr = sift_manifest_read_range(rel, NULL, NULL);
	for (int i = 0; i < pp->nslots; i++)
	{
		const SiftPackedSlot *s = &pp->slots[i];
		bool		leaves = false;

		for (int k = 0; k < nleaving; k++)
			leaves |= leaving[k].part_gen == s->part_gen;
		if (leaves)
			continue;
		for (int k = 0; k < mr.n; k++)
		{
			int			np;
			SiftPartDesc *parts = sift_manifest_read_parts(rel, &mr.entries[k], &np);

			for (int j = 0; j < np; j++)
				if (parts[j].part_gen == s->part_gen && parts[j].header_blk == blk)
					return false;
		}
	}
	return true;
}

int
sift_pack_fit(const SiftPackItem *items, int n)
{
	uint32		end = sizeof(SiftPackedPageData);
	int			k = 0;

	while (k < n && k < SIFT_PACKED_MAX_SLOTS && MAXALIGN(end) + items[k].len <= SIFT_PAGE_PAYLOAD)
		end = MAXALIGN(end) + items[k++].len;
	return k;
}

BlockNumber
sift_pack_write(Relation rel, const SiftPackItem *items, int n)
{
	PGAlignedBlock img;
	Page		page = img.data;
	SiftPackedPageData *pp;
	uint32		end = sizeof(SiftPackedPageData);
	BlockNumber blk;
	SiftXlog	x;

	Assert(n >= 1 && n == sift_pack_fit(items, n));
	sift_page_init_part(page, SIFT_PT_PACKED_PART, 0, 0);
	pp = (SiftPackedPageData *) SiftPageGetData(page);
	pp->magic = SIFT_PACKED_MAGIC;
	pp->version = SIFT_PACKED_VERSION;
	pp->nslots = n;
	for (int i = 0; i < n; i++)
	{
		end = MAXALIGN(end);
		pp->slots[i] = (SiftPackedSlot) {.part_gen = items[i].part_gen,.part_id = items[i].part_id,.off = end,.len = items[i].len};
		memcpy(SiftPageGetData(page) + end, items[i].image, items[i].len);
		end += items[i].len;
	}
	pp->used_end = end;
	SiftPageSetUsed(page, end);
	sift_page_seal(page);
	blk = sift_fsm_alloc_meta_page(rel);
	x = sift_xlog_start(rel);
	memcpy(sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE), page, BLCKSZ);
	sift_xlog_done(&x);
	return blk;
}

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
