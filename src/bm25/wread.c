/*
 * bm25/wread.c - reads of a write buffer: the docs of its pages, their deletion by VACUUM, the check of their structure and their sums
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

#include "bm25/am.h"
#include "core/lane.h"
#include "core/maint/maint.h"
#include "core/memtrack.h"
#include "bm25/wbuf.h"
#include "bm25/wbuf_int.h"

#define corrupt SIFT_CORRUPT

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
	Size		nasm;
	Size		asmcap;
	Size		asmbytes;
	Size		asmbytescap;
	uint32	   *ahash;
	uint8	   *atf;
	uint8	   *alen;
	char	   *aterms;
	uint16		afdl[SIFT_MAX_FIELDS];
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
	if (r->ahash)
	{
		pfree(r->ahash);
		pfree(r->atf);
		pfree(r->alen);
	}
	if (r->aterms)
		pfree(r->aterms);
	pfree(r->page);
	pfree(r);
}

void
sift_wbuf_note_walked(const SiftWbufReader *r)
{
	if (r->entry.kind == SIFT_SEG_WBUF_OPEN)
		sift_maint_wbuf_walked(r->index, r->entry.gen, r->nwalked, r->ndocs);
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
	if (REC_NFIELDS(d) > SIFT_MAX_FIELDS)
		corrupt(r->index, "A write buffer record has the lengths of %u fields.", REC_NFIELDS(d));
	*size = RECSIZE(d->nterms, d->termbytes, REC_NFIELDS(d));
	if (r->pos + *size > r->used)
		corrupt(r->index, "A record of %zu bytes at byte %zu overruns the %zu used bytes of a write buffer page.",
				*size, r->pos, r->used);
	len = (const uint8 *) REC_HASH(d) + d->nterms * 5;
	sum = sift_sum_u8(len, d->nterms);
	if (sum != d->termbytes)
		corrupt(r->index, "A write buffer record lists %u term bytes but its terms add up to %u.", d->termbytes, sum);
	return d;
}

static void
asm_piece(SiftWbufReader *r, const SiftWbufDocData *d)
{
	const uint32 *hash = REC_HASH(d);
	const uint8 *tf = (const uint8 *) (hash + d->nterms),
			   *len = tf + d->nterms;

	if (r->nasm + d->nterms > r->asmcap)
	{
		r->asmcap = Max(2 * r->asmcap, r->nasm + d->nterms);
		SIFT_REGROW_HUGE_IN(r->mcxt, r->ahash, r->asmcap * sizeof(uint32));
		SIFT_REGROW_HUGE_IN(r->mcxt, r->atf, r->asmcap);
		SIFT_REGROW_HUGE_IN(r->mcxt, r->alen, r->asmcap);
	}
	if (r->asmbytes + d->termbytes > r->asmbytescap)
	{
		r->asmbytescap = Max(2 * r->asmbytescap, r->asmbytes + d->termbytes);
		SIFT_REGROW_HUGE_IN(r->mcxt, r->aterms, r->asmbytescap);
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
		doc->field_dl = (d->flags & SIFT_WBUF_FIELDDL) ? (const uint16 *) (d + 1) : NULL;
		doc->nfields = REC_NFIELDS(d);
		if (r->pos < r->used || !(r->pflags & SIFT_WBUFPAGE_SPANS) || !BlockNumberIsValid(r->it.nextblk))
		{
			const uint32 *hash = REC_HASH(d);

			doc->nterms = d->nterms;
			doc->hash = hash;
			doc->tf = (const uint8 *) (hash + d->nterms);
			doc->len = doc->tf + d->nterms;
			doc->terms = (const char *) (doc->len + d->nterms);
			return true;
		}
		/* the doc goes on in the next page; the copy of this one is about to be replaced */
		r->nasm = r->asmbytes = 0;
		if (doc->field_dl)
		{
			memcpy(r->afdl, doc->field_dl, REC_NFIELDS(d) * sizeof(uint16));
			doc->field_dl = r->afdl;
		}
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

bool
sift_wbuf_has_docs(Relation index, const SiftSegEntry *entry)
{
	SiftWbufReader *r = sift_wbuf_open(index, entry, CurrentMemoryContext, NULL, NULL);
	SiftWbufDoc	doc;
	bool		any = sift_wbuf_next(r, &doc);

	sift_wbuf_close(r);
	return any;
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
		const uint32 *hash = REC_HASH(d);

		if (!ItemPointerIsValid(&d->tid) || (d->flags & ~(SIFT_WBUF_DELETED | SIFT_WBUF_CONT | SIFT_WBUF_FIELDDL)))
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
