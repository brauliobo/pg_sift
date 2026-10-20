/*
 * bm25/walk.c - segment inspection and verification: sift_seg_walk (sift.segments, sift.verify, tests).
 */
#include "core/host.h"

#include "bm25/kernels.h"
#include "core/live.h"
#include "core/popcount.h"
#include "bm25/segment.h"

#include "bm25/segment_int.h"

const uint32 sift_df_class_lo[SIFT_DF_CLASSES] = {1, 2, 3, 16, 128, 1024, 8192, 65536, 524288};
const char *const sift_df_class_name[SIFT_DF_CLASSES] = {
	"1", "2", "3-15", "16-127", "128-1K", "1K-8K", "8K-64K", "64K-512K", "512K+"
};

/* ---------- inspection ---------- */

typedef struct Walk
{
	SiftSegment *seg;
	SiftScoreRef ref;
	bool		verify;
	SiftSegStats *st;
	SiftTermVisit visit;
	void	   *arg;
	SiftKey    *keys;			/* every doc, ascending */
	uint16	   *dl;
	uint64		ndocs;
	/* current term */
	MemoryContext tcxt;
	SiftKey    *tkeys;
	uint8	   *ttf;
	uint16	   *tdl;
	uint32	   *tpoff;			/* positions: offset and length of each posting's record in the stream */
	uint32	   *tplen;
	const uint8 *tprec;			/* the term's records, back to back */
	uint32		tcap;
	sift_tid   *tids;			/* SIFT_GRP_MAXTIDS */
	uint8	   *gtf;
	char		prev[SIFT_MAX_TERM_BYTES];
	int			prevlen;
	bool		stopped;
	uint32		nslots;			/* terms with a slot so far (format 2) */
} Walk;

/* Every doc of the segment with its dl; checks the docmap on the way */
static void
walk_docs(Walk *w)
{
	SiftSegment *seg = w->seg;
	DocMap	   *dm = docmap(seg);
	uint64		sum = 0;

	w->keys = MemoryContextAllocHuge(CurrentMemoryContext, seg->hdr->ndocs * sizeof(SiftKey));
	w->dl = MemoryContextAllocHuge(CurrentMemoryContext, seg->hdr->ndocs * sizeof(uint16));
	for (int64 i = 0; i < dm->nsb; i++)
	{
		load_sb(seg, dm, i);
		index_sb(seg, dm);
		w->st->parts.docmap_presence += dm->reclen;
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			const uint64 *om = dm->om + dm->maskidx[p] * SIFT_OFFMASK_WORDS;
			BlockNumber blk = dm->dir[i].sb * SIFT_SB_PAGES + p;
			const uint16 *row = dm->rows[p] ? dl_row(dm, p) : NULL;

			if (row)
			{
				w->st->parts.docmap_dl += SIFT_DL_ROW_BYTES(dm->dlcode, dm->rowlen[p]);
				w->st->parts.docmap_presence -= SIFT_DL_ROW_BYTES(dm->dlcode, dm->rowlen[p]);
			}
			for (int o = 0; row && o < dm->rowlen[p]; o++)
			{
				uint16		v = row[o];

				if (!((om[o / 64] >> (o % 64)) & 1))
				{
					if (w->verify && v != 0)
						corrupt(seg, "Absent slot (%u,%d) has dl %u.", blk, o + 1, v);
					continue;
				}
				w->keys[w->ndocs] = ((SiftKey) blk << 16) | (o + 1);
				w->dl[w->ndocs++] = v;
				sum += v;
			}
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (w->ndocs != seg->hdr->ndocs || sum != seg->hdr->sum_dl)
		corrupt(seg, "DOCMAP holds %llu docs and %llu tokens, the header %llu and %llu.",
				ULL(w->ndocs), ULL(sum), ULL(seg->hdr->ndocs), ULL(seg->hdr->sum_dl));
}

static void
verify_live(Walk *w)
{
	SiftSegment *seg = w->seg;
	SiftLive	l = seg_live(seg);
	uint32		nsb = l.nsb;
	uint64		start = SIFT_LIVE_START(nsb),
				live = 0;
	Buffer		buf = InvalidBuffer;
	SiftLiveHeaderData lh;
	bool		dirty = false;

	for (int i = 0; i < (int) sizeof(lh); i++)
		((uint8 *) &lh)[i] = sift_live_byte(&l, i, &buf);
	if (lh.nsb != nsb || (start + (seg->hdr->ndocs + 7) / 8 + SIFT_PAGE_PAYLOAD - 1) / SIFT_PAGE_PAYLOAD != seg->hdr->live_npages)
		corrupt(seg, "Liveness header lists %u SBs in %u pages, expected %u.", lh.nsb, seg->hdr->live_npages, nsb);
	for (uint32 i = 0; i < (nsb + 7) / 8; i++)
		dirty |= sift_live_byte(&l, sizeof(lh) + i, &buf) != 0;
	for (uint64 r = 0; r < seg->hdr->ndocs; r += 8)
	{
		uint8		v = sift_live_byte(&l, start + r / 8, &buf);

		if (seg->hdr->ndocs - r < 8 && (v >> (seg->hdr->ndocs - r)) != 0)
			corrupt(seg, "Liveness bits are set past the last doc.");
		live += sift_pc64(v);
	}
	UnlockReleaseBuffer(buf);
	if (live + lh.ndead != seg->hdr->ndocs || ((seg->entry.flags & SIFT_SEG_DIRTY) == 0 && (dirty || lh.ndead)))
		corrupt(seg, "%llu live and %u dead of %llu docs (dirty flag %d).",
				ULL(live), lh.ndead, ULL(seg->hdr->ndocs), (seg->entry.flags & SIFT_SEG_DIRTY) != 0);
}

/* Format 2: the live rows are the docmap's offsets less the vacuumed docs, the header counts them, and the table counts their postings */
static void
verify_live2(Walk *w)
{
	SiftSegment *seg = w->seg;
	SiftLive	l = seg_live(seg);
	SiftLive2HeaderData h;
	DocMap	   *dm = docmap(seg);
	uint8	   *dirty;
	uint64		dead = 0,
				dead_dl = 0,
				rowbytes = 0;
	bool		anydirty = false;

	sift_live2_header(&l, &h);
	dirty = palloc((h.nsb + 7) / 8);
	sift_live2_dirty(&l, &h, dirty);
	for (uint32 i = 0; i < (h.nsb + 7) / 8; i++)
		anydirty |= dirty[i] != 0;
	live2(seg)->rok = false;
	for (int64 i = 0; i < dm->nsb; i++)
	{
		uint32		sb = dm->dir[i].sb,
					idx = sb - seg->hdr->sb_lo;
		const sift_grp *rows;
		uint64		sbdead = 0;
		int			k = 0;

		load_sb(seg, dm, i);
		index_sb(seg, dm);
		live2(seg)->rok = false;
		rows = live2_rows(seg, sb);
		if (rows == NULL || memcmp(rows->pages, dm->pagemask, sizeof(dm->pagemask)) != 0)
			corrupt(seg, "The live rows of SB %u are not those of its pages.", sb);
		rowbytes += sizeof(SiftLive2RegionData) + (uint64) dm->np * rows->w;
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			const uint64 *om = dm->om + dm->maskidx[p] * SIFT_OFFMASK_WORDS,
					   *live = (const uint64 *) (rows->off + (Size) k * rows->w);
			const uint16 *dl;

			if (!dm->rows[p])
				continue;
			dl = dl_row(dm, p);
			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			{
				uint64		lv = wd < (int) (rows->w / 8) ? live[wd] : 0;

				if ((lv & ~om[wd]) || (wd >= (int) (rows->w / 8) && om[wd]))
					corrupt(seg, "The live row of page %d of SB %u holds offsets the docmap does not (word %d).", p, sb, wd);
				for (uint64 m = om[wd] & ~lv; m; m &= m - 1, sbdead++)
					dead_dl += dl[wd * 64 + __builtin_ctzll(m)];
			}
			k++;
		}
		if (sbdead > 0 && !(dirty[idx / 8] >> (idx % 8) & 1))
			corrupt(seg, "SB %u has %llu dead docs and no dirty bit.", sb, ULL(sbdead));
		dead += sbdead;
	}
	if (dead != h.ndead || dead_dl != h.dead_dl || rowbytes != h.tbl_off - h.rows_off ||
		((seg->entry.flags & SIFT_SEG_DIRTY) == 0 && (anydirty || h.ndead)))
		corrupt(seg, "%llu dead docs (%llu tokens) in %llu bytes of rows; the header %u (%llu), %llu bytes (dirty flag %d).",
				ULL(dead), ULL(dead_dl), ULL(rowbytes), h.ndead, ULL(h.dead_dl), ULL(h.tbl_off - h.rows_off),
				(seg->entry.flags & SIFT_SEG_DIRTY) != 0);
	pfree(dirty);
}

/* The table of dead postings against the postings themselves; after the walk of the terms has checked them */
static void
verify_dead2(Walk *w)
{
	SiftSegment *seg = w->seg;
	SiftLive	l = seg_live(seg);
	SiftLive2HeaderData h;

	sift_live2_header(&l, &h);
	if (h.tbl_ndead == h.ndead)
	{
		uint32	   *have = palloc(Max(h.nslots, 1) * sizeof(uint32)),
				   *want = palloc(Max(h.nslots, 1) * sizeof(uint32));

		if (w->nslots != h.nslots)
			corrupt(seg, "The dictionary has %u terms with a slot, the table %u.", w->nslots, h.nslots);
		sift_live2_dead_all(&l, &h, have);
		count_dead(seg, &h, want);
		for (uint32 i = 0; i < h.nslots; i++)
			if (have[i] != want[i])
				corrupt(seg, "The table counts %u dead postings for slot %u, the postings %u.", have[i], i, want[i]);
		pfree(have);
		pfree(want);
	}
}

static int
df_class(uint32 df)
{
	int			c = SIFT_DF_CLASSES - 1;

	while (df < sift_df_class_lo[c])
		c--;
	return c;
}

/* dl of key, searching forward from *pos (keys of a term ascend) */
static uint16
doc_dl(Walk *w, SiftKey key, uint64 *pos)
{
	uint64		lo = *pos,
				hi = w->ndocs,
				step = 1;

	while (lo + step < hi && w->keys[lo + step] <= key)
		step *= 2;
	hi = Min(hi, lo + step + 1);
	while (lo < hi)
	{
		uint64		mid = (lo + hi) / 2;

		if (w->keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == w->ndocs || w->keys[lo] != key)
		corrupt(w->seg, "Posting (%u,%u) is not a doc of the segment.", sift_key_block(key), (uint32) (key & 0xFFFF));
	*pos = lo;
	return w->dl[lo];
}

/*
 * The position records of the term in rank order: offsets and lengths of each, and with verify the
 * counts against tf, the occurrences against dl and the checkpoints
 */
static void
term_positions(Walk *w, SiftPostingCursor *c, const char *term, int len, const SiftTermInfo *info)
{
	SiftSegment *seg = w->seg;
	const uint32 *ck;
	const uint8 *rec,
			   *p,
			   *end;
	uint32		reclen;

	sift_post_pos_stream(c, &ck, &rec, &reclen);
	p = rec;
	end = rec + reclen;
	for (uint32 j = 0; j < info->df; j++)
	{
		uint32		cnt,
					last;
		const uint8 *q = sift_pos_skip(p, end, &cnt, &last);

		if (w->verify)
		{
			uint16		dl = w->tdl[j];

			if ((ck && j % SIFT_POS_EVERY == 0 && ck[j / SIFT_POS_EVERY] != (uint32) (p - rec)) ||
				(w->ttf[j] < SIFT_MAX_TF ? cnt != w->ttf[j] : cnt < SIFT_MAX_TF) || (dl < SIFT_MAX_DL && last >= dl))
				corrupt(seg, "Posting (%u,%u) of \"%.*s\" has %u positions ending at %u for tf %u, dl %u.",
						sift_key_block(w->tkeys[j]), (uint32) (w->tkeys[j] & 0xFFFF), len, term, cnt, last, w->ttf[j], dl);
		}
		w->tpoff[j] = p - rec;
		w->tplen[j] = q - p;
		p = q;
	}
	if (p != end)
		corrupt(seg, "Term \"%.*s\" has %u bytes of positions after its %u postings.", len, term, (uint32) (end - p), info->df);
	w->tprec = rec;
}

static int
varint_len(uint64 x)
{
	int			n = 1;

	for (; x >= 0x80; x >>= 7)
		n++;
	return n;
}

/* The bytes of one term in the parts of SiftPartStats, as the writers lay them out (bm25/dict.h, bm25/posting.h) */
static void
account_term(SiftPartStats *ps, bool packed, int shared, int len, const SiftTermInfo *info, const SiftPostingCursor *c)
{
	uint64		df = info->df;
	bool		pos = (info->flags & SIFT_TERM_POS) != 0;
	bool		tf1 = (info->flags & SIFT_TERM_ALL_TF1) != 0;

	if (packed)
	{
		ps->dict_terms += 1 + (shared >= 15) + (len - shared - 1 >= 15) + len - shared;
		ps->dict_entries += 2 + (df >= 15 ? varint_len(df - 15) : 0);
	}
	else
	{
		ps->dict_terms += 2 + len - shared;
		ps->dict_entries += varint_len(df) + 2;
	}
	if (info->flags & SIFT_TERM_INLINE)
	{
		ps->inline_terms++;
		ps->dict_inline_keys += (packed ? varint_len(sift_dict_pack_key(info->inline_keys[0])) : varint_len(info->inline_keys[0])) +
			(df > 1 ? (packed ? varint_len(sift_dict_pack_key(info->inline_keys[1]) - sift_dict_pack_key(info->inline_keys[0])) :
					   varint_len(info->inline_keys[1] - info->inline_keys[0])) : 0) + (tf1 ? 0 : df);
		if (pos)
			ps->dict_inline_pos += varint_len(info->pos_len) + info->pos_len;
	}
	else
	{
		uint64		side = info->post_len - info->codec_len - (pos ? info->pos_len : 0);
		uint32		ng,
					nrows;

		ps->blob_terms++;
		ps->dict_entries += varint_len(info->post_len) + varint_len(info->post_len - info->codec_len) +
			(tf1 ? 0 : varint_len(info->nnib)) + (pos ? varint_len(info->pos_len) : 0);
		sift_post_group_stats(c, &ng, &nrows);
		if (info->flags & SIFT_TERM_SPARSE)
			ps->list_codec += info->codec_len;
		else
		{
			ps->grouped_dir += 20 + 12 * (uint64) ng;
			ps->grouped_payload += info->codec_len - 20 - 12 * (uint64) ng;
		}
		if (side > 0)
		{
			ps->align_pad += ALIGN4(info->codec_len) - info->codec_len;
			side -= ALIGN4(info->codec_len) - info->codec_len;
			ps->gscore += (uint64) ng * sizeof(SiftGroupScore);
			ps->blkbound += (uint64) nrows * SIFT_BLKS_PER_SB;
			side -= (uint64) ng * sizeof(SiftGroupScore) + (uint64) nrows * SIFT_BLKS_PER_SB;
			if (!tf1)
			{
				ps->tf_l1 += (df + 7) / 8;
				ps->tf_nib += ((uint64) info->nnib + 1) / 2;
				ps->tf_esc += side - (df + 7) / 8 - ((uint64) info->nnib + 1) / 2;
			}
		}
		if (pos)
		{
			ps->pos_ckpt += (df + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY * sizeof(uint32);
			ps->pos_records += info->pos_len - (df + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY * sizeof(uint32);
		}
	}
}

static bool
walk_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Walk	   *w = arg;
	SiftSegment *seg = w->seg;
	MemoryContext old = MemoryContextSwitchTo(w->tcxt);
	SiftPostingCursor *c;
	const SiftGroupBounds *b;
	int			nb,
				bi = 0,
				shared = 0;
	uint32		n = 0;
	uint64		pos = 0;
	SiftDfClassStats *cls = &w->st->cls[df_class(Max(info->df, 1))];

	if (w->verify && w->prevlen >= 0 &&
		(memcmp(w->prev, term, Min(len, w->prevlen)) > 0 ||
		 (memcmp(w->prev, term, Min(len, w->prevlen)) == 0 && w->prevlen >= len)))
		corrupt(seg, "Dictionary term \"%.*s\" follows \"%.*s\".", len, term, w->prevlen, w->prev);
	/* a block's first term is not front coded */
	if (w->st->nterms % SIFT_DICT_BLOCK != 0)
		while (shared < Min(len, w->prevlen) && term[shared] == w->prev[shared])
			shared++;
	memcpy(w->prev, term, len);
	w->prevlen = len;
	if (info->df > w->ndocs)
		corrupt(seg, "Term \"%.*s\" has df %u in a segment of %llu docs.", len, term, info->df, ULL(w->ndocs));
	if (w->verify && seg->fmt >= SIFT_SEGFMT_LIVEROWS &&
		info->slot != ((info->flags & SIFT_TERM_INLINE) ? -1 : (int32) w->nslots++))
		corrupt(seg, "Term \"%.*s\" has slot %d, expected %d.", len, term, info->slot,
				(info->flags & SIFT_TERM_INLINE) ? -1 : (int) w->nslots - 1);
	if (info->df > w->tcap)
	{
		w->tcap = Max(info->df, w->tcap * 2);
		w->tkeys = w->tkeys ? repalloc_huge(w->tkeys, w->tcap * sizeof(SiftKey)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(SiftKey));
		w->ttf = w->ttf ? repalloc_huge(w->ttf, w->tcap) : MemoryContextAllocHuge(seg->mcxt, w->tcap);
		w->tdl = w->tdl ? repalloc_huge(w->tdl, w->tcap * sizeof(uint16)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint16));
		w->tpoff = w->tpoff ? repalloc_huge(w->tpoff, w->tcap * sizeof(uint32)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint32));
		w->tplen = w->tplen ? repalloc_huge(w->tplen, w->tcap * sizeof(uint32)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint32));
	}
	c = w->verify ? sift_post_open_verified(&seg->sr, info, w->tcxt) : sift_post_open(&seg->sr, info, w->tcxt);
	if (w->verify)
		sift_post_check(c);
	b = sift_post_bounds(c, &nb);
	account_term(&w->st->parts, seg->fmt >= SIFT_SEGFMT_PACKED, shared, len, info, c);
	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g; g = sift_next(sift_post_cursor(c)))
	{
		uint32		k = sift_grp_tids(g, NULL, w->tids);

		if (n + k > info->df || (g->n && g->n != k) || g->base != n)
			corrupt(seg, "Term \"%.*s\" decodes past its df %u.", len, term, info->df);
		sift_post_group_tf(c, w->gtf);
		while (bi < nb && b[bi].gid < g->gid)
			bi++;
		if (bi == nb || b[bi].gid != g->gid)
			corrupt(seg, "Term \"%.*s\" has no bound for SB %u.", len, term, g->gid);
		for (uint32 j = 0; j < k; j++)
		{
			uint16		dl = doc_dl(w, w->tids[j], &pos);
			uint8		ub;

			if (n + j > 0 && w->tids[j] <= (j ? w->tids[j - 1] : w->tkeys[n - 1]))
				corrupt(seg, "Postings of \"%.*s\" are out of order.", len, term);
			w->tkeys[n + j] = w->tids[j];
			w->ttf[n + j] = w->gtf[j];
			w->tdl[n + j] = dl;
			ub = sift_bound_u8(&w->ref, w->gtf[j], dl);
			if (w->verify && (w->gtf[j] == 0 || ub > info->max_bound || ub > b[bi].bound ||
				ub > b[bi].blk_bound[(SIFT_BLK(w->tids[j]) & (SIFT_SB_PAGES - 1)) >> SIFT_BLK_SHIFT]))
				corrupt(seg, "Posting (%u,%u) of \"%.*s\" (tf %u, dl %u, bound %u) exceeds its bounds %u/%u/%u.",
						SIFT_BLK(w->tids[j]), SIFT_OFF(w->tids[j]), len, term, w->gtf[j], dl, ub,
						info->max_bound, b[bi].bound,
						b[bi].blk_bound[(SIFT_BLK(w->tids[j]) & (SIFT_SB_PAGES - 1)) >> SIFT_BLK_SHIFT]);
		}
		n += k;
	}
	if (n != info->df)
		corrupt(seg, "Term \"%.*s\" has %u postings, df %u.", len, term, n, info->df);
	w->tprec = NULL;
	if ((info->flags & SIFT_TERM_POS) && (w->verify || w->visit))
		term_positions(w, c, term, len, info);
	cls->terms++;
	cls->postings += n;
	if (info->flags & SIFT_TERM_INLINE)
		cls->inline_terms++;
	else
	{
		cls->list_terms += (info->flags & SIFT_TERM_SPARSE) != 0;
		cls->grouped_terms += (info->flags & SIFT_TERM_SPARSE) == 0;
		cls->codec_bytes += info->codec_len;
		cls->sidecar_bytes += info->post_len - info->codec_len - ((info->flags & SIFT_TERM_POS) ? info->pos_len : 0);
	}
	w->st->nterms++;
	w->st->postings += n;
	MemoryContextSwitchTo(old);
	w->stopped = w->visit && !w->visit(w->arg, term, len, info, w->tkeys, w->ttf, w->tdl, w->tprec, w->tplen);
	MemoryContextReset(w->tcxt);
	return !w->stopped;
}

void
sift_seg_walk(SiftSegment *seg, const SiftScoreRef *ref, bool verify, SiftSegStats *st,
			  SiftTermVisit visit, void *arg)
{
	Walk		w = {0};
	const SiftSectionRef *s = seg->hdr->sections;

	memset(st, 0, sizeof(SiftSegStats));
	w.seg = seg;
	w.ref = *ref;
	w.ref.avgdl = seg->hdr->avgdl_ref;
	w.verify = verify;
	w.st = st;
	w.visit = visit;
	w.arg = arg;
	w.prevlen = -1;
	w.tcxt = AllocSetContextCreate(CurrentMemoryContext, "sift walk term", ALLOCSET_DEFAULT_SIZES);
	w.tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	w.gtf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	st->dict_bytes = s[SIFT_SEC_DICT].len + s[SIFT_SEC_DICTIDX].len + seg->hdr->dictroot_n * sizeof(uint64);
	walk_docs(&w);
	if (verify && seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		verify_live2(&w);
	else if (verify)
		verify_live(&w);
	sift_dict_scan(seg->dict, NULL, 0, NULL, 0, walk_term, &w);
	if (!w.stopped && st->nterms != seg->hdr->nterms)
		corrupt(seg, "Dictionary holds %llu terms, the header %llu.", ULL(st->nterms), ULL(seg->hdr->nterms));
	st->parts.docmap_dir = (uint64) docmap(seg)->nsb * sizeof(SiftDocSBRef) + sizeof(uint32);
	st->parts.dictidx = s[SIFT_SEC_DICTIDX].len + seg->hdr->dictroot_n * sizeof(uint64);
	st->parts.dict_blocks = s[SIFT_SEC_DICT].len - st->parts.dict_terms - st->parts.dict_entries - st->parts.dict_inline_keys -
		st->parts.dict_inline_pos;
	if (verify && !w.stopped && seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		verify_dead2(&w);
	MemoryContextDelete(w.tcxt);
}
