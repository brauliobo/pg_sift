/*
 * bm25/fielddl.c - the length of the fields of the docs of a segment (SIFT_SEGFMT_FIELDDL): the sums over its live docs, and the lengths of a
 * segment written before they were kept, which a merge rewrites with them.
 */
#include "core/host.h"

#include "core/memtrack.h"
#include "bm25/analyze/analyze.h"
#include "bm25/bitmap/bitmap.h"
#include "bm25/kernels/kernels.h"
#include "bm25/segment/int.h"

int
sift_seg_index_nfields(SiftSegment *seg)
{
#if defined(SIFT_HOST_SQLITE) || defined(SIFT_HOST_MARIADB)
	return 0;
#else
	SiftAnalyzerConfig cfg;

	if (seg->fmt >= SIFT_SEGFMT_FIELDDL)
		return sift_seg_nfields(seg);
	sift_analyzer_config_from_index(seg->index, &cfg);
	return cfg.nfields;
#endif
}

/* The length of each field, over the docs of the segment that are vacuumed */
static void
dead_field_sums(SiftSegment *seg, int nf, uint64 *dead)
{
	SiftDocChunk dc;

	memset(dead, 0, nf * sizeof(uint64));
	for (uint32 sb = seg->hdr->sb_lo; sb <= seg->hdr->sb_hi && sift_seg_docs_next(seg, sb, &dc); sb = dc.sb + 1)
	{
		if (!dc.dirty)
			continue;
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			uint64		present[SIFT_OFFMASK_WORDS],
						live[SIFT_OFFMASK_WORDS];

			if (!sift_seg_page(seg, &dc, p, present, NULL))
				continue;
			sift_seg_page_live(seg, &dc, p, live);
			for (int f = 0; f < nf; f++)
			{
				const uint16 *rows[SIFT_BLK_PAGES];
				const uint16 *row;

				sift_seg_block_field_dl(seg, &dc, p / SIFT_BLK_PAGES, 1U << (p % SIFT_BLK_PAGES), f, rows);
				row = rows[p % SIFT_BLK_PAGES];
				for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
					for (uint64 m = present[wd] & ~live[wd]; m; m &= m - 1)
						dead[f] += row[wd * 64 + __builtin_ctzll(m)];
			}
		}
	}
}

void
sift_seg_live_field_sums(SiftSegment *seg, uint64 *sum_dl)
{
	int			nf = sift_seg_nfields(seg);
	uint64		ndocs,
				sum;

	sift_seg_field_sums(seg, sum_dl);
	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return;
	sift_seg_live_totals(seg, &ndocs, &sum);
	if (ndocs < seg->hdr->ndocs)
	{
		uint64	   *dead;
		void	   *handle;

		/* the docs are only ever vacuumed, so the number of dead ones names the set the sums are of */
		if (!sift_seg_cache_get(seg, SIFT_SEGCACHE_FIELDDEAD, seg->hdr->ndocs - ndocs, nf * sizeof(uint64), (void **) &dead, &handle))
		{
			dead_field_sums(seg, nf, dead);
			if (handle)
				sift_segcache_ready(handle);
		}
		for (int f = 0; f < nf; f++)
			sum_dl[f] -= dead[f];
	}
}

typedef struct LegacyFields
{
	SiftSegment *seg;
	int			nf;
	uint16	   *dl;				/* [rank][field] over all the docs of the segment */
	MemoryContext cx;
	sift_tid   *tids;
	uint8	   *tf;
} LegacyFields;

/* Rank in the segment of the doc at page, offset off of the loaded SB (its present docs in key order) */
static uint64
doc_rank(const SiftDocChunk *dc, const DocMap *dm, int page, int off)
{
	const uint64 *om = dm->om + dm->maskidx[page] * SIFT_OFFMASK_WORDS;
	uint64		rank = dc->rank_base + dm->before[page];

	for (int w = 0; w < off / 64; w++)
		rank += sift_pc64(om[w]);
	return rank + sift_pc64(om[off / 64] & ((UINT64CONST(1) << (off % 64)) - 1));
}

/* A word of field f: its postings add their tf to the length of their docs */
static bool
legacy_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	LegacyFields *lf = arg;
	SiftPostingCursor *c;
	int			f = (unsigned char) term[0] - SIFT_FIELD_BASE;

	if (len < 2 || term[1] == SIFT_PREFIX_SEP)
		return true;
	c = sift_seg_post_open(lf->seg, info, lf->cx);
	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g; g = sift_next(sift_post_cursor(c)))
	{
		SiftDocChunk dc;
		uint32		k = sift_grp_tids(g, NULL, lf->tids);
		DocMap	   *dm;

		sift_post_group_tf(c, lf->tf);
		SIFT_INVARIANT(sift_seg_docs(lf->seg, g->gid, &dc), "sift: a posting lies in SB %u, which has no docs", g->gid);
		dm = indexed(lf->seg, &dc);
		for (uint32 j = 0; j < k; j++)
		{
			SiftKey		key = lf->tids[j];
			uint16	   *dl = &lf->dl[doc_rank(&dc, dm, sift_key_block(key) & (SIFT_SB_PAGES - 1), (key & 0xFFFF) - 1) * lf->nf + f];

			*dl = Min(*dl + lf->tf[j], SIFT_MAX_DL);
		}
	}
	MemoryContextReset(lf->cx);
	return true;
}

uint16 *
sift_seg_legacy_field_dl(SiftSegment *seg, int nf, MemoryContext mcxt)
{
	LegacyFields lf = {.seg = seg,.nf = nf};
	Size		bytes = (Size) seg->hdr->ndocs * nf * sizeof(uint16);
	char		lo = SIFT_FIELD_BASE,
				hi = SIFT_FIELD_BASE + nf;

	sift_budget_check(bytes, (int) (sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) / 1024), "the field lengths of a segment of an older format", "maintenance_work_mem", NULL);
	lf.dl = MemoryContextAllocExtended(mcxt, bytes, MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	lf.cx = sift_mem_allocset_create(CurrentMemoryContext, "sift legacy field lengths");
	lf.tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	lf.tf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	sift_dict_scan(seg->dict, &lo, 1, &hi, 1, legacy_term, &lf);
	MemoryContextDelete(lf.cx);
	pfree(lf.tids);
	pfree(lf.tf);
	return lf.dl;
}

void
sift_seg_doc_fields(SiftSegment *seg, const SiftDocChunk *dc, int page, int off, const uint16 *legacy, int nf, uint16 *out)
{
	if (legacy)
	{
		memcpy(out, legacy + doc_rank(dc, indexed(seg, dc), page, off) * nf, nf * sizeof(uint16));
		return;
	}
	for (int f = 0; f < nf; f++)
	{
		const uint16 *rows[SIFT_BLK_PAGES];

		sift_seg_block_field_dl(seg, dc, page / SIFT_BLK_PAGES, 1U << (page % SIFT_BLK_PAGES), f, rows);
		out[f] = rows[page % SIFT_BLK_PAGES][off];
	}
}

void
sift_seg_verify_fields(SiftSegment *seg)
{
	int			nf = sift_seg_nfields(seg);
	uint16	   *want = sift_seg_legacy_field_dl(seg, nf, CurrentMemoryContext);
	uint64		sum[SIFT_MAX_FIELDS] = {0},
				nz[SIFT_MAX_FIELDS] = {0},
				stored[SIFT_MAX_FIELDS];
	SiftDocChunk dc;

	for (uint32 sb = seg->hdr->sb_lo; sb <= seg->hdr->sb_hi && sift_seg_docs_next(seg, sb, &dc); sb = dc.sb + 1)
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			uint64		present[SIFT_OFFMASK_WORDS];

			if (!sift_seg_page(seg, &dc, p, present, NULL))
				continue;
			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
				for (uint64 m = present[wd]; m; m &= m - 1)
				{
					int			o = wd * 64 + __builtin_ctzll(m);
					uint16		got[SIFT_MAX_FIELDS];
					const uint16 *exp = want + doc_rank(&dc, indexed(seg, &dc), p, o) * nf;

					sift_seg_doc_fields(seg, &dc, p, o, NULL, nf, got);
					for (int f = 0; f < nf; f++)
					{
						if (got[f] != exp[f])
							corrupt(seg, "Doc (%u,%d) has length %u in field %d, the tf of its words there add up to %u.",
									(dc.sb * SIFT_SB_PAGES + p), o + 1, got[f], f + 1, exp[f]);
						sum[f] += got[f];
						nz[f] += got[f] > 0;
					}
				}
		}
	sift_seg_field_sums(seg, stored);
	for (int f = 0; f < nf; f++)
		if (sum[f] != stored[f] || nz[f] != docmap(seg)->ft->ndocs_nz[f])
			corrupt(seg, "Field %d has length %llu over %llu docs, its trailer %llu over %llu.", f + 1, ULL(sum[f]), ULL(nz[f]),
					ULL(stored[f]), ULL(docmap(seg)->ft->ndocs_nz[f]));
	pfree(want);
}
