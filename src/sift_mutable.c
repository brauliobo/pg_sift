/*
 * sift_mutable.c - mutable segments (DESIGN.md (c)): aminsert appends doc
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
#include "sift_maint.h"
#include "sift_mutable.h"

#define corrupt(index, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), \
			 errdetail(__VA_ARGS__)))

#define MUTPAGE(page)	((SiftMutPageHeaderData *) SiftPageGetData(page))
#define MUTHDR(page)	((SiftMutHeaderData *) SiftPageGetData(page))
#define RECS(page)		(SiftPageGetData(page) + sizeof(SiftMutPageHeaderData))
#define ROOM(page)		(SIFT_PAGE_PAYLOAD - SiftPageGetUsed(page))
#define RECSIZE(nterms, termbytes) \
	TYPEALIGN(4, sizeof(SiftMutDocData) + (Size) (nterms) * 6 + (termbytes))

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
sift_mut_bloom_has(const uint64 *bloom, uint32 h)
{
	uint32		a = h & 255,
				b = (h >> 8) & 255;

	return ((bloom[a >> 6] >> (a & 63)) & (bloom[b >> 6] >> (b & 63)) & 1) != 0;
}

/* ---------- pages ---------- */

static void
init_data_page(Page page)
{
	memset(MUTPAGE(page), 0, sizeof(SiftMutPageHeaderData));
	MUTPAGE(page)->next = InvalidBlockNumber;
	SiftPageSetUsed(page, sizeof(SiftMutPageHeaderData));
}

static void
init_header_page(Page page, uint64 gen, BlockNumber first)
{
	SiftMutHeaderData *hdr = MUTHDR(page);

	hdr->gen = gen;
	for (int l = 0; l < SIFT_LANES; l++)
		hdr->lanes[l] = (SiftLaneData) {first + l, first + l, 1};
	SiftPageSetUsed(page, sizeof(SiftMutHeaderData));
}

static uint32
header_pages(const SiftMutHeaderData *hdr)
{
	uint32		n = 0;

	for (int l = 0; l < SIFT_LANES; l++)
		n += hdr->lanes[l].npages;
	return n;
}

static uint32
limit_pages(Relation index)
{
	return Max(2 * SIFT_LANES, (uint64) sift_index_options(index)->mutable_limit * 1024 / BLCKSZ);
}

static Buffer
read_locked(Relation index, BlockNumber blk, int mode)
{
	Buffer		buf = ReadBuffer(index, blk);

	LockBuffer(buf, mode);
	return buf;
}

static void
read_header(Relation index, const SiftSegEntry *e, SiftMutHeaderData *hdr)
{
	Buffer		buf = read_locked(index, e->header_blk, BUFFER_LOCK_SHARE);

	sift_store_check_page(index, buf, SIFT_PT_MUTHEADER, e->gen);
	*hdr = *MUTHDR(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
}

static const SiftSegEntry *
find_kind(const SiftDirSnapshot *snap, SiftSegKind kind)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == kind)
			return &snap->segs[i];
	return NULL;
}

static bool has_docs(Relation index, const SiftSegEntry *entry);

/* ---------- segment creation and sealing ---------- */

/* Allocates and initializes (logged) the pages of an empty active segment; not yet reachable */
static SiftSegEntry
new_segment(Relation index)
{
	uint64		gen = sift_store_new_gen(index);
	Buffer		bufs[SIFT_LANES + 1];
	BlockNumber first;
	SiftSegEntry e = {.gen = gen, .kind = SIFT_SEG_MUTABLE_ACTIVE};

	bufs[0] = sift_alloc_page(index, SIFT_PT_MUTHEADER, gen);
	first = sift_alloc_extent(index, SIFT_LANES, SIFT_PT_MUTDATA, gen, bufs + 1);
	e.header_blk = BufferGetBlockNumber(bufs[0]);
	for (int i = 0; i <= SIFT_LANES; i += MAX_GENERIC_XLOG_PAGES)
	{
		int			n = Min(MAX_GENERIC_XLOG_PAGES, SIFT_LANES + 1 - i);
		GenericXLogState *state = GenericXLogStart(index);

		for (int j = 0; j < n; j++)
		{
			Page		page = GenericXLogRegisterBuffer(state, bufs[i + j], 0);

			if (i + j == 0)
				init_header_page(page, gen, first);
			else
				init_data_page(page);
		}
		GenericXLogFinish(state);
	}
	for (int i = 0; i <= SIFT_LANES; i++)
		UnlockReleaseBuffer(bufs[i]);
	return e;
}

typedef struct SwapArg
{
	uint64		sealed;			/* 0: only add */
	SiftSegEntry fresh;
} SwapArg;

static void
swap_active(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	SwapArg    *a = arg;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == a->sealed)
			dir->segs[i].kind = SIFT_SEG_MUTABLE_SEALED;
	dir->segs[dir->nsegs++] = a->fresh;
}

static void
check_room(Relation index, const SiftDirSnapshot *snap)
{
	if (snap->nsegs >= SIFT_MAX_SEGMENTS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift index \"%s\" has %d segments", RelationGetRelationName(index), snap->nsegs),
				 errhint("Run sift.merge() on the index.")));
}

/* Creates the active segment unless another backend did meanwhile; false when the directory is full */
static bool
create_active(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		room = true;

	LockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(index, snap);
	if (find_kind(snap, SIFT_SEG_MUTABLE_ACTIVE) == NULL && (room = snap->nsegs < SIFT_MAX_SEGMENTS))
	{
		SwapArg		arg = {0};

		arg.fresh = new_segment(index);
		sift_dir_update(index, &(SiftDirChange) {.fn = swap_active,.arg = &arg,.extra = InvalidBuffer});
	}
	UnlockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	pfree(snap);
	return room;
}

/* Sets the sealed flag on the true tail of every lane */
void
sift_mut_flag_sealed(Relation index, const SiftSegEntry *entry)
{
	SiftMutHeaderData hdr;

	read_header(index, entry, &hdr);
	for (int l = 0; l < SIFT_LANES; l++)
	{
		BlockNumber blk = hdr.lanes[l].tail;

		for (;;)
		{
			Buffer		buf = read_locked(index, blk, BUFFER_LOCK_EXCLUSIVE);
			Page		page;

			sift_store_check_page(index, buf, SIFT_PT_MUTDATA, entry->gen);
			page = BufferGetPage(buf);
			blk = MUTPAGE(page)->next;
			if (!BlockNumberIsValid(blk) && !(MUTPAGE(page)->flags & SIFT_MUTPAGE_SEALED))
			{
				GenericXLogState *state = GenericXLogStart(index);

				MUTPAGE(GenericXLogRegisterBuffer(state, buf, 0))->flags |= SIFT_MUTPAGE_SEALED;
				GenericXLogFinish(state);
			}
			UnlockReleaseBuffer(buf);
			if (!BlockNumberIsValid(blk))
				break;
		}
	}
}

uint64
sift_mut_seal(Relation index, bool force)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftSegEntry *act;
	SiftSegEntry sealed;
	SwapArg		arg = {0};

	LockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(index, snap);
	act = find_kind(snap, SIFT_SEG_MUTABLE_ACTIVE);
	if (act != NULL)
	{
		SiftMutHeaderData hdr;

		read_header(index, act, &hdr);
		if (force ? has_docs(index, act) : header_pages(&hdr) >= limit_pages(index))
		{
			if (force || snap->nsegs < SIFT_MAX_SEGMENTS)
			{
				check_room(index, snap);
				sealed = *act;
				arg.sealed = act->gen;
				arg.fresh = new_segment(index);
				sift_dir_update(index, &(SiftDirChange) {.fn = swap_active,.arg = &arg,.extra = InvalidBuffer});
			}
		}
	}
	UnlockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	if (arg.sealed)
		sift_mut_flag_sealed(index, &sealed);
	pfree(snap);
	return arg.sealed;
}

/* ---------- append ---------- */

/* Cached per relation: the active segment and the lane tails last seen */
typedef struct MutHint
{
	uint64		gen;			/* 0: unknown */
	BlockNumber hdr;
	BlockNumber tail[SIFT_LANES];
} MutHint;

static MutHint *
get_hint(Relation index)
{
	if (index->rd_amcache == NULL)
		index->rd_amcache = MemoryContextAllocZero(index->rd_indexcxt, sizeof(MutHint));
	return index->rd_amcache;
}

static void
refresh_hint(Relation index, MutHint *h, uint32 analyzer_hash)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftSegEntry *act;
	SiftMutHeaderData hdr;

	for (;;)
	{
		sift_dir_read(index, snap);
		if (snap->meta.analyzer_hash != analyzer_hash)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" was built with other analyzer options", RelationGetRelationName(index)),
					 errhint("Use REINDEX to apply the new options.")));
		if ((act = find_kind(snap, SIFT_SEG_MUTABLE_ACTIVE)) != NULL)
			break;
		if (!create_active(index) && !sift_maint_make_room(index))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" has %d segments that cannot be merged", RelationGetRelationName(index),
							SIFT_MAX_SEGMENTS)));
	}
	read_header(index, act, &hdr);
	h->gen = act->gen;
	h->hdr = act->header_blk;
	for (int l = 0; l < SIFT_LANES; l++)
		h->tail[l] = hdr.lanes[l].tail;
	pfree(snap);
}

/* A page appends may still go to: the hinted segment's, not sealed */
static bool
open_tail(Page page, uint64 gen)
{
	return !PageIsNew(page) && SiftPageGetOpaque(page)->type == SIFT_PT_MUTDATA &&
		SiftPageGetOpaque(page)->gen == gen && !(MUTPAGE(page)->flags & SIFT_MUTPAGE_SEALED);
}

/*
 * Exclusively locks the tail of a lane, trying the lanes after
 * MyProcNumber % SIFT_LANES without waiting first. InvalidBuffer when the
 * hint is stale (the segment was sealed).
 */
static Buffer
lock_tail(Relation index, MutHint *h, int *lane)
{
	int			start = (uint32) MyProcNumber % SIFT_LANES;

	for (int i = 0; i <= SIFT_LANES; i++)
	{
		int			l = (start + i) % SIFT_LANES;
		Buffer		buf = ReadBuffer(index, h->tail[l]);

		if (i == SIFT_LANES)
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		else if (!ConditionalLockBuffer(buf))
		{
			ReleaseBuffer(buf);
			continue;
		}
		while (open_tail(BufferGetPage(buf), h->gen) && BlockNumberIsValid(MUTPAGE(BufferGetPage(buf))->next))
		{
			BlockNumber next = MUTPAGE(BufferGetPage(buf))->next;

			UnlockReleaseBuffer(buf);
			buf = read_locked(index, next, BUFFER_LOCK_EXCLUSIVE);
		}
		if (!open_tail(BufferGetPage(buf), h->gen))
		{
			UnlockReleaseBuffer(buf);
			return InvalidBuffer;
		}
		*lane = l;
		return buf;
	}
	pg_unreachable();
}

/* Largest n whose piece of terms[0..n) fits room; -1 when not even an empty piece does */
static int
fit(Size room, const SiftTermFreq *t, int nterms, int *termbytes)
{
	int			n = 0,
				tb = 0;

	*termbytes = 0;
	if (sizeof(SiftMutDocData) > room)
		return -1;
	while (n < nterms && RECSIZE(n + 1, tb + t[n].len) <= room)
		tb += t[n++].len;
	*termbytes = tb;
	return n;
}

static void
put_piece(Page page, ItemPointer tid, uint32 dl, bool cont, const SiftTermFreq *t, int n, int termbytes)
{
	SiftMutPageHeaderData *ph = MUTPAGE(page);
	Size		used = SiftPageGetUsed(page),
				size = RECSIZE(n, termbytes);
	SiftMutDocData *d = (SiftMutDocData *) (SiftPageGetData(page) + used);
	uint32	   *hash = (uint32 *) (d + 1);
	uint8	   *tf = (uint8 *) (hash + n),
			   *len = tf + n;
	char	   *tb = (char *) (len + n);

	memset(d, 0, size);
	d->tid = *tid;
	d->flags = cont ? SIFT_MUT_CONT : 0;
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

/*
 * Writes the doc starting at the locked tail cur; returns whether the segment
 * reached mutable_limit. Leaves the final tail block in *tail.
 */
static bool
write_doc(Relation index, MutHint *h, int lane, Buffer cur, ItemPointer tid, const SiftTermFreq *t,
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
		GenericXLogState *state;
		Buffer		nb,
					hb;
		Page		cp,
					np;
		SiftMutHeaderData *hd;

		if (n == rem)
		{
			state = GenericXLogStart(index);
			put_piece(GenericXLogRegisterBuffer(state, cur, 0), tid, dl, !first, t + i, n, tb);
			GenericXLogFinish(state);
			break;
		}
		/* a doc that fits an empty page moves there whole rather than splitting */
		place = n > 0 && !(first && RECSIZE(nterms, whole_bytes) <= SIFT_PAGE_PAYLOAD - sizeof(SiftMutPageHeaderData));
		nb = sift_alloc_page(index, SIFT_PT_MUTDATA, h->gen);
		hb = read_locked(index, h->hdr, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		cp = GenericXLogRegisterBuffer(state, cur, 0);
		np = GenericXLogRegisterBuffer(state, nb, 0);
		hd = MUTHDR(GenericXLogRegisterBuffer(state, hb, 0));
		if (place)
		{
			put_piece(cp, tid, dl, !first, t + i, n, tb);
			if (i + n < nterms)
				MUTPAGE(cp)->flags |= SIFT_MUTPAGE_SPANS;
		}
		MUTPAGE(cp)->next = BufferGetBlockNumber(nb);
		init_data_page(np);
		hd->lanes[lane].tail = BufferGetBlockNumber(nb);
		hd->lanes[lane].npages++;
		crossed = header_pages(hd) >= limit_pages(index);
		GenericXLogFinish(state);
		UnlockReleaseBuffer(hb);
		UnlockReleaseBuffer(cur);
		cur = nb;
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
sift_mut_append(Relation index, ItemPointer tid, const SiftTermFreq *terms, int nterms, uint32 dl)
{
	MutHint    *h = get_hint(index);
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
		if (BufferIsValid(buf = lock_tail(index, h, &lane)))
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
	if (sift_mut_append(index, heap_tid, terms, n, dl))
		sift_maint_after_seal(index);
	sift_analyzer_reset(an);
	return false;
}

/* ---------- reading ---------- */

struct SiftMutReader
{
	Relation	index;
	SiftSegEntry entry;
	MemoryContext mcxt;
	SiftMutPageFilter filter;
	void	   *filter_arg;
	BlockNumber head[SIFT_LANES];
	int			lane;			/* next lane to start */
	BlockNumber nextblk;		/* next page of the current lane */
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

SiftMutReader *
sift_mut_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt, SiftMutPageFilter filter,
			  void *filter_arg)
{
	SiftMutReader *r = MemoryContextAllocZero(mcxt, sizeof(SiftMutReader));
	SiftMutHeaderData hdr;

	r->index = index;
	r->entry = *entry;
	r->mcxt = mcxt;
	r->filter = filter;
	r->filter_arg = filter_arg;
	r->page = MemoryContextAlloc(mcxt, BLCKSZ);
	r->nextblk = InvalidBlockNumber;
	read_header(index, entry, &hdr);
	for (int l = 0; l < SIFT_LANES; l++)
		r->head[l] = hdr.lanes[l].head;
	return r;
}

void
sift_mut_close(SiftMutReader *r)
{
	pfree(r->page);
	pfree(r);
}

void
sift_mut_totals(const SiftMutReader *r, uint64 *ndocs, uint64 *sum_dl)
{
	*ndocs = r->ndocs;
	*sum_dl = r->sum_dl;
}

/* Copies a page out under a share lock, so decoding holds no buffer */
static void
copy_page(Relation index, const SiftSegEntry *e, BlockNumber blk, Page dst)
{
	Buffer		buf = read_locked(index, blk, BUFFER_LOCK_SHARE);

	sift_store_check_page(index, buf, SIFT_PT_MUTDATA, e->gen);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	if (SiftPageGetUsed(dst) < sizeof(SiftMutPageHeaderData))
		corrupt(index, "Mutable block %u of segment %llu is truncated.", blk, (unsigned long long) e->gen);
}

/* Loads the next page of the segment; a page that may span is never filtered out unless force is false and the filter says so */
static bool
advance_page(SiftMutReader *r, bool force)
{
	for (;;)
	{
		BlockNumber blk = r->nextblk;
		SiftMutPageHeaderData *ph;

		while (!BlockNumberIsValid(blk) && r->lane < SIFT_LANES)
			blk = r->head[r->lane++];
		if (!BlockNumberIsValid(blk))
			return false;
		copy_page(r->index, &r->entry, blk, r->page);
		ph = MUTPAGE(r->page);
		r->nextblk = ph->next;
		r->pos = 0;
		r->used = SiftPageGetUsed(r->page) - sizeof(SiftMutPageHeaderData);
		r->pflags = ph->flags;
		r->ndocs += ph->ndocs;
		r->sum_dl += ph->sum_dl;
		if (force || !r->filter || (ph->flags & SIFT_MUTPAGE_SPANS) || r->filter(r->filter_arg, ph->bloom))
			return true;
		CHECK_FOR_INTERRUPTS();
	}
}

/* The record at r->pos, checked; *size its length */
static const SiftMutDocData *
rec_at(SiftMutReader *r, Size *size)
{
	const SiftMutDocData *d = (const SiftMutDocData *) (RECS(r->page) + r->pos);
	const uint8 *len;
	int			sum = 0;

	if (r->pos + sizeof(SiftMutDocData) > r->used)
		corrupt(r->index, "A record header at byte %zu overruns the %zu used bytes of a mutable page.", r->pos, r->used);
	*size = RECSIZE(d->nterms, d->termbytes);
	if (r->pos + *size > r->used)
		corrupt(r->index, "A record of %zu bytes at byte %zu overruns the %zu used bytes of a mutable page.",
				*size, r->pos, r->used);
	len = (const uint8 *) (d + 1) + d->nterms * 5;
	for (int i = 0; i < d->nterms; i++)
		sum += len[i];
	if (sum != d->termbytes)
		corrupt(r->index, "A mutable record lists %u term bytes but its terms add up to %d.", d->termbytes, sum);
	return d;
}

static void
asm_piece(SiftMutReader *r, const SiftMutDocData *d)
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
sift_mut_next(SiftMutReader *r, SiftMutDoc *doc)
{
	for (;;)
	{
		const SiftMutDocData *d;
		Size		size;

		if (r->pos >= r->used)
		{
			if (!advance_page(r, false))
				return false;
			continue;
		}
		d = rec_at(r, &size);
		if (d->flags & SIFT_MUT_CONT)
			corrupt(r->index, "A continuation record of segment %llu has no first piece.", (unsigned long long) r->entry.gen);
		doc->tid = d->tid;
		doc->dl = d->dl;
		doc->deleted = (d->flags & SIFT_MUT_DELETED) != 0;
		r->pos += size;
		if (r->pos < r->used || !(r->pflags & SIFT_MUTPAGE_SPANS) || !BlockNumberIsValid(r->nextblk))
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
			if (r->pos < r->used || !(r->pflags & SIFT_MUTPAGE_SPANS) || !advance_page(r, true) || r->pos >= r->used)
				break;
			d = rec_at(r, &size);
			if (!(d->flags & SIFT_MUT_CONT))
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
	SiftMutReader *r = sift_mut_open(index, entry, CurrentMemoryContext, NULL, NULL);
	SiftMutDoc	doc;
	bool		any = sift_mut_next(r, &doc);

	sift_mut_close(r);
	return any;
}

/* ---------- pages ---------- */

/* Data pages of the lane chains as they are now (appenders may add more) */
static BlockNumber *
data_blocks(Relation index, const SiftSegEntry *entry, int *n)
{
	SiftMutHeaderData hdr;
	int			cap = 64;
	BlockNumber *blks = palloc(cap * sizeof(BlockNumber));

	*n = 0;
	read_header(index, entry, &hdr);
	for (int l = 0; l < SIFT_LANES; l++)
		for (BlockNumber blk = hdr.lanes[l].head; BlockNumberIsValid(blk);)
		{
			Buffer		buf = read_locked(index, blk, BUFFER_LOCK_SHARE);

			sift_store_check_page(index, buf, SIFT_PT_MUTDATA, entry->gen);
			if (*n == cap)
				blks = repalloc(blks, (cap *= 2) * sizeof(BlockNumber));
			blks[(*n)++] = blk;
			blk = MUTPAGE(BufferGetPage(buf))->next;
			UnlockReleaseBuffer(buf);
		}
	return blks;
}

static int
blk_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(*(const BlockNumber *) a, *(const BlockNumber *) b);
}

SiftExtent *
sift_mut_pages(Relation index, const SiftSegEntry *entry, int *n)
{
	int			nb;
	BlockNumber *blks = data_blocks(index, entry, &nb);
	SiftExtent *ext = palloc((nb + 1) * sizeof(SiftExtent));

	blks = repalloc(blks, (nb + 1) * sizeof(BlockNumber));
	blks[nb++] = entry->header_blk;
	qsort(blks, nb, sizeof(BlockNumber), blk_cmp);
	*n = 0;
	for (int i = 0; i < nb; i++)
		if (*n > 0 && ext[*n - 1].start + ext[*n - 1].nblocks == blks[i])
			ext[*n - 1].nblocks++;
		else
			ext[(*n)++] = (SiftExtent) {blks[i], 1};
	pfree(blks);
	return ext;
}

/* ---------- VACUUM ---------- */

int64
sift_mut_bulkdelete(Relation index, const SiftSegEntry *entry, IndexBulkDeleteCallback cb, void *cbstate,
					IndexBulkDeleteResult *stats)
{
	int			nb;
	BlockNumber *blks = data_blocks(index, entry, &nb);
	SiftMutReader *r = sift_mut_open(index, entry, CurrentMemoryContext, NULL, NULL);
	int64		removed = 0;
	Size	   *offs = palloc(SIFT_PAGE_PAYLOAD / sizeof(SiftMutDocData) * sizeof(Size));
	ItemPointerData *tids = palloc(SIFT_PAGE_PAYLOAD / sizeof(SiftMutDocData) * sizeof(ItemPointerData));

	for (int i = 0; i < nb; i++)
	{
		int			n = 0,
					ndead = 0;

		copy_page(index, entry, blks[i], r->page);
		r->pos = 0;
		r->used = SiftPageGetUsed(r->page) - sizeof(SiftMutPageHeaderData);
		while (r->pos < r->used)
		{
			Size		size;
			const SiftMutDocData *d = rec_at(r, &size);

			if (!(d->flags & (SIFT_MUT_CONT | SIFT_MUT_DELETED)))
			{
				offs[n] = r->pos;
				tids[n++] = d->tid;
			}
			r->pos += size;
		}
		for (int k = 0; k < n; k++)
		{
			if (cb(&tids[k], cbstate))
				offs[ndead++] = offs[k];
			else
				stats->num_index_tuples++;
		}
		if (ndead > 0)
		{
			Buffer		buf = read_locked(index, blks[i], BUFFER_LOCK_EXCLUSIVE);
			GenericXLogState *state = GenericXLogStart(index);
			Page		page = GenericXLogRegisterBuffer(state, buf, 0);

			for (int k = 0; k < ndead; k++)
				((SiftMutDocData *) (RECS(page) + offs[k]))->flags |= SIFT_MUT_DELETED;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buf);
			removed += ndead;
		}
		CHECK_FOR_INTERRUPTS();
	}
	sift_mut_close(r);
	pfree(offs);
	pfree(tids);
	pfree(blks);
	stats->tuples_removed += removed;
	return removed;
}

/* ---------- verification ---------- */

uint64
sift_mut_verify(Relation index, const SiftSegEntry *entry)
{
	SiftMutHeaderData hdr;
	SiftMutReader *r = sift_mut_open(index, entry, CurrentMemoryContext, NULL, NULL);
	uint64		live = 0,
				gen = entry->gen;

	read_header(index, entry, &hdr);
	if (hdr.gen != gen)
		corrupt(index, "Mutable header of segment %llu carries generation %llu.", (unsigned long long) gen, (unsigned long long) hdr.gen);
	for (int l = 0; l < SIFT_LANES; l++)
	{
		BlockNumber blk = hdr.lanes[l].head;
		uint32		npages = 0;

		if (!BlockNumberIsValid(blk))
			corrupt(index, "Lane %d of mutable segment %llu has no pages.", l, (unsigned long long) gen);
		for (; BlockNumberIsValid(blk); npages++)
		{
			uint64		ndocs = 0,
						sum_dl = 0,
						bloom[4] = {0};
			SiftMutPageHeaderData *ph;

			if (npages > RelationGetNumberOfBlocks(index))
				corrupt(index, "Lane %d of mutable segment %llu is a cycle.", l, (unsigned long long) gen);
			copy_page(index, entry, blk, r->page);
			ph = MUTPAGE(r->page);
			r->pos = 0;
			r->used = SiftPageGetUsed(r->page) - sizeof(SiftMutPageHeaderData);
			while (r->pos < r->used)
			{
				Size		size;
				const SiftMutDocData *d = rec_at(r, &size);
				const uint32 *hash = (const uint32 *) (d + 1);

				if (!ItemPointerIsValid(&d->tid) || (d->flags & ~(SIFT_MUT_DELETED | SIFT_MUT_CONT)))
					corrupt(index, "A record of mutable segment %llu has tid (%u,%u) and flags %x.",
							(unsigned long long) gen, ItemPointerGetBlockNumberNoCheck(&d->tid),
							ItemPointerGetOffsetNumberNoCheck(&d->tid), d->flags);
				for (int i = 0; i < d->nterms; i++)
					bloom_add(bloom, hash[i]);
				if (!(d->flags & SIFT_MUT_CONT))
				{
					ndocs++;
					sum_dl += d->dl;
					live += !(d->flags & SIFT_MUT_DELETED);
				}
				r->pos += size;
			}
			if (ndocs != ph->ndocs || sum_dl != ph->sum_dl)
				corrupt(index, "Mutable block %u holds %llu docs and %llu tokens, its header %u and %u.", blk,
						(unsigned long long) ndocs, (unsigned long long) sum_dl, ph->ndocs, ph->sum_dl);
			for (int i = 0; i < 4; i++)
				if (bloom[i] & ~ph->bloom[i])
					corrupt(index, "The bloom filter of mutable block %u misses a term of its records.", blk);
			blk = ph->next;
		}
		/* appenders may have added pages since the header was read */
		if (npages < hdr.lanes[l].npages)
			corrupt(index, "Lane %d of mutable segment %llu has %u pages, its header %u.",
					l, (unsigned long long) gen, npages, hdr.lanes[l].npages);
	}
	sift_mut_close(r);
	return live;
}

static bool
no_page(void *arg, const uint64 *bloom)
{
	return false;
}

void
sift_mut_summary(Relation index, const SiftSegEntry *entry, uint64 *ndocs, uint64 *sum_dl, uint64 *npages)
{
	SiftMutHeaderData hdr;
	SiftMutReader *r = sift_mut_open(index, entry, CurrentMemoryContext, no_page, NULL);
	SiftMutDoc	d;

	while (sift_mut_next(r, &d))
		;
	sift_mut_totals(r, ndocs, sum_dl);
	sift_mut_close(r);
	read_header(index, entry, &hdr);
	*npages = 1 + header_pages(&hdr);
}
