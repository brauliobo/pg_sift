/*
 * sift_wbuf.c - write buffers (DESIGN.md (c)): aminsert appends doc
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

#include "sift_am.h"
#include "sift_lane.h"
#include "sift_maint.h"
#include "sift_wbuf.h"

#define corrupt SIFT_CORRUPT

#define WBUFPAGE(page)	((SiftWbufPageHeaderData *) SiftPageGetData(page))
#define RECS(page)		(SiftPageGetData(page) + sizeof(SiftWbufPageHeaderData))
#define ROOM(page)		(SIFT_PAGE_PAYLOAD - SiftPageGetUsed(page))
#define RECSIZE(nterms, termbytes) \
	TYPEALIGN(4, sizeof(SiftWbufDocData) + (Size) (nterms) * 6 + (termbytes))

/* ---------- bloom ---------- */

static inline void
bloom_add(uint64 *bloom, uint32 h)
{
	uint32		a = h & 255,
				b = (h >> 8) & 255;

	bloom[a >> 6] |= UINT64CONST(1) << (a & 63);
	bloom[b >> 6] |= UINT64CONST(1) << (b & 63);
}

bool
sift_wbuf_bloom_has(const uint64 *bloom, uint32 h)
{
	uint32		a = h & 255,
				b = (h >> 8) & 255;

	return ((bloom[a >> 6] >> (a & 63)) & (bloom[b >> 6] >> (b & 63)) & 1) != 0;
}

/* ---------- segments ---------- */

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
	return sift_lane_seal(index, force, sizeof(SiftWbufPageHeaderData), limit_pages(index), has_docs);
}

/* ---------- append ---------- */

/* Cached per relation: the active segment and the lane tails last seen */
typedef struct WbufHint
{
	uint64		gen;			/* 0: unknown */
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
	sift_lane_refresh(index, analyzer_hash, "analyzer options", sizeof(SiftWbufPageHeaderData), sift_maint_make_room,
					  &h->gen, &h->hdr, h->tail);
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
		bloom_add(ph->bloom, t[i].hash);
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
} PieceArg;

static void
fill_piece(Page cp, Page np, Page hp, void *arg)
{
	PieceArg   *a = arg;

	put_piece(cp, a->tid, a->dl, a->cont, a->t, a->n, a->termbytes);
	if (a->spans)
		WBUFPAGE(cp)->flags |= SIFT_WBUFPAGE_SPANS;
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
			GenericXLogState *state = GenericXLogStart(index);

			put_piece(GenericXLogRegisterBuffer(state, cur, 0), tid, dl, !first, t + i, n, tb);
			GenericXLogFinish(state);
			break;
		}
		/* a doc that fits an empty page moves there whole rather than splitting */
		place = n > 0 && !(first && RECSIZE(nterms, whole_bytes) <= SIFT_PAGE_PAYLOAD - sizeof(SiftWbufPageHeaderData));
		pa = (PieceArg) {tid, dl, !first, t + i, n, tb, i + n < nterms};
		cur = sift_lane_grow(index, h->gen, h->hdr, lane, cur, sizeof(SiftWbufPageHeaderData), limit_pages(index),
							 place ? fill_piece : NULL, &pa, &crossed);
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

/* ---------- reading ---------- */

struct SiftWbufReader
{
	Relation	index;
	SiftSegEntry entry;
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

SiftWbufReader *
sift_wbuf_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt, SiftWbufPageFilter filter,
			  void *filter_arg)
{
	SiftWbufReader *r = MemoryContextAllocZero(mcxt, sizeof(SiftWbufReader));

	r->index = index;
	r->entry = *entry;
	r->mcxt = mcxt;
	r->filter = filter;
	r->filter_arg = filter_arg;
	r->page = MemoryContextAlloc(mcxt, BLCKSZ);
	sift_lane_iter_init(&r->it, index, &r->entry);
	return r;
}

void
sift_wbuf_close(SiftWbufReader *r)
{
	pfree(r->page);
	pfree(r);
}

void
sift_wbuf_totals(const SiftWbufReader *r, uint64 *ndocs, uint64 *sum_dl)
{
	*ndocs = r->ndocs;
	*sum_dl = r->sum_dl;
}

/* Loads the next page of the segment; a page that may span is never filtered out unless force is false and the filter says so */
static bool
advance_page(SiftWbufReader *r, bool force)
{
	for (;;)
	{
		SiftWbufPageHeaderData *ph;

		if (!sift_lane_iter_next(&r->it, r->page, sizeof(SiftWbufPageHeaderData)))
			return false;
		ph = WBUFPAGE(r->page);
		r->pos = 0;
		r->used = SiftPageGetUsed(r->page) - sizeof(SiftWbufPageHeaderData);
		r->pflags = ph->flags;
		r->ndocs += ph->ndocs;
		r->sum_dl += ph->sum_dl;
		if (force || !r->filter || (ph->flags & SIFT_WBUFPAGE_SPANS) || r->filter(r->filter_arg, ph->bloom))
			return true;
		CHECK_FOR_INTERRUPTS();
	}
}

/* The record at r->pos, checked; *size its length */
static const SiftWbufDocData *
rec_at(SiftWbufReader *r, Size *size)
{
	const SiftWbufDocData *d = (const SiftWbufDocData *) (RECS(r->page) + r->pos);
	const uint8 *len;
	int			sum = 0;

	if (r->pos + sizeof(SiftWbufDocData) > r->used)
		corrupt(r->index, "A record header at byte %zu overruns the %zu used bytes of a write buffer page.", r->pos, r->used);
	*size = RECSIZE(d->nterms, d->termbytes);
	if (r->pos + *size > r->used)
		corrupt(r->index, "A record of %zu bytes at byte %zu overruns the %zu used bytes of a write buffer page.",
				*size, r->pos, r->used);
	len = (const uint8 *) (d + 1) + d->nterms * 5;
	for (int i = 0; i < d->nterms; i++)
		sum += len[i];
	if (sum != d->termbytes)
		corrupt(r->index, "A write buffer record lists %u term bytes but its terms add up to %d.", d->termbytes, sum);
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

/* ---------- pages ---------- */

SiftExtent *
sift_wbuf_pages(Relation index, const SiftSegEntry *entry, int *n)
{
	return sift_lane_extents(index, entry, n);
}

/* ---------- VACUUM ---------- */

static int
collect_docs(void *arg, Page page, Size *offs, ItemPointerData *tids)
{
	SiftWbufReader *r = arg;
	int			n = 0;

	r->pos = 0;
	r->used = SiftPageGetUsed(page) - sizeof(SiftWbufPageHeaderData);
	while (r->pos < r->used)
	{
		Size		size;
		const SiftWbufDocData *d = rec_at(r, &size);

		if (!(d->flags & (SIFT_WBUF_CONT | SIFT_WBUF_DELETED)))
		{
			offs[n] = r->pos;
			tids[n++] = d->tid;
		}
		r->pos += size;
	}
	return n;
}

static void
flag_doc(Page page, Size off)
{
	((SiftWbufDocData *) (RECS(page) + off))->flags |= SIFT_WBUF_DELETED;
}

int64
sift_wbuf_bulkdelete(Relation index, const SiftSegEntry *entry, IndexBulkDeleteCallback cb, void *cbstate,
					IndexBulkDeleteResult *stats)
{
	SiftWbufReader *r = sift_wbuf_open(index, entry, CurrentMemoryContext, NULL, NULL);
	int64		removed = sift_lane_bulkdelete(index, entry, sizeof(SiftWbufPageHeaderData), sizeof(SiftWbufDocData), r->page,
											   collect_docs, flag_doc, r, cb, cbstate, stats);

	sift_wbuf_close(r);
	return removed;
}

/* ---------- verification ---------- */

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
				bloom[4] = {0};

	memcpy(r->page, page, BLCKSZ);
	r->pos = 0;
	r->used = SiftPageGetUsed(page) - sizeof(SiftWbufPageHeaderData);
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
			bloom_add(bloom, hash[i]);
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
}

uint64
sift_wbuf_verify(Relation index, const SiftSegEntry *entry)
{
	VerifyState v = {sift_wbuf_open(index, entry, CurrentMemoryContext, NULL, NULL), entry->gen, 0};

	sift_lane_verify(index, entry, sizeof(SiftWbufPageHeaderData), verify_page, &v);
	sift_wbuf_close(v.r);
	return v.live;
}

static bool
no_page(void *arg, const uint64 *bloom)
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
