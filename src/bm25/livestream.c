/*
 * bm25/livestream.c - the live part of a segment streamed: the input of a merge (sift_seg_stream) and the collecting dump.
 */
#include "core/host.h"

#include "bm25/kernels/kernels.h"
#include "bm25/segment/segment.h"

#include "bm25/segment/int.h"
#include "core/memtrack.h"

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

/* Where the row of each page of the SB starts (a row ends past the last doc of its page) and the slots of all of them, in e->start */
static void
sbdocs_layout(SiftSegment *seg, const SiftDocChunk *ch, SBDocs *e)
{
	e->start[0] = 0;
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		uint64		present[SIFT_OFFMASK_WORDS];
		int			len = 0;

		if (ch->pagemask[p / 64] & (1ULL << (p % 64)))
		{
			sift_seg_page(seg, ch, p, present, NULL);
			for (int wd = SIFT_OFFMASK_WORDS - 1; wd >= 0 && len == 0; wd--)
				if (present[wd])
					len = wd * 64 + 64 - __builtin_clzll(present[wd]);
		}
		e->start[p + 1] = e->start[p] + len;
	}
}

static inline uint16
sbdocs_slot_dl(const SBDocs *e, uint32 slot)
{
	if (e->bits == 16)
		return ((const uint16 *) e->dl)[slot];
	return e->bits == 8 ? e->dl[slot] : e->dl[slot / 2] >> (slot % 2 * 4) & 15;
}

static inline void
sbdocs_put_dl(SBDocs *e, uint32 slot, uint16 dl)
{
	if (e->bits == 16)
		((uint16 *) e->dl)[slot] = dl;
	else if (e->bits == 8)
		e->dl[slot] = dl;
	else
		e->dl[slot / 2] |= dl << (slot % 2 * 4);
}

/* The bits a slot of the SB needs for the longest of its docs */
static uint8
sbdocs_bits(SiftSegment *seg, const SiftDocChunk *ch, const SBDocs *e)
{
	uint16		maxdl = 0;

	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		const uint16 *dl;

		if (e->start[p + 1] == e->start[p])
			continue;
		sift_seg_page(seg, ch, p, NULL, &dl);
		for (uint32 i = 0; i < e->start[p + 1] - e->start[p]; i++)
			maxdl = Max(maxdl, dl[i]);
	}
	return maxdl < 16 ? 4 : maxdl < 256 ? 8 : 16;
}

/* The vacuumed docs of page p in e->dead */
static bool
sbdocs_mark_dead(SiftSegment *seg, const SiftDocChunk *ch, int p, const uint64 *present, SBDocs *e)
{
	uint64		live[SIFT_OFFMASK_WORDS];
	bool		any = false;

	sift_seg_page_live(seg, ch, p, live);
	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		for (uint64 m = present[wd] & ~live[wd]; m; m &= m - 1, any = true)
		{
			uint32		slot = e->start[p] + wd * 64 + __builtin_ctzll(m);

			e->dead[slot / 8] |= 1 << (slot % 8);
		}
	return any;
}

/* The docs of the SB at position at of the directory: the entry, the dl of its rows at the width they need and their vacuumed bits in one allocation, whose size the cache counts */
static SBDocs *
sbdocs_build(DocCache *dc, int64 at)
{
	SiftSegment *seg = dc->seg;
	SiftDocChunk ch;
	SBDocs		layout;
	uint32		total;
	Size		dllen,
				deadlen;
	SBDocs	   *e;
	bool		anydead = false;

	sift_seg_docs(seg, docmap(seg)->dir[at].sb, &ch);
	dirty_everywhere(seg, &ch);
	sbdocs_layout(seg, &ch, &layout);
	layout.bits = sbdocs_bits(seg, &ch, &layout);
	total = layout.start[SIFT_SB_PAGES];
	dllen = ((Size) total * layout.bits + 7) / 8;
	deadlen = ch.dirty ? (total + 7) / 8 : 0;
	/* check-memory: allow the docs of one superblock (at most 256 pages of 291 rows) */
	e = MemoryContextAllocZero(dc->mcxt, sizeof(SBDocs) + dllen + deadlen);
	e->size = sizeof(SBDocs) + dllen + deadlen;
	memcpy(e->start, layout.start, sizeof(layout.start));
	e->bits = layout.bits;
	e->dl = (uint8 *) (e + 1);
	e->dead = e->dl + dllen;
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		uint64		present[SIFT_OFFMASK_WORDS];
		const uint16 *dl;

		if (e->start[p + 1] == e->start[p])
			continue;
		sift_seg_page(seg, &ch, p, present, &dl);
		for (uint32 i = 0; i < e->start[p + 1] - e->start[p]; i++)
			sbdocs_put_dl(e, e->start[p] + i, dl[i]);
		if (ch.dirty)
			anydead |= sbdocs_mark_dead(seg, &ch, p, present, e);
	}
	if (!anydead)
		e->dead = NULL;
	e->at = at;
	return e;
}

SBDocs *
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
		pfree(old);
	}
	dc->sb[at] = e;
	dc->used += e->size;
	lru_push(dc, e);
	return e;
}

/* The dl of the doc with this key, vacuumed or not; a posting of a doc the SB does not have is corruption */
uint16
sbdocs_raw(DocCache *dc, const SBDocs *e, SiftKey key, uint32 *slot)
{
	int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;

	if (off < 0 || (uint32) off >= e->start[page + 1] - e->start[page] || sbdocs_slot_dl(e, *slot = e->start[page] + off) == 0)
		corrupt(dc->seg, "Posting (%u,%u) is not a doc of the segment.", sift_key_block(key), (uint32) (key & 0xFFFF));
	return sbdocs_slot_dl(e, *slot);
}

/* The dl of the doc with this key, 0 when it was vacuumed */
static uint16
sbdocs_dl(DocCache *dc, const SBDocs *e, SiftKey key)
{
	uint32		slot;
	uint16		dl = sbdocs_raw(dc, e, key, &slot);

	return e->dead && (e->dead[slot / 8] >> (slot % 8) & 1) ? 0 : dl;
}

void
doccache_init(DocCache *dc, SiftSegment *seg, MemoryContext mcxt, Size budget)
{
	*dc = (DocCache) {.seg = seg,.mcxt = mcxt,.budget = budget};
	if (budget == 0)
		return;
	dc->sb = MemoryContextAlloc(mcxt, docmap(seg)->nsb * sizeof(SBDocs *));
	memset(dc->sb, 0, docmap(seg)->nsb * sizeof(SBDocs *));
}

/* A term can have this many postings (a group, the most the evaluator decodes at once) */
#define STREAM_POSTINGS		SIFT_GRP_MAXTIDS
/* The position records the pending postings of a term whose df went first may hold before they go */
#define STREAM_POS_KEEP		(128 * 1024)

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
	bool		streaming;		/* the df of the term went first: its postings go as they come */
	int			nf;				/* fields whose lengths the docs deliver */
	uint16	   *legacy;			/* their lengths for a segment that has none */
	/* the live postings not yet delivered */
	uint32		n;
	uint32		pcap;			/* entries of keys, tf, dl and plen */
	uint32		gcap;			/* entries of tids and gtf */
	SiftKey    *keys;
	uint8	   *tf;
	uint16	   *dl;
	uint32	   *plen;
	uint8	   *pos;			/* their records, back to back (positions only) */
	Size		poslen;
	Size		poscap;
} Stream;

/* MEM[MEM-BM25-12]: bm25 livestream/facet scratch + allowlist sweep (1.8MB/seg eager; 41 DEFAULT_SIZES + 37 huge) */
/* The arrays of a group of k postings (what decodes at once), growing from 256 to what the groups of the segment need */
static void
stream_reserve_group(Stream *s, uint32 k)
{
	if (k <= s->gcap)
		return;
	s->gcap = Min(Max(k, Max(256, 2 * s->gcap)), STREAM_POSTINGS);
	SIFT_REGROW_IN(s->dc.mcxt, s->tids, (Size) s->gcap * sizeof(sift_tid));
	SIFT_REGROW_IN(s->dc.mcxt, s->gtf, s->gcap + SIFT_KERN_PAD);
}

/* The pending postings: k more */
static void
stream_reserve_pending(Stream *s, uint32 k)
{
	if (s->n + k <= s->pcap)
		return;
	s->pcap = Min(Max(s->n + k, Max(256, 2 * s->pcap)), STREAM_POSTINGS);
	SIFT_REGROW_IN(s->dc.mcxt, s->keys, (Size) s->pcap * sizeof(SiftKey));
	SIFT_REGROW_IN(s->dc.mcxt, s->tf, s->pcap);
	SIFT_REGROW_IN(s->dc.mcxt, s->dl, (Size) s->pcap * sizeof(uint16));
	SIFT_REGROW_IN(s->dc.mcxt, s->plen, (Size) s->pcap * sizeof(uint32));
}

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
	uint32		k;

	stream_reserve_group(s, sift_grp_count(g));
	stream_reserve_pending(s, sift_grp_count(g));
	k = sift_grp_tids(g, NULL, s->tids);
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
				SIFT_REGROW_HUGE_IN(s->dc.mcxt, s->pos, s->poscap);
			}
			memcpy(s->pos + s->poslen, rec, len);
			s->poslen += len;
			s->plen[s->n] = len;
		}
		s->keys[s->n] = s->tids[j];
		s->tf[s->n] = s->gtf[j];
		s->dl[s->n++] = dl;
		if (s->streaming && s->poslen >= STREAM_POS_KEEP)
			stream_flush(s, true);
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
		uint32		k;

		stream_reserve_group(s, sift_grp_count(g));
		k = sift_grp_tids(g, NULL, s->tids);
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
	SiftPostingCursor *c = sift_seg_post_open(s->seg, info, s->tcxt);
	bool		haspos = (info->flags & SIFT_TERM_POS) != 0,
				buffered = s->dirty && info->df <= STREAM_POSTINGS;
	SiftPosReader *pr = haspos ? sift_post_pos_reader(c, s->tcxt) : NULL;
	SiftTermInfo live = *info;

	/* the df of a term goes first: known, counted, or (a small term of a vacuumed segment) that of its buffered postings */
	if (s->dirty && !buffered)
		live.df = stream_count(s, c);
	if (!buffered && live.df > 0)
		s->sink->term(s->arg, term, len, &live);
	s->streaming = !buffered;
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
	uint16	   *fdl = s->nf ? palloc(lengthof(keys) * s->nf * sizeof(uint16)) : NULL;
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
					if (s->nf)
							sift_seg_doc_fields(seg, &ch, p, o, s->legacy, s->nf, fdl + n * s->nf);
						dl[n++] = row[o];
					if (n == lengthof(keys))
					{
						s->sink->docs(s->arg, keys, dl, fdl, n);
						n = 0;
					}
				}
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (n > 0)
		s->sink->docs(s->arg, keys, dl, fdl, n);
}

void
sift_seg_stream(SiftSegment *seg, Size budget, const SiftSegSink *sink, void *arg)
{
	MemoryContext cxt = sift_mem_allocset_create(CurrentMemoryContext, "sift stream");
	MemoryContext old = MemoryContextSwitchTo(cxt);
	Stream		s = {.seg = seg,.sink = sink,.arg = arg,.dirty = (seg->entry.flags & SIFT_SEG_DIRTY) != 0};

	doccache_init(&s.dc, seg, cxt, budget);
	s.nf = sift_seg_index_nfields(seg);
	if (s.nf && seg->fmt < SIFT_SEGFMT_FIELDDL)
		s.legacy = sift_seg_legacy_field_dl(seg, s.nf, cxt);
	s.tcxt = sift_mem_allocset_create(cxt, "sift stream term");
	stream_docs(&s);
	if (sink->term)
		sift_dict_scan(seg->dict, NULL, 0, NULL, 0, stream_term, &s);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

struct SiftSegCursor
{
	Stream		s;
	MemoryContext cxt;
	/* docs */
	uint32		next_sb;
	bool		docs_started;
	SiftDocChunk ch;
	int			page;
	int			wd;
	uint64		live[SIFT_OFFMASK_WORDS];
	const uint16 *row;
	uint64		m;
	uint16		fdl[SIFT_MAX_FIELDS];	/* the lengths of the fields of the doc last returned */
	/* terms */
	SiftDictIter *dit;
	SiftTermInfo info;
	SiftPostingCursor *pc;
	SiftPosReader *pr;
	uint32		ri;				/* of the pending postings, the next to deliver */
	Size		poff;			/* ... and where its record starts in s.pos */
};

SiftSegCursor *
sift_seg_cursor(SiftSegment *seg, Size budget, const char *lo, int lolen)
{
	MemoryContext cxt = sift_mem_allocset_create(CurrentMemoryContext, "sift cursor");
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftSegCursor *c = palloc0(sizeof(SiftSegCursor));
	Stream	   *s = &c->s;

	c->cxt = cxt;
	s->seg = seg;
	s->dirty = (seg->entry.flags & SIFT_SEG_DIRTY) != 0;
	s->nf = sift_seg_index_nfields(seg);
	doccache_init(&s->dc, seg, cxt, budget);
	c->page = SIFT_SB_PAGES;
	c->wd = SIFT_OFFMASK_WORDS;
	c->next_sb = seg->hdr->sb_lo;
	if (budget > 0)
	{
		s->tcxt = sift_mem_allocset_create(cxt, "sift cursor term");
		c->dit = sift_dict_iter(seg->dict, lo, lolen, cxt);
	}
	MemoryContextSwitchTo(old);
	return c;
}

void
sift_seg_cursor_close(SiftSegCursor *c)
{
	MemoryContextDelete(c->cxt);
}

int
sift_seg_cursor_nfields(const SiftSegCursor *c)
{
	return c->s.nf;
}

bool
sift_seg_cursor_doc(SiftSegCursor *c, SiftKey *key, uint16 *dl, const uint16 **fdl)
{
	SiftSegment *seg = c->s.seg;

	if (c->s.nf && c->s.legacy == NULL && seg->fmt < SIFT_SEGFMT_FIELDDL)
		c->s.legacy = sift_seg_legacy_field_dl(seg, c->s.nf, c->cxt);

	while (c->m == 0)
	{
		if (++c->wd < SIFT_OFFMASK_WORDS)
		{
			c->m = c->live[c->wd];
			continue;
		}
		for (c->page++; c->page < SIFT_SB_PAGES; c->page++)
			if (c->ch.pagemask[c->page / 64] & (1ULL << (c->page % 64)))
				break;
		if (c->page < SIFT_SB_PAGES)
		{
			sift_seg_page(seg, &c->ch, c->page, c->live, &c->row);
			if (c->ch.dirty)
				sift_seg_page_live(seg, &c->ch, c->page, c->live);
			c->wd = 0;
			c->m = c->live[0];
			continue;
		}
		if (c->docs_started)
			c->next_sb = c->ch.sb + 1;
		if (c->next_sb > seg->hdr->sb_hi || !sift_seg_docs_next(seg, c->next_sb, &c->ch))
			return false;
		c->docs_started = true;
		dirty_everywhere(seg, &c->ch);
		c->page = -1;
		c->wd = SIFT_OFFMASK_WORDS;
		CHECK_FOR_INTERRUPTS();
	}
	{
		int			o = c->wd * 64 + __builtin_ctzll(c->m);

		c->m &= c->m - 1;
		*key = ((SiftKey) (c->ch.sb * SIFT_SB_PAGES + c->page) << 16) | (o + 1);
		*dl = c->row[o];
		if (c->s.nf)
			sift_seg_doc_fields(seg, &c->ch, c->page, o, c->s.legacy, c->s.nf, c->fdl);
		*fdl = c->s.nf ? c->fdl : NULL;
	}
	return true;
}

bool
sift_seg_cursor_term(SiftSegCursor *c, const char **term, int *len, SiftTermInfo *info)
{
	Stream	   *s = &c->s;
	SiftTermInfo found;

	MemoryContextReset(s->tcxt);
	c->pc = NULL;
	while (sift_dict_iter_next(c->dit, term, len, &found))
	{
		c->info = found;
		c->pc = sift_seg_post_open(s->seg, &c->info, s->tcxt);
		c->pr = (found.flags & SIFT_TERM_POS) ? sift_post_pos_reader(c->pc, s->tcxt) : NULL;
		if (s->dirty)
			c->info.df = stream_count(s, c->pc);
		if (c->info.df == 0)
		{
			MemoryContextReset(s->tcxt);
			c->pc = NULL;
			continue;
		}
		s->n = c->ri = 0;
		s->poslen = c->poff = 0;
		*info = c->info;
		return true;
	}
	return false;
}

bool
sift_seg_cursor_posting(SiftSegCursor *c, SiftKey *key, uint8 *tf, uint16 *dl, const uint8 **rec, uint32 *len)
{
	Stream	   *s = &c->s;

	while (c->ri == s->n)
	{
		const sift_grp *g;

		s->n = c->ri = 0;
		s->poslen = c->poff = 0;
		if ((g = sift_next(sift_post_cursor(c->pc))) == NULL)
			return false;
		stream_group(s, c->pc, g, c->pr);
	}
	*key = s->keys[c->ri];
	*tf = s->tf[c->ri];
	*dl = s->dl[c->ri];
	if (c->pr)
	{
		*len = s->plen[c->ri];
		*rec = s->pos + c->poff;
		c->poff += *len;
	}
	else
		*rec = NULL, *len = 0;
	c->ri++;
	return true;
}

void
sift_seg_live_stats(SiftSegment *seg, uint64 *ndocs, uint64 *sum_dl)
{
	SiftSegCursor *c;
	SiftKey		key;
	uint16		dl;
	const uint16 *fdl;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY) || seg->fmt >= SIFT_SEGFMT_LIVEROWS)
	{
		sift_seg_live_totals(seg, ndocs, sum_dl);
		return;
	}
	c = sift_seg_cursor(seg, 0, NULL, 0);
	*ndocs = *sum_dl = 0;
	while (sift_seg_cursor_doc(c, &key, &dl, &fdl))
	{
		(*ndocs)++;
		*sum_dl += dl;
	}
	sift_seg_cursor_close(c);
}
