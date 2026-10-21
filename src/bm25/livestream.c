/*
 * bm25/livestream.c - the live part of a segment streamed: the input of a merge (sift_seg_stream) and the collecting dump.
 */
#include "core/host.h"

#include "bm25/kernels.h"
#include "core/live.h"
#include "core/popcount.h"
#include "bm25/segment.h"

#include "bm25/segment_int.h"

/* ---------- the live part of a segment, streamed (the input of a merge) ---------- */

/*
 * The dl of every doc of an SB, in the rows of its DOCMAP record, and which of them were vacuumed: what the postings of the SB
 * look up. Terms come in dictionary order and each one runs through the SBs, so the SBs a term touches are kept (the most
 * recently used within a budget) instead of the dl of the whole segment.
 */
typedef struct SBDocs
{
	struct SBDocs *newer;
	struct SBDocs *older;
	int64		at;				/* in the SB directory */
	Size		size;
	uint16	   *dl;				/* the rows of its pages one after the other */
	uint8	   *dead;			/* a bit for each doc slot of dl: vacuumed (NULL in a clean SB) */
	uint32		start[SIFT_SB_PAGES];	/* of the row of each page in dl */
	uint16		len[SIFT_SB_PAGES];		/* 0: no docs on the page */
} SBDocs;

typedef struct DocCache
{
	SiftSegment *seg;
	MemoryContext mcxt;
	SBDocs	  **sb;				/* by position in the SB directory */
	SBDocs	   *newest;
	SBDocs	   *oldest;
	Size		used;
	Size		budget;
	uint16	   *rows;			/* scratch of a build: SIFT_SB_PAGES * MAXOFF slots and their dead bits */
	uint8	   *dead;
} DocCache;

static void
lru_unlink(DocCache *dc, SBDocs *e)
{
	*(e->newer ? &e->newer->older : &dc->newest) = e->older;
	*(e->older ? &e->older->newer : &dc->oldest) = e->newer;
}

static void
lru_push(DocCache *dc, SBDocs *e)
{
	e->newer = NULL;
	e->older = dc->newest;
	*(dc->newest ? &dc->newest->newer : &dc->oldest) = e;
	dc->newest = e;
}

/*
 * The dirty bit of an SB is cached when the segment is first asked for docs, and goes stale when docs are deleted through the
 * same SiftSegment later (the MariaDB merge drops its dead documents first): a segment with a vacuumed doc is dirty in every SB
 */
static void
dirty_everywhere(const SiftSegment *seg, SiftDocChunk *ch)
{
	ch->dirty = (seg->entry.flags & SIFT_SEG_DIRTY) != 0;
}

/* The docs of the SB at position at of the directory */
static SBDocs *
sbdocs_build(DocCache *dc, int64 at)
{
	SiftSegment *seg = dc->seg;
	SiftDocChunk ch;
	SBDocs	   *e = MemoryContextAllocZero(dc->mcxt, sizeof(SBDocs));
	uint32		total = 0;
	bool		anydead = false;

	sift_seg_docs(seg, docmap(seg)->dir[at].sb, &ch);
	dirty_everywhere(seg, &ch);
	memset(dc->dead, 0, (SIFT_SB_PAGES * MAXOFF + 7) / 8);
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		uint64		present[SIFT_OFFMASK_WORDS],
					live[SIFT_OFFMASK_WORDS];
		const uint16 *dl;
		int			len = 0;

		if (!(ch.pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		sift_seg_page(seg, &ch, p, present, &dl);
		for (int wd = SIFT_OFFMASK_WORDS - 1; wd >= 0 && len == 0; wd--)
			if (present[wd])
				len = wd * 64 + 64 - __builtin_clzll(present[wd]);
		memcpy(dc->rows + total, dl, len * sizeof(uint16));
		if (ch.dirty)
		{
			sift_seg_page_live(seg, &ch, p, live);
			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
				for (uint64 m = present[wd] & ~live[wd]; m; m &= m - 1, anydead = true)
				{
					uint32		slot = total + wd * 64 + __builtin_ctzll(m);

					dc->dead[slot / 8] |= 1 << (slot % 8);
				}
		}
		e->start[p] = total;
		e->len[p] = len;
		total += len;
	}
	e->at = at;
	e->dl = MemoryContextAlloc(dc->mcxt, total * sizeof(uint16));
	memcpy(e->dl, dc->rows, total * sizeof(uint16));
	e->size = sizeof(SBDocs) + total * sizeof(uint16);
	if (anydead)
	{
		e->dead = MemoryContextAlloc(dc->mcxt, (total + 7) / 8);
		memcpy(e->dead, dc->dead, (total + 7) / 8);
		e->size += (total + 7) / 8;
	}
	return e;
}

static SBDocs *
sbdocs_get(DocCache *dc, uint32 sb)
{
	DocMap	   *dm = docmap(dc->seg);
	int64		at = sb_search(dm, sb);
	SBDocs	   *e;

	if (at == dm->nsb || dm->dir[at].sb != sb)
		corrupt(dc->seg, "A posting lies in SB %u, which has no docs.", sb);
	if ((e = dc->sb[at]) != NULL)
	{
		if (e != dc->newest)
		{
			lru_unlink(dc, e);
			lru_push(dc, e);
		}
		return e;
	}
	e = sbdocs_build(dc, at);
	while (dc->oldest && dc->used + e->size > dc->budget)
	{
		SBDocs	   *old = dc->oldest;

		lru_unlink(dc, old);
		dc->sb[old->at] = NULL;
		dc->used -= old->size;
		pfree(old->dl);
		if (old->dead)
			pfree(old->dead);
		pfree(old);
	}
	dc->sb[at] = e;
	dc->used += e->size;
	lru_push(dc, e);
	return e;
}

/* The dl of the doc with this key, 0 when it was vacuumed; a posting of a doc the SB does not have is corruption */
static uint16
sbdocs_dl(DocCache *dc, const SBDocs *e, SiftKey key)
{
	int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;
	uint32		slot;

	if (off < 0 || off >= e->len[page] || e->dl[slot = e->start[page] + off] == 0)
		corrupt(dc->seg, "Posting (%u,%u) is not a doc of the segment.", sift_key_block(key), (uint32) (key & 0xFFFF));
	return e->dead && (e->dead[slot / 8] >> (slot % 8) & 1) ? 0 : e->dl[slot];
}

/* A term can have this many postings (a group, the most the evaluator decodes at once) */
#define STREAM_POSTINGS		SIFT_GRP_MAXTIDS

typedef struct Stream
{
	SiftSegment *seg;
	const SiftSegSink *sink;
	void	   *arg;
	DocCache	dc;
	MemoryContext tcxt;
	sift_tid   *tids;
	uint8	   *gtf;
	bool		dirty;			/* some doc was vacuumed */
	/* the live postings not yet delivered */
	uint32		n;
	SiftKey    *keys;
	uint8	   *tf;
	uint16	   *dl;
	uint32	   *plen;
	uint8	   *pos;			/* their records, back to back (positions only) */
	Size		poslen;
	Size		poscap;
} Stream;

static void
stream_flush(Stream *s, bool haspos)
{
	if (s->n > 0)
		s->sink->postings(s->arg, s->keys, s->tf, s->dl, haspos ? s->pos : NULL, haspos ? s->plen : NULL, s->n);
	s->n = 0;
	s->poslen = 0;
}

/* Adds the live postings of the group to the pending ones; pr: the records of the term (read in step with the postings) */
static void
stream_group(Stream *s, SiftPostingCursor *c, const sift_grp *g, SiftPosReader *pr)
{
	SBDocs	   *e = sbdocs_get(&s->dc, g->gid);
	uint32		k = sift_grp_tids(g, NULL, s->tids);

	sift_post_group_tf(c, s->gtf);
	for (uint32 j = 0; j < k; j++)
	{
		uint16		dl = sbdocs_dl(&s->dc, e, s->tids[j]);
		uint32		len = 0;
		const uint8 *rec = pr ? sift_post_pos_next(pr, &len) : NULL;

		if (dl == 0)
			continue;
		if (rec)
		{
			if (s->poslen + len > s->poscap)
			{
				s->poscap = Max(s->poslen + len, Max(8192, s->poscap * 2));
				s->pos = s->pos ? repalloc_huge(s->pos, s->poscap) : MemoryContextAllocHuge(s->dc.mcxt, s->poscap);
			}
			memcpy(s->pos + s->poslen, rec, len);
			s->poslen += len;
			s->plen[s->n] = len;
		}
		s->keys[s->n] = s->tids[j];
		s->tf[s->n] = s->gtf[j];
		s->dl[s->n++] = dl;
	}
}

/* Live postings of the term, counted without delivering them */
static uint64
stream_count(Stream *s, SiftPostingCursor *c)
{
	uint64		live = 0;

	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g; g = sift_next(sift_post_cursor(c)))
	{
		SBDocs	   *e = sbdocs_get(&s->dc, g->gid);
		uint32		k = sift_grp_tids(g, NULL, s->tids);

		for (uint32 j = 0; j < k; j++)
			live += sbdocs_dl(&s->dc, e, s->tids[j]) != 0;
	}
	sift_post_rewind(c);
	return live;
}

static bool
stream_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Stream	   *s = arg;
	SiftPostingCursor *c = sift_post_open(&s->seg->sr, info, s->tcxt);
	bool		haspos = (info->flags & SIFT_TERM_POS) != 0,
				buffered = s->dirty && info->df <= STREAM_POSTINGS;
	SiftPosReader *pr = haspos ? sift_post_pos_reader(c, s->tcxt) : NULL;
	SiftTermInfo live = *info;

	/* the df of a term goes first: known, counted, or (a small term of a vacuumed segment) that of its buffered postings */
	if (s->dirty && !buffered)
		live.df = stream_count(s, c);
	if (!buffered && live.df > 0)
		s->sink->term(s->arg, term, len, &live);
	if (buffered || live.df > 0)
		for (const sift_grp *g = sift_next(sift_post_cursor(c)); g; g = sift_next(sift_post_cursor(c)))
		{
			stream_group(s, c, g, pr);
			if (!buffered)
				stream_flush(s, haspos);
		}
	if (buffered && s->n > 0)
	{
		live.df = s->n;
		s->sink->term(s->arg, term, len, &live);
		stream_flush(s, haspos);
	}
	MemoryContextReset(s->tcxt);
	CHECK_FOR_INTERRUPTS();
	return true;
}

/* The live docs of the segment in key order, a batch at a time */
static void
stream_docs(Stream *s)
{
	SiftSegment *seg = s->seg;
	SiftDocChunk ch;
	SiftKey		keys[1024];
	uint16		dl[1024];
	int			n = 0;

	for (uint32 sb = seg->hdr->sb_lo; sb <= seg->hdr->sb_hi && sift_seg_docs_next(seg, sb, &ch); sb = ch.sb + 1)
	{
		dirty_everywhere(seg, &ch);
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			uint64		live[SIFT_OFFMASK_WORDS];
			const uint16 *row;

			if (!(ch.pagemask[p / 64] & (1ULL << (p % 64))))
				continue;
			sift_seg_page(seg, &ch, p, live, &row);
			if (ch.dirty)
				sift_seg_page_live(seg, &ch, p, live);
			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
				for (uint64 m = live[wd]; m; m &= m - 1)
				{
					int			o = wd * 64 + __builtin_ctzll(m);

					keys[n] = ((SiftKey) (ch.sb * SIFT_SB_PAGES + p) << 16) | (o + 1);
					dl[n++] = row[o];
					if (n == lengthof(keys))
					{
						s->sink->docs(s->arg, keys, dl, n);
						n = 0;
					}
				}
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (n > 0)
		s->sink->docs(s->arg, keys, dl, n);
}

void
sift_seg_stream(SiftSegment *seg, Size budget, const SiftSegSink *sink, void *arg)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift stream", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	Stream		s = {.seg = seg,.sink = sink,.arg = arg,.dirty = (seg->entry.flags & SIFT_SEG_DIRTY) != 0};

	s.dc = (DocCache) {.seg = seg,.mcxt = cxt,.budget = budget};
	s.dc.sb = MemoryContextAllocHuge(cxt, docmap(seg)->nsb * sizeof(SBDocs *));
	memset(s.dc.sb, 0, docmap(seg)->nsb * sizeof(SBDocs *));
	s.dc.rows = palloc(SIFT_SB_PAGES * MAXOFF * sizeof(uint16));
	s.dc.dead = palloc((SIFT_SB_PAGES * MAXOFF + 7) / 8);
	s.tcxt = AllocSetContextCreate(cxt, "sift stream term", ALLOCSET_DEFAULT_SIZES);
	s.tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	s.gtf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	s.keys = palloc(STREAM_POSTINGS * sizeof(SiftKey));
	s.tf = palloc(STREAM_POSTINGS);
	s.dl = palloc(STREAM_POSTINGS * sizeof(uint16));
	s.plen = palloc(STREAM_POSTINGS * sizeof(uint32));
	stream_docs(&s);
	sift_dict_scan(seg->dict, NULL, 0, NULL, 0, stream_term, &s);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/* sift_seg_dump: the postings of a term gathered, for the visitor of whole terms */
typedef struct DumpTerm
{
	MemoryContext mcxt;
	SiftDocVisit docs;
	SiftTermVisit visit;
	void	   *arg;
	char		term[SIFT_MAX_TERM_BYTES];
	int			len;
	SiftTermInfo info;
	uint32		n;
	uint32		cap;
	SiftKey    *keys;
	uint8	   *tf;
	uint16	   *dl;
	uint32	   *plen;
	uint8	   *pos;
	Size		poslen;
	Size		poscap;
} DumpTerm;

static void
collect_docs(void *arg, const SiftKey *keys, const uint16 *dl, int n)
{
	DumpTerm   *d = arg;

	d->docs(d->arg, keys, dl, n);
}

static void
collect_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	DumpTerm   *d = arg;

	memcpy(d->term, term, len);
	d->len = len;
	d->info = *info;
	d->n = d->poslen = 0;
	if (info->df > d->cap)
	{
		d->cap = Max(info->df, d->cap * 2);
		d->keys = d->keys ? repalloc_huge(d->keys, d->cap * sizeof(SiftKey)) : MemoryContextAllocHuge(d->mcxt, d->cap * sizeof(SiftKey));
		d->tf = d->tf ? repalloc_huge(d->tf, d->cap) : MemoryContextAllocHuge(d->mcxt, d->cap);
		d->dl = d->dl ? repalloc_huge(d->dl, d->cap * sizeof(uint16)) : MemoryContextAllocHuge(d->mcxt, d->cap * sizeof(uint16));
		d->plen = d->plen ? repalloc_huge(d->plen, d->cap * sizeof(uint32)) : MemoryContextAllocHuge(d->mcxt, d->cap * sizeof(uint32));
	}
}

static void
collect_postings(void *arg, const SiftKey *keys, const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen,
			  int n)
{
	DumpTerm   *d = arg;

	memcpy(d->keys + d->n, keys, n * sizeof(SiftKey));
	memcpy(d->tf + d->n, tf, n);
	memcpy(d->dl + d->n, dl, n * sizeof(uint16));
	if (pos)
	{
		Size		len = 0;

		for (int i = 0; i < n; i++)
			len += poslen[i];
		if (d->poslen + len > d->poscap)
		{
			d->poscap = Max(d->poslen + len, d->poscap * 2);
			d->pos = d->pos ? repalloc_huge(d->pos, d->poscap) : MemoryContextAllocHuge(d->mcxt, d->poscap);
		}
		memcpy(d->pos + d->poslen, pos, len);
		memcpy(d->plen + d->n, poslen, n * sizeof(uint32));
		d->poslen += len;
	}
	d->n += n;
	if (d->n == d->info.df)
		d->visit(d->arg, d->term, d->len, &d->info, d->keys, d->tf, d->dl, pos ? d->pos : NULL, d->plen);
}

void
sift_seg_dump(SiftSegment *seg, const SiftScoreRef *ref, SiftDocVisit docs, SiftTermVisit terms, void *arg)
{
	DumpTerm	d = {.mcxt = CurrentMemoryContext,.docs = docs,.visit = terms,.arg = arg};

	sift_seg_stream(seg, (Size) maintenance_work_mem * 1024 / 2, &(SiftSegSink) {collect_docs, collect_term, collect_postings}, &d);
	if (d.keys)
	{
		pfree(d.keys);
		pfree(d.tf);
		pfree(d.dl);
		pfree(d.plen);
	}
	if (d.pos)
		pfree(d.pos);
}
