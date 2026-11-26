/*
 * col/part_format.c - write and open an immutable part of format major 2 (col/part_format.h).
 */
#include "core/host.h"

#include "access/xact.h"

#include "col/f2/part/format.h"
#include "col/granule/granule.h"
#include "core/extlock.h"
#include "core/f2/bulk_io.h"
#include "core/f2/extent.h"
#include "core/f2/fsm/fsm.h"
#include "core/f2/io_stream.h"
#include "col/f2/tier/tier.h"
#include "core/hash.h"
#include "core/page_io.h"

#define PHDR(p)		((SiftPartHeaderData *) SiftPageGetData(p))

bool
sift_section_known(uint16 tag)
{
	return (tag >= SEC_SCHEMA && tag <= SEC_DICTS) || (tag >= SEC_VERS && tag <= SEC_ROW_PACK) || (tag >= SEC_GRANULE_TSTRIDE && tag <= SEC_VARIANT_EXC) || (tag >= SEC_ZONES && tag <= SEC_POINT) || tag == SEC_SKIP_INDEX || tag == SEC_KLL || tag == SEC_KEY_MARKS || tag == SEC_COL_STATS ||
		(tag >= SEC_LEAF_STATS && tag <= SEC_SCHEMA_DEFAULTS && tag != 0x0302) || (tag >= SEC_SCOPE_HDR && tag <= SEC_SCOPE_ENT_ROUTE);
}

static int
secin_cmp(const void *a, const void *b)
{
	return (int) ((const SiftSecInput *) a)->tag - (int) ((const SiftSecInput *) b)->tag;
}

/* The extents the next writes of this backend take their blocks from before the allocator is asked (sift_part_prefer_extents): of one relation file, for the length of the move that asked; the first of them is the one taken from, the memory of the rest stays the transaction's */
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

/* The extents live in the memory of the transaction that took them: at its end, or at the abort of the subtransaction that was running the move, they are gone whatever the move did */
static void
preferred_xact_end(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT)
		preferred_clear();
}

static void
preferred_subxact_end(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		preferred_clear();
}

void
sift_part_prefer_extents(Relation rel, const SiftExtent *ext, int n)
{
	if (!preferred_callback_registered)
	{
		RegisterXactCallback(preferred_xact_end, NULL);
		RegisterSubXactCallback(preferred_subxact_end, NULL);
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
		{
			preferred++;
			if (--npreferred == 0)
				preferred = NULL;
		}
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
	SiftBulkRun *run = in->bulk_tail ? sift_bulk_run_begin(rel, ext, next) : NULL;

	for (int i = 0; i < nsec; i++)
		INIT_CRC32C(crc[i]);
	PG_TRY();
	{
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
	}
	PG_CATCH();
	{
		if (run != NULL)
			sift_bulk_run_abort(run);
		PG_RE_THROW();
	}
	PG_END_TRY();
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

/* The sections the readers reach through sift_part_read_section or sift_part_read_section_bytes, which may sit in the header page */
static bool
inlinable(uint16 tag)
{
	return tag == SEC_SCHEMA || tag == SEC_GRANULE_DIR || tag == SEC_DICTS || tag == SEC_TID_INDEX || tag == SEC_GRANULE_TSTRIDE || tag == SEC_VARIANT_EXC || tag == SEC_PREAGG || tag == SEC_POINT ||
		tag == SEC_SKIP_INDEX || tag == SEC_KLL || tag == SEC_KEY_MARKS || tag == SEC_COL_STATS || tag == SEC_SCHEMA_DEFAULTS || tag == SEC_ZONES;
}

static int
len_cmp(const void *a, const void *b)
{
	uint64		x = (*(const SiftSecInput *const *) a)->len,
				y = (*(const SiftSecInput *const *) b)->len;

	return x < y ? -1 : x > y;
}

/* STOR[STOR-COL-01]: greedy inline of the small metadata sections in SIFT_PT_PARTHDR (reclaims ~7.3 KB of header slack and a tail page per part) */
static void
inline_small_sections(SiftSecInput *secs, int nsec)
{
	uint64		room = SIFT_PAGE_PAYLOAD - sizeof(SiftPartHeaderData) - (uint64) nsec * sizeof(SiftSecDirEntry) - SIFT_INLINE_EXTENT_RESERVE;
	/* check-memory: allow one pointer for each section of the part */
	const SiftSecInput **cand = palloc(Max(nsec, 1) * sizeof(*cand));
	int			n = 0;

	for (int i = 0; i < nsec; i++)
	{
		if (secs[i].inline_in_header)
			room -= Min(room, (uint64) MAXALIGN(secs[i].len));
		else if (secs[i].fill == NULL && inlinable(secs[i].tag))
			cand[n++] = &secs[i];
	}
	qsort(cand, n, sizeof(*cand), len_cmp);
	for (int i = 0; i < n && MAXALIGN(cand[i]->len) <= room; i++)
	{
		room -= MAXALIGN(cand[i]->len);
		((SiftSecInput *) cand[i])->inline_in_header = true;
	}
	pfree(cand);
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

/* The descriptor of a part written from in, but for the sort key range */
static SiftPartDesc
part_desc(const SiftPartIn *in, BlockNumber header_blk, uint64 nbytes)
{
	SiftPartDesc d = {0};

	d.part_gen = in->part_gen;
	d.level = in->level;
	d.flags = (uint16) in->flags;
	d.header_blk = header_blk;
	d.dv_blk = InvalidBlockNumber;
	d.col_delta_blk = InvalidBlockNumber;
	tid_envelope(in, &d);
	d.min_xid = in->min_xid;
	d.max_xid = in->max_xid;
	d.dv_max_xid = InvalidFullTransactionId;
	d.nrows = in->nrows;
	d.nbytes = nbytes;
	return d;
}

/*
 * The blocks a part took from the allocator and failed to write (a full disk, a cancel) are given back: its stream extents, the pages of its extent list and its header page. The extents a move
 * prefers are the move's, and none of these blocks is in a manifest. The error is held while the allocator is asked, which takes locks and logs: no lock of the allocator is held where the
 * write can fail (the allocation is done), and the lock of the extension is given back by write_stream before this runs.
 */
static void
part_abandon(Relation rel, const SiftPartIn *in, const SiftExtent *ext, int n, BlockNumber extlist, int nlisted, BlockNumber hdr_blk)
{
	BlockNumber *pages = NULL;
	int			npages = 0;
	SiftExtent *back;

	if (extlist != InvalidBlockNumber)
		(void) sift_extlist_read(rel, extlist, in->part_gen, in->part_id, nlisted, &pages, &npages);
	back = palloc((n + npages + 1) * sizeof(SiftExtent));
	memcpy(back, ext, n * sizeof(SiftExtent));
	for (int i = 0; i < npages; i++)
		back[n + i] = (SiftExtent) {pages[i], 1};
	back[n + npages] = (SiftExtent) {hdr_blk, 1};
	sift_fsm_release(rel, back, n + npages + 1, InvalidFullTransactionId);
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
	BlockNumber hdr_blk;
	int			taken;
	MemoryContext cx;

	memcpy(secs, in->sections, nsec * sizeof(SiftSecInput));
	qsort(secs, nsec, sizeof(SiftSecInput), secin_cmp);
	inline_stream = fits_header_page(secs, nsec);
	if (!inline_stream)
		inline_small_sections(secs, nsec);
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
	fixed = sizeof(SiftPartHeaderData) + nsec * sizeof(SiftSecDirEntry);
	if (fixed + inline_bytes > SIFT_PAGE_PAYLOAD)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: the inline sections and the section table of a part take %u bytes, a header page holds %zu",
																		fixed + inline_bytes, (size_t) SIFT_PAGE_PAYLOAD),
						errhint("Write the larger sections as data extents, not inline.")));
	npages = SIFT_PAYLOAD_PAGES(stream_bytes);
	remaining = npages;
	ext = palloc((npages + 1) * sizeof(SiftExtent));
	while (remaining > 0 && preferred_of(rel))
	{
		uint64		want = remaining;

		next += take_preferred(rel, &want, chunk, ext + next);
		remaining = want;
	}
	taken = next;
	while (remaining > 0)
	{
		uint64		want = Min(remaining, chunk);

		next += sift_fsm_alloc_stream(rel, want, ext + next, (int) (npages + 1) - next);
		remaining -= want;
	}
	ninline = (SIFT_PAGE_PAYLOAD - fixed - inline_bytes) / sizeof(SiftExtent);
	ninline = Min(ninline, (uint32) next);
	if (in->inline_extent_cap >= 0)
		ninline = Min(ninline, (uint32) in->inline_extent_cap);
	if ((int) ninline < next)
		extlist = sift_extlist_write(rel, in->part_gen, in->part_id, ext + ninline, next - ninline);

	hdr_blk = sift_fsm_alloc_meta_page(rel);
	cx = CurrentMemoryContext;
	PG_TRY();
	{
		if (npages > 0)
			write_stream(rel, in, secs, dir, nsec, stream_bytes, ext, next);
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

		x = sift_xlog_start(rel);
		memcpy(sift_xlog_register(&x, sift_blk_new(rel, hdr_blk), GENERIC_XLOG_FULL_IMAGE), page, BLCKSZ);
		sift_xlog_done(&x);
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(cx);
		e = CopyErrorData();
		FlushErrorState();
		part_abandon(rel, in, ext + taken, next - taken, extlist, next - (int) ninline, hdr_blk);
		ReThrowError(e);
	}
	PG_END_TRY();

	return part_desc(in, hdr_blk, inline_stream ? fixed + inline_bytes : stream_bytes);
}

static SiftPart *parse_part(Relation rel, char *payload, uint32 used, BlockNumber header_blk, uint64 part_gen, uint32 part_id, bool remote);

/* The part of in with every section inline in an image of its own, as a slot of a packed page is, and nothing written: its descriptor names no block */
SiftPart *
sift_part_in_memory(Relation rel, const SiftPartIn *in, SiftPartDesc *d)
{
	int			nsec = in->nsections;
	SiftSecInput *secs = palloc(Max(nsec, 1) * sizeof(SiftSecInput));
	SiftSecDirEntry *dir = palloc0(Max(nsec, 1) * sizeof(SiftSecDirEntry));
	uint32		fixed = sizeof(SiftPartHeaderData) + nsec * sizeof(SiftSecDirEntry),
				inline_bytes = 0;
	char	   *image;
	SiftPartHeaderData *h;

	memcpy(secs, in->sections, nsec * sizeof(SiftSecInput));
	qsort(secs, nsec, sizeof(SiftSecInput), secin_cmp);
	for (int i = 0; i < nsec; i++)
	{
		if (secs[i].fill != NULL || (i > 0 && secs[i].tag == secs[i - 1].tag))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: a part in memory takes its sections as they are, once each")));
		dir[i].tag = secs[i].tag;
		dir[i].flags = secs[i].flags | SIFT_SEC_FLAG_INLINE;
		dir[i].len = secs[i].len;
		dir[i].crc32c = (secs[i].flags & SIFT_SEC_FLAG_NO_CRC) ? 0 : sift_crc32c(secs[i].data, secs[i].len);
		dir[i].off = fixed + inline_bytes;
		inline_bytes += MAXALIGN(secs[i].len);
	}
	image = palloc0(fixed + inline_bytes);
	h = (SiftPartHeaderData *) image;
	h->magic = SIFT_PARTHDR_MAGIC;
	h->version = SIFT_PARTHDR_VERSION;
	h->nsec = nsec;
	h->flags = in->flags | SIFT_PART_FLAG_INLINE_STREAM;
	h->part_gen = in->part_gen;
	h->part_id = in->part_id;
	h->level = in->level;
	h->ncols = in->ncols;
	h->nrows = in->nrows;
	h->ngranules = in->ngranules;
	h->granule_rows = in->granule_rows;
	h->min_xid = in->min_xid;
	h->max_xid = in->max_xid;
	h->extlist_blk = InvalidBlockNumber;
	h->secdir_off = sizeof(SiftPartHeaderData);
	h->extents_off = fixed;
	h->inline_meta_off = inline_bytes > 0 ? fixed : 0;
	h->inline_meta_len = inline_bytes;
	memcpy(image + h->secdir_off, dir, nsec * sizeof(SiftSecDirEntry));
	for (int i = 0; i < nsec; i++)
		memcpy(image + dir[i].off, secs[i].data, secs[i].len);
	*d = part_desc(in, InvalidBlockNumber, fixed + inline_bytes);
	return parse_part(rel, image, fixed + inline_bytes, InvalidBlockNumber, in->part_gen, in->part_id, false);
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

static uint32
live_home(const SiftLivePackedSet *set, BlockNumber blk, uint64 part_gen)
{
	return (uint32) (sift_fmix64(part_gen ^ ((uint64) blk << 32)) & set->mask);
}

static void
live_add(SiftLivePackedSet *set, BlockNumber blk, uint64 part_gen)
{
	uint32		h = live_home(set, blk, part_gen);

	while (set->slots[h].header_blk != InvalidBlockNumber)
		h = (h + 1) & set->mask;
	set->slots[h] = (SiftLivePackedKey) {blk, part_gen};
	set->count++;
}

static bool
live_has(const SiftLivePackedSet *set, BlockNumber blk, uint64 part_gen)
{
	for (uint32 h = live_home(set, blk, part_gen); set->slots[h].header_blk != InvalidBlockNumber; h = (h + 1) & set->mask)
		if (set->slots[h].header_blk == blk && set->slots[h].part_gen == part_gen)
			return true;
	return false;
}

static void
live_grow(SiftLivePackedSet *set)
{
	SiftLivePackedKey *old = set->slots;
	uint32		n = set->mask + 1;

	set->mask = n * 2 - 1;
	set->slots = palloc((set->mask + 1) * sizeof(SiftLivePackedKey));
	for (uint32 i = 0; i <= set->mask; i++)
		set->slots[i].header_blk = InvalidBlockNumber;
	set->count = 0;
	for (uint32 i = 0; i < n; i++)
		if (old[i].header_blk != InvalidBlockNumber)
			live_add(set, old[i].header_blk, old[i].part_gen);
	pfree(old);
}

void
sift_live_packed_set_build(Relation rel, SiftLivePackedSet *set)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	set->mask = 63;
	set->slots = palloc((set->mask + 1) * sizeof(SiftLivePackedKey));
	for (uint32 i = 0; i <= set->mask; i++)
		set->slots[i].header_blk = InvalidBlockNumber;
	set->count = 0;
	for (int k = 0; k < mr.n; k++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &mr.entries[k], &np);

		for (int j = 0; j < np; j++)
		{
			if (!(parts[j].flags & SIFT_PART_FLAG_PACKED_SLOT))
				continue;
			if ((uint64) (set->count + 1) * 2 > (uint64) set->mask + 1)
				live_grow(set);
			live_add(set, parts[j].header_blk, parts[j].part_gen);
		}
		pfree(parts);
	}
}

void
sift_live_packed_set_free(SiftLivePackedSet *set)
{
	pfree(set->slots);
	set->slots = NULL;
}

bool
sift_packed_unreferenced_in(Relation rel, BlockNumber blk, const SiftPartRef *leaving, int nleaving, const SiftLivePackedSet *live)
{
	Page		page = read_page_copy(rel, blk);
	const SiftPackedPageData *pp = (const SiftPackedPageData *) SiftPageGetData(page);

	sift_page_expect(rel, page, blk, SIFT_PT_PACKED_PART, 0, 0);
	if (pp->magic != SIFT_PACKED_MAGIC || !sift_page_sum_ok(page) || pp->nslots == 0 || pp->nslots > SIFT_PACKED_MAX_SLOTS)
		SIFT_CORRUPT(rel, "Block %u: the packed page has the wrong magic, slot count or checksum.", blk);
	for (int i = 0; i < pp->nslots; i++)
	{
		const SiftPackedSlot *s = &pp->slots[i];
		int			lo = 0,
					hi = nleaving;

		while (lo < hi)
		{
			int			mid = lo + (hi - lo) / 2;

			if (leaving[mid].part_gen < s->part_gen)
				lo = mid + 1;
			else
				hi = mid;
		}
		if ((lo == nleaving || leaving[lo].part_gen != s->part_gen) && live_has(live, blk, s->part_gen))
			return false;
	}
	return true;
}

bool
sift_packed_unreferenced(Relation rel, BlockNumber blk, const SiftPartRef *leaving, int nleaving)
{
	SiftLivePackedSet live;
	bool		free;

	sift_live_packed_set_build(rel, &live);
	free = sift_packed_unreferenced_in(rel, blk, leaving, nleaving, &live);
	sift_live_packed_set_free(&live);
	return free;
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

/* ---- the part moved piece by piece ---- */

static void
add_piece(SiftExtent *ext, int *next, BlockNumber start, uint32 nblocks)
{
	if (*next > 0 && ext[*next - 1].start + ext[*next - 1].nblocks == start)
		ext[*next - 1].nblocks += nblocks;
	else
		ext[(*next)++] = (SiftExtent) {start, nblocks};
}

static void
copy_stream_page(Relation rel, SiftPartBulkWriter *w, const SiftPart *part, BlockNumber from, BlockNumber to)
{
	PGAlignedBlock img;
	Buffer		buf = sift_blk_read(rel, from, BUFFER_LOCK_SHARE);

	memcpy(img.data, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	sift_page_expect(rel, img.data, from, SIFT_PT_DATA, part->hdr.part_gen, part->hdr.part_id);
	memcpy(sift_bulk_page(w, to), img.data, BLCKSZ);
}

/* The extents of the stream in its order, the pieces at and above cut being those of `to` in turn; the blocks the pieces left are added to freed */
static int
move_stream_pieces(Relation rel, const SiftPart *part, BlockNumber cut, const SiftExtent *to, int nto, SiftExtent *ext, SiftExtent *freed, int *nfreed)
{
	SiftPartBulkWriter w;
	int			next = 0,
				at = 0;
	uint32		used = 0;

	sift_bulk_extend(rel, to, nto);
	sift_bulk_begin(&w, rel);
	for (uint32 i = 0; i < part->hdr.total_extents; i++)
	{
		SiftExtent	e = part->extents[i];
		uint64		end = (uint64) e.start + e.nblocks;
		BlockNumber moved_from = Max(e.start, cut);

		if (e.start < cut)
			add_piece(ext, &next, e.start, (uint32) (Min(end, (uint64) cut) - e.start));
		if (end <= cut)
			continue;
		freed[(*nfreed)++] = (SiftExtent) {moved_from, (uint32) (end - moved_from)};
		for (BlockNumber from = moved_from; from < end;)
		{
			uint32		take;

			SIFT_INVARIANT(at < nto, "index \"%s\": the blocks reserved for the move of part %llu hold fewer pages than the stream has above block %u", RelationGetRelationName(rel),
						   (unsigned long long) part->hdr.part_gen, cut);
			take = (uint32) Min(end - from, (uint64) to[at].nblocks - used);

			for (uint32 b = 0; b < take; b++)
				copy_stream_page(rel, &w, part, from + b, to[at].start + used + b);
			add_piece(ext, &next, to[at].start + used, take);
			from += take;
			used += take;
			if (used == to[at].nblocks)
			{
				at++;
				used = 0;
			}
		}
	}
	sift_bulk_end(&w);
	SIFT_INVARIANT(at == nto, "index \"%s\": the blocks reserved for the move of part %llu hold more pages than the stream has above block %u", RelationGetRelationName(rel),
				   (unsigned long long) part->hdr.part_gen, cut);
	return next;
}

/* The header page of the part with the extents ext[0..next) in place of its own: the sections it keeps inline follow the extents it holds, the rest of the list goes to a chain of its own */
static BlockNumber
write_moved_header(Relation rel, const SiftPart *part, const SiftExtent *ext, int next, BlockNumber *extlist, uint32 *ninline_out)
{
	Page		page = read_page_copy(rel, part->header_blk);
	SiftPartHeaderData *h = PHDR(page);
	SiftSecDirEntry *dir = (SiftSecDirEntry *) (SiftPageGetData(page) + h->secdir_off);
	uint32		fixed = h->extents_off,
				old_meta = h->inline_meta_off,
				ninline = Min((uint32) ((SIFT_PAGE_PAYLOAD - fixed - h->inline_meta_len) / sizeof(SiftExtent)), (uint32) next),
				meta = h->inline_meta_len > 0 ? fixed + ninline * sizeof(SiftExtent) : 0;
	SiftXlog	x;
	BlockNumber blk;

	*extlist = InvalidBlockNumber;
	*ninline_out = ninline;
	if (h->inline_meta_len > 0)
	{
		memmove(SiftPageGetData(page) + meta, SiftPageGetData(page) + old_meta, h->inline_meta_len);
		for (int i = 0; i < h->nsec; i++)
			if (dir[i].flags & SIFT_SEC_FLAG_INLINE)
				dir[i].off = dir[i].off - old_meta + meta;
	}
	memcpy(SiftPageGetData(page) + fixed, ext, ninline * sizeof(SiftExtent));
	if ((int) ninline < next)
		*extlist = sift_extlist_write(rel, h->part_gen, h->part_id, ext + ninline, next - ninline);
	h->total_extents = next;
	h->ninline_extents = ninline;
	h->extlist_blk = *extlist;
	h->inline_meta_off = meta;
	SiftPageSetUsed(page, fixed + ninline * sizeof(SiftExtent) + h->inline_meta_len);
	sift_page_seal(page);
	blk = sift_fsm_alloc_meta_page(rel);
	x = sift_xlog_start(rel);
	memcpy(sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE), page, BLCKSZ);
	sift_xlog_done(&x);
	return blk;
}

SiftPartMove
sift_part_move_tail(Relation rel, const SiftPart *part, BlockNumber cut, const SiftExtent *to, int nto)
{
	int			n = part->hdr.total_extents,
				next,
				nfreed = 0,
				nnew = 0,
				nlist = 0;
	/* check-memory: allow the extents of one part, which the size of the part bounds */
	SiftExtent *ext = palloc((2 * n + nto + 1) * sizeof(SiftExtent));
	SiftExtent *freed = palloc((n + part->nextlist_pages + 2) * sizeof(SiftExtent));
	SiftPartMove mv = {0};
	uint32		ninline;
	BlockNumber extlist,
			   *list_pages = NULL;

	Assert(!part->is_packed && part->remote == NULL);
	next = move_stream_pieces(rel, part, cut, to, nto, ext, freed, &nfreed);
	mv.header_blk = write_moved_header(rel, part, ext, next, &extlist, &ninline);
	freed[nfreed++] = (SiftExtent) {part->header_blk, 1};
	for (int i = 0; i < part->nextlist_pages; i++)
		freed[nfreed++] = (SiftExtent) {part->extlist_pages[i], 1};
	if (extlist != InvalidBlockNumber)
		pfree(sift_extlist_read(rel, extlist, part->hdr.part_gen, part->hdr.part_id, next - (int) ninline, &list_pages, &nlist));
	mv.made = palloc((1 + nlist + nto) * sizeof(SiftExtent));
	mv.made[nnew++] = (SiftExtent) {mv.header_blk, 1};
	for (int i = 0; i < nlist; i++)
		mv.made[nnew++] = (SiftExtent) {list_pages[i], 1};
	for (int i = 0; i < nto; i++)
		mv.made[nnew++] = to[i];
	mv.nmade = nnew;
	mv.freed = freed;
	mv.nfreed = nfreed;
	pfree(ext);
	return mv;
}
