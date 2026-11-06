/*
 * bm25/wbuf.c - write buffers: aminsert appends doc
 * records to one of SIFT_LANES lane chains of the active segment, each append
 * one generic WAL record. A full active segment is sealed in one directory
 * record and promoted to an immutable one by maintenance.
 *
 * Lock order: lane tail page, newly allocated page, header page. Sealing
 * takes lane tails one at a time and never the header. A doc that does not
 * fit the tail is split into pieces written while the tail is held (lock
 * coupling to the new page), so one doc is always adjacent in one lane.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "common/int.h"
#include "nodes/execnodes.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "bm25/am.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/wbuf.h"

bool		sift_test_wbuf_narrow = false;

#define corrupt SIFT_CORRUPT

#define WBUFPAGE(page)	((SiftWbufPageHeaderData *) SiftPageGetData(page))
#define RECS(page, hs)	(SiftPageGetData(page) + (hs))
#define ROOM(page)		(SIFT_PAGE_PAYLOAD - SiftPageGetUsed(page))
#define RECSIZE(nterms, termbytes) \
	TYPEALIGN(4, sizeof(SiftWbufDocData) + (Size) (nterms) * 6 + (termbytes))

/* The header of a page of a buffer of this format: the buffers of format 0 have the 256-bit bloom in it, those of SIFT_WBUF_FORMAT_WIDE a bloom of 8,192 bits after it */
static inline Size
hdr_size(uint16 format)
{
	return format >= SIFT_WBUF_FORMAT_WIDE ? sizeof(SiftWbufWideHeaderData) : sizeof(SiftWbufPageHeaderData);
}

/* Two probes in a bloom of nwords words (a power of two): the low bits of the hash, then the ones above them */
static inline void
bloom_probes(int nwords, uint32 h, uint32 *a, uint32 *b)
{
	uint32		bits = (uint32) nwords * 64;

	*a = h & (bits - 1);
	*b = (h >> pg_leftmost_one_pos32(bits)) & (bits - 1);
}

static inline void
bloom_add(uint64 *bloom, int nwords, uint32 h)
{
	uint32		a,
				b;

	bloom_probes(nwords, h, &a, &b);
	bloom[a >> 6] |= UINT64CONST(1) << (a & 63);
	bloom[b >> 6] |= UINT64CONST(1) << (b & 63);
}

bool
sift_wbuf_bloom_has(const uint64 *bloom, int nwords, uint32 h)
{
	uint32		a,
				b;

	bloom_probes(nwords, h, &a, &b);
	return ((bloom[a >> 6] >> (a & 63)) & (bloom[b >> 6] >> (b & 63)) & 1) != 0;
}

/* the format a buffer opened now has */
static uint16
new_format(void)
{
	return sift_test_wbuf_narrow ? 0 : SIFT_WBUF_FORMAT_WIDE;
}

static uint32
limit_pages(Relation index)
{
	return Max(2 * SIFT_LANES, (uint64) sift_index_options(index)->write_buffer_limit * 1024 / BLCKSZ);
}

static bool has_docs(Relation index, const SiftSegEntry *entry);

void
sift_wbuf_flag_sealed(Relation index, const SiftSegEntry *entry)
{
	sift_lane_flag_sealed(index, entry);
}

uint64
sift_wbuf_seal(Relation index, bool force)
{
	return sift_lane_seal(index, force, hdr_size(new_format()), new_format(), 0, limit_pages(index), has_docs);
}

/*
 * VACUUM seals an open buffer once it holds a quarter of write_buffer_limit, as for a sift_col index: a search reads every document of the open buffer (and the heap
 * pages of those not yet all-visible), so the 4 MB a buffer may reach cost a search many times what the index needs after a round of updates (300,000 documents,
 * 16 MB: 684 blocks and 1.6 ms for a search at 4 MB, 186 and 0.54 ms when the VACUUM of the round seals it), and its rows are promoted and merged instead of waiting
 * for a seal that may never come.
 */
uint64
sift_wbuf_seal_vacuum(Relation index)
{
	return sift_lane_seal(index, false, hdr_size(new_format()), new_format(), 0, Max(limit_pages(index) / 4, 2 * SIFT_LANES), has_docs);
}

/* Cached per relation: the active segment and the lane tails last seen */
typedef struct WbufHint
{
	uint64		gen;			/* 0: unknown */
	uint16		format;			/* of that buffer */
	BlockNumber hdr;
	BlockNumber tail[SIFT_LANES];
} WbufHint;

static WbufHint *
get_hint(Relation index)
{
	if (index->rd_amcache == NULL)
		index->rd_amcache = MemoryContextAllocZero(index->rd_indexcxt, sizeof(WbufHint));
	return index->rd_amcache;
}

static void
refresh_hint(Relation index, WbufHint *h, uint32 analyzer_hash)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	sift_lane_refresh(index, analyzer_hash, "analyzer options", hdr_size(new_format()), new_format(), 0, sift_maint_make_room,
					  &h->gen, &h->hdr, h->tail);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].gen == h->gen)
			h->format = snap->segs[i].format;
	pfree(snap);
}

/* Largest n whose piece of terms[0..n) fits room; -1 when not even an empty piece does */
static int
fit(Size room, const SiftTermFreq *t, int nterms, int *termbytes)
{
	int			n = 0,
				tb = 0;

	*termbytes = 0;
	if (sizeof(SiftWbufDocData) > room)
		return -1;
	while (n < nterms && RECSIZE(n + 1, tb + t[n].len) <= room)
		tb += t[n++].len;
	*termbytes = tb;
	return n;
}

static void
put_piece(Page page, ItemPointer tid, uint32 dl, bool cont, const SiftTermFreq *t, int n, int termbytes)
{
	SiftWbufPageHeaderData *ph = WBUFPAGE(page);
	Size		used = SiftPageGetUsed(page),
				size = RECSIZE(n, termbytes);
	SiftWbufDocData *d = (SiftWbufDocData *) (SiftPageGetData(page) + used);
	uint32	   *hash = (uint32 *) (d + 1);
	uint8	   *tf = (uint8 *) (hash + n),
			   *len = tf + n;
	char	   *tb = (char *) (len + n);

	memset(d, 0, size);
	d->tid = *tid;
	d->flags = cont ? SIFT_WBUF_CONT : 0;
	d->nterms = n;
	d->dl = cont ? 0 : dl;
	d->termbytes = termbytes;
	for (int i = 0; i < n; i++)
	{
		hash[i] = t[i].hash;
		tf[i] = t[i].tf;
		len[i] = t[i].len;
		memcpy(tb, t[i].term, t[i].len);
		tb += t[i].len;
		bloom_add(ph->bloom, 4, t[i].hash);
	}
	if (!cont)
	{
		ph->ndocs++;
		ph->sum_dl += dl;
	}
	SiftPageSetUsed(page, used + size);
}

typedef struct PieceArg
{
	ItemPointer tid;
	uint32		dl;
	bool		cont;
	const SiftTermFreq *t;
	int			n;
	int			termbytes;
	bool		spans;			/* the doc goes on in the next page */
	uint16		format;
	bool		place;			/* the piece goes on the page being left */
} PieceArg;

/* The page is full: the terms of its records go to the wide bloom (the append of a record changes the head of the page, whose log record grows with the bytes it changes) */
static void
fill_wide(Page page)
{
	SiftWbufWideHeaderData *ph = (SiftWbufWideHeaderData *) WBUFPAGE(page);

	memset(ph->wide, 0, sizeof(ph->wide));
	for (Size pos = sizeof(SiftWbufWideHeaderData); pos < SiftPageGetUsed(page);)
	{
		const SiftWbufDocData *d = (const SiftWbufDocData *) (SiftPageGetData(page) + pos);
		const uint32 *hash = (const uint32 *) (d + 1);

		for (int i = 0; i < d->nterms; i++)
			bloom_add(ph->wide, SIFT_WBUF_WIDE_WORDS, hash[i]);
		pos += RECSIZE(d->nterms, d->termbytes);
	}
	ph->base.flags |= SIFT_WBUFPAGE_WIDE;
}

static void
fill_piece(Page cp, Page np, Page hp, void *arg)
{
	PieceArg   *a = arg;

	if (a->place)
	{
		put_piece(cp, a->tid, a->dl, a->cont, a->t, a->n, a->termbytes);
		if (a->spans)
			WBUFPAGE(cp)->flags |= SIFT_WBUFPAGE_SPANS;
	}
	if (a->format >= SIFT_WBUF_FORMAT_WIDE)
		fill_wide(cp);
}

/*
 * Writes the doc starting at the locked tail cur; returns whether the segment
 * reached write_buffer_limit. Leaves the final tail block in *tail.
 */
static bool
write_doc(Relation index, WbufHint *h, int lane, Buffer cur, ItemPointer tid, const SiftTermFreq *t,
		  int nterms, uint32 dl, BlockNumber *tail)
{
	bool		crossed = false,
				first = true;
	int			i = 0,
				whole_bytes = 0;

	for (int k = 0; k < nterms; k++)
		whole_bytes += t[k].len;
	for (;;)
	{
		int			rem = nterms - i,
					tb,
					n = fit(ROOM(BufferGetPage(cur)), t + i, rem, &tb);
		bool		place;
		PieceArg	pa;

		if (n == rem)
		{
			SiftPageEdit ed;

			sift_page_edit_begin(&ed, cur, SIFT_PAGE_DATA_OFFSET + sizeof(SiftWbufPageHeaderData));
			put_piece(ed.page, tid, dl, !first, t + i, n, tb);
			sift_page_edit_end(index, &ed, 1);
			break;
		}
		/* a doc that fits an empty page moves there whole rather than splitting */
		place = n > 0 && !(first && RECSIZE(nterms, whole_bytes) <= SIFT_PAGE_PAYLOAD - hdr_size(h->format));
		pa = (PieceArg) {tid, dl, !first, t + i, n, tb, i + n < nterms, h->format, place};
		cur = sift_lane_grow(index, h->gen, h->hdr, lane, cur, hdr_size(h->format), limit_pages(index),
							 fill_piece, &pa, &crossed);
		if (place)
		{
			first = false;
			i += n;
		}
	}
	*tail = BufferGetBlockNumber(cur);
	UnlockReleaseBuffer(cur);
	return crossed;
}

bool
sift_wbuf_append(Relation index, ItemPointer tid, const SiftTermFreq *terms, int nterms, uint32 dl)
{
	WbufHint    *h = get_hint(index);
	SiftAnalyzerConfig cfg;
	Buffer		buf;
	int			lane;
	BlockNumber tail;
	bool		crossed;

	for (;;)
	{
		if (h->gen == 0)
		{
			sift_analyzer_config_from_index(index, &cfg);
			refresh_hint(index, h, cfg.hash);
		}
		if (BufferIsValid(buf = sift_lane_lock_tail(index, h->gen, h->tail, SIFT_LANES, &lane)))
			break;
		h->gen = 0;
	}
	crossed = write_doc(index, h, lane, buf, tid, terms, nterms, dl, &tail);
	h->tail[lane] = tail;
	return crossed;
}

bool
sift_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
			  IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	SiftAnalyzer *an = indexInfo->ii_AmCache;
	SiftTermFreq *terms;
	uint32		dl;
	int			n;
	text	   *t;

	if (isnull[0])
		return false;
	if (an == NULL)
	{
		SiftAnalyzerConfig cfg;

		sift_analyzer_config_from_index(index, &cfg);
		an = indexInfo->ii_AmCache = sift_analyzer_create(&cfg, indexInfo->ii_Context);
	}
	t = DatumGetTextPP(values[0]);
	n = sift_analyze_doc(an, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &terms, &dl);
	if (sift_wbuf_append(index, heap_tid, terms, n, dl))
		sift_maint_after_seal(index);
	sift_analyzer_reset(an);
	return false;
}

struct SiftWbufReader
{
	Relation	index;
	SiftSegEntry entry;
	Size		hs;				/* the header of its pages */
	bool		force;			/* the page being loaded is read whatever the filter says */
	MemoryContext mcxt;
	SiftWbufPageFilter filter;
	void	   *filter_arg;
	SiftLaneIter it;
	Page		page;			/* copy of the current page */
	Size		pos;			/* record offset in the page */
	Size		used;
	uint16		pflags;
	uint64		ndocs;
	uint64		sum_dl;
	uint64		nwalked;		/* the docs of the pages that were loaded, not skipped for their bloom */
	/* terms of a doc assembled from pieces */
	int			nasm;
	int			asmcap;
	int			asmbytes;
	int			asmbytescap;
	uint32	   *ahash;
	uint8	   *atf;
	uint8	   *alen;
	char	   *aterms;
};

static bool page_ruled_out(void *arg, Page page);

SiftWbufReader *
sift_wbuf_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt, SiftWbufPageFilter filter,
			  void *filter_arg)
{
	SiftWbufReader *r = MemoryContextAllocZero(mcxt, sizeof(SiftWbufReader));

	r->index = index;
	r->entry = *entry;
	r->hs = hdr_size(entry->format);
	r->mcxt = mcxt;
	r->filter = filter;
	r->filter_arg = filter_arg;
	r->page = MemoryContextAlloc(mcxt, BLCKSZ);
	sift_lane_iter_init(&r->it, index, &r->entry);
	r->it.skip = page_ruled_out;
	r->it.skip_arg = r;
	return r;
}

void
sift_wbuf_close(SiftWbufReader *r)
{
	pfree(r->page);
	pfree(r);
}

void
sift_wbuf_note_walked(const SiftWbufReader *r)
{
	if (r->entry.kind == SIFT_SEG_WBUF_OPEN)
		sift_maint_wbuf_walked(r->index, r->entry.gen, r->nwalked);
}

void
sift_wbuf_totals(const SiftWbufReader *r, uint64 *ndocs, uint64 *sum_dl)
{
	*ndocs = r->ndocs;
	*sum_dl = r->sum_dl;
}

/* On the page in the buffer: does the bloom of the page rule its docs out? (a page that may span is never ruled out, and none when the page is forced) */
static bool
page_ruled_out(void *arg, Page page)
{
	SiftWbufReader *r = arg;
	const SiftWbufPageHeaderData *ph = WBUFPAGE(page);

	if (r->force || !r->filter || (ph->flags & SIFT_WBUFPAGE_SPANS))
		return false;
	return !((ph->flags & SIFT_WBUFPAGE_WIDE) ? r->filter(r->filter_arg, ((const SiftWbufWideHeaderData *) ph)->wide, SIFT_WBUF_WIDE_WORDS) :
			 r->filter(r->filter_arg, ph->bloom, 4));
}

/* Loads the next page of the segment; a page that may span is never filtered out unless force is false and the filter says so, and a page the filter rules out is not copied */
static bool
advance_page(SiftWbufReader *r, bool force)
{
	r->force = force;
	for (;;)
	{
		SiftWbufPageHeaderData *ph;

		if (!sift_lane_iter_next(&r->it, r->page, r->hs))
			return false;
		ph = WBUFPAGE(r->page);
		r->pos = 0;
		r->used = SiftPageGetUsed(r->page) - r->hs;
		r->pflags = ph->flags;
		r->ndocs += ph->ndocs;
		r->sum_dl += ph->sum_dl;
		if (!r->it.skipped)
		{
			r->nwalked += ph->ndocs;
			return true;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/* The record at r->pos, checked; *size its length */
static const SiftWbufDocData *
rec_at(SiftWbufReader *r, Size *size)
{
	const SiftWbufDocData *d = (const SiftWbufDocData *) (RECS(r->page, r->hs) + r->pos);
	const uint8 *len;
	uint32		sum;

	if (r->pos + sizeof(SiftWbufDocData) > r->used)
		corrupt(r->index, "A record header at byte %zu overruns the %zu used bytes of a write buffer page.", r->pos, r->used);
	*size = RECSIZE(d->nterms, d->termbytes);
	if (r->pos + *size > r->used)
		corrupt(r->index, "A record of %zu bytes at byte %zu overruns the %zu used bytes of a write buffer page.",
				*size, r->pos, r->used);
	len = (const uint8 *) (d + 1) + d->nterms * 5;
	sum = sift_sum_u8(len, d->nterms);
	if (sum != d->termbytes)
		corrupt(r->index, "A write buffer record lists %u term bytes but its terms add up to %u.", d->termbytes, sum);
	return d;
}

static void
asm_piece(SiftWbufReader *r, const SiftWbufDocData *d)
{
	const uint32 *hash = (const uint32 *) (d + 1);
	const uint8 *tf = (const uint8 *) (hash + d->nterms),
			   *len = tf + d->nterms;

	if (r->nasm + d->nterms > r->asmcap)
	{
		MemoryContext old = MemoryContextSwitchTo(r->mcxt);

		r->asmcap = Max(2 * r->asmcap, r->nasm + d->nterms);
		r->ahash = r->ahash ? repalloc(r->ahash, r->asmcap * sizeof(uint32)) : palloc(r->asmcap * sizeof(uint32));
		r->atf = r->atf ? repalloc(r->atf, r->asmcap) : palloc(r->asmcap);
		r->alen = r->alen ? repalloc(r->alen, r->asmcap) : palloc(r->asmcap);
		MemoryContextSwitchTo(old);
	}
	if (r->asmbytes + d->termbytes > r->asmbytescap)
	{
		r->asmbytescap = Max(2 * r->asmbytescap, r->asmbytes + d->termbytes);
		r->aterms = r->aterms ? repalloc(r->aterms, r->asmbytescap) : MemoryContextAlloc(r->mcxt, r->asmbytescap);
	}
	memcpy(r->ahash + r->nasm, hash, d->nterms * sizeof(uint32));
	memcpy(r->atf + r->nasm, tf, d->nterms);
	memcpy(r->alen + r->nasm, len, d->nterms);
	memcpy(r->aterms + r->asmbytes, len + d->nterms, d->termbytes);
	r->nasm += d->nterms;
	r->asmbytes += d->termbytes;
}

bool
sift_wbuf_next(SiftWbufReader *r, SiftWbufDoc *doc)
{
	for (;;)
	{
		const SiftWbufDocData *d;
		Size		size;

		if (r->pos >= r->used)
		{
			if (!advance_page(r, false))
				return false;
			continue;
		}
		d = rec_at(r, &size);
		if (d->flags & SIFT_WBUF_CONT)
			corrupt(r->index, "A continuation record of segment %llu has no first piece.", (unsigned long long) r->entry.gen);
		doc->tid = d->tid;
		doc->dl = d->dl;
		doc->deleted = (d->flags & SIFT_WBUF_DELETED) != 0;
		r->pos += size;
		if (r->pos < r->used || !(r->pflags & SIFT_WBUFPAGE_SPANS) || !BlockNumberIsValid(r->it.nextblk))
		{
			const uint32 *hash = (const uint32 *) (d + 1);

			doc->nterms = d->nterms;
			doc->hash = hash;
			doc->tf = (const uint8 *) (hash + d->nterms);
			doc->len = doc->tf + d->nterms;
			doc->terms = (const char *) (doc->len + d->nterms);
			return true;
		}
		/* the doc goes on in the next page; the copy of this one is about to be replaced */
		r->nasm = r->asmbytes = 0;
		for (;;)
		{
			asm_piece(r, d);
			if (r->pos < r->used || !(r->pflags & SIFT_WBUFPAGE_SPANS) || !advance_page(r, true) || r->pos >= r->used)
				break;
			d = rec_at(r, &size);
			if (!(d->flags & SIFT_WBUF_CONT))
				break;
			r->pos += size;
		}
		doc->nterms = r->nasm;
		doc->hash = r->ahash;
		doc->tf = r->atf;
		doc->len = r->alen;
		doc->terms = r->aterms;
		return true;
	}
}

static bool
has_docs(Relation index, const SiftSegEntry *entry)
{
	SiftWbufReader *r = sift_wbuf_open(index, entry, CurrentMemoryContext, NULL, NULL);
	SiftWbufDoc	doc;
	bool		any = sift_wbuf_next(r, &doc);

	sift_wbuf_close(r);
	return any;
}

SiftExtent *
sift_wbuf_pages(Relation index, const SiftSegEntry *entry, int *n)
{
	return sift_lane_extents(index, entry, n);
}

static int
collect_docs(void *arg, Page page, Size *offs, ItemPointerData *tids)
{
	SiftWbufReader *r = arg;
	int			n = 0;

	r->pos = 0;
	r->used = SiftPageGetUsed(page) - r->hs;
	while (r->pos < r->used)
	{
		Size		size;
		const SiftWbufDocData *d = rec_at(r, &size);

		if (!(d->flags & (SIFT_WBUF_CONT | SIFT_WBUF_DELETED)))
		{
			offs[n] = r->hs + r->pos;
			tids[n++] = d->tid;
		}
		r->pos += size;
	}
	return n;
}

static void
flag_doc(Page page, Size off)
{
	((SiftWbufDocData *) (SiftPageGetData(page) + off))->flags |= SIFT_WBUF_DELETED;
}

int64
sift_wbuf_bulkdelete(Relation index, const SiftSegEntry *entry, IndexBulkDeleteCallback cb, void *cbstate,
					IndexBulkDeleteResult *stats)
{
	SiftWbufReader *r = sift_wbuf_open(index, entry, CurrentMemoryContext, NULL, NULL);
	int64		removed = sift_lane_bulkdelete(index, entry, r->hs, sizeof(SiftWbufDocData), r->page,
											   collect_docs, flag_doc, r, cb, cbstate, stats);

	sift_wbuf_close(r);
	return removed;
}

typedef struct VerifyState
{
	SiftWbufReader *r;
	uint64		gen;
	uint64		live;
} VerifyState;

static void
verify_page(void *arg, Page page, BlockNumber blk)
{
	VerifyState *v = arg;
	SiftWbufReader *r = v->r;
	SiftWbufPageHeaderData *ph = WBUFPAGE(page);
	uint64		ndocs = 0,
				sum_dl = 0,
				bloom[4] = {0},
				wide[SIFT_WBUF_WIDE_WORDS] = {0};

	memcpy(r->page, page, BLCKSZ);
	r->pos = 0;
	r->used = SiftPageGetUsed(page) - r->hs;
	while (r->pos < r->used)
	{
		Size		size;
		const SiftWbufDocData *d = rec_at(r, &size);
		const uint32 *hash = (const uint32 *) (d + 1);

		if (!ItemPointerIsValid(&d->tid) || (d->flags & ~(SIFT_WBUF_DELETED | SIFT_WBUF_CONT)))
			corrupt(r->index, "A record of write buffer %llu has tid (%u,%u) and flags %x.",
					(unsigned long long) v->gen, ItemPointerGetBlockNumberNoCheck(&d->tid),
					ItemPointerGetOffsetNumberNoCheck(&d->tid), d->flags);
		for (int i = 0; i < d->nterms; i++)
		{
			bloom_add(bloom, 4, hash[i]);
			bloom_add(wide, SIFT_WBUF_WIDE_WORDS, hash[i]);
		}
		if (!(d->flags & SIFT_WBUF_CONT))
		{
			ndocs++;
			sum_dl += d->dl;
			v->live += !(d->flags & SIFT_WBUF_DELETED);
		}
		r->pos += size;
	}
	if (ndocs != ph->ndocs || sum_dl != ph->sum_dl)
		corrupt(r->index, "Write buffer block %u holds %llu docs and %llu tokens, its header %u and %u.", blk,
				(unsigned long long) ndocs, (unsigned long long) sum_dl, ph->ndocs, ph->sum_dl);
	for (int i = 0; i < 4; i++)
		if (bloom[i] & ~ph->bloom[i])
			corrupt(r->index, "The bloom filter of write buffer block %u misses a term of its records.", blk);
	if (ph->flags & SIFT_WBUFPAGE_WIDE)
		for (int i = 0; i < SIFT_WBUF_WIDE_WORDS; i++)
			if (wide[i] & ~((SiftWbufWideHeaderData *) ph)->wide[i])
				corrupt(r->index, "The wide bloom filter of write buffer block %u misses a term of its records.", blk);
}

uint64
sift_wbuf_verify(Relation index, const SiftSegEntry *entry)
{
	VerifyState v = {sift_wbuf_open(index, entry, CurrentMemoryContext, NULL, NULL), entry->gen, 0};

	sift_lane_verify(index, entry, v.r->hs, verify_page, &v);
	sift_wbuf_close(v.r);
	return v.live;
}

static bool
no_page(void *arg, const uint64 *bloom, int nwords)
{
	return false;
}

void
sift_wbuf_summary(Relation index, const SiftSegEntry *entry, uint64 *ndocs, uint64 *sum_dl, uint64 *npages)
{
	SiftWbufHeaderData hdr;
	SiftWbufReader *r = sift_wbuf_open(index, entry, CurrentMemoryContext, no_page, NULL);
	SiftWbufDoc	d;

	while (sift_wbuf_next(r, &d))
		;
	sift_wbuf_totals(r, ndocs, sum_dl);
	sift_wbuf_close(r);
	sift_lane_read_header(index, entry, &hdr);
	*npages = 1 + sift_lane_npages(&hdr);
}
