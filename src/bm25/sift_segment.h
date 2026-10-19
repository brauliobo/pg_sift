/*
 * sift_segment.h - immutable segments: writer (build, promotion, merge) and
 * reader (docmap, liveness, dictionary, term cursors).
 *
 * Stream layout: DOCMAP, POSTINGS, DICT, DICTIDX, dictionary root array.
 * DOCMAP: one record per SB with docs, in SB order, each 4-byte aligned:
 *   u32 ndocs, u32 chunk_len, codec chunk of the present docs
 *   (sift_chunk_encode), zero pad to 4, then for every present page in page
 *   order a u16 dl row indexed by offset (slot off-1) up to the page's
 *   highest present offset (absent slots 0); then the SB directory
 *   SiftDocSBRef[nsb] and a final u32 nsb. A doc's rank (liveness bit) is
 *   its position in key order. Liveness: one bit per doc in rank order, plus
 *   a dirty bit per SB (LIVE pages, see sift_page.h).
 */
#ifndef SIFT_SEGMENT_H
#define SIFT_SEGMENT_H

#include "sift_host.h"

#include "sift_dict.h"
#include "sift_posting.h"

typedef struct SiftDocSBRef
{
	uint32		sb;
	uint32		ndocs;
	uint64		off;			/* record offset in the stream */
} SiftDocSBRef;

/*
 * Writer: docs first (strictly ascending key; dl 0 for text without
 * tokens), then terms (strictly ascending bytes, possibly none). avgdl_ref
 * is the segment's own avgdl, so only k1 and b of ref are used. positions:
 * the terms' postings carry position records (SIFT_SEG_POSITIONS).
 */
typedef struct SiftSegmentWriter SiftSegmentWriter;

#ifdef SIFT_TESTING
/* sift.test_segfmt: the segment format a build, promotion or merge writes (0 = the build's own, SIFT_WRITE_SEGFMT) */
extern PGDLLIMPORT int sift_test_segfmt;
#endif

extern SiftSegmentWriter *sift_segw_begin(SiftWriteTarget *wt, uint64 gen,
										  const SiftScoreRef *ref, MemoryContext mcxt, bool positions);
extern void sift_segw_add_docs(SiftSegmentWriter *w, const SiftKey *keys, const uint16 *dl, int n);

/* Terms of min_postings postings or more are staged in temporary files as they arrive (sift_postw_spool), before the first term */
extern void sift_segw_spool(SiftSegmentWriter *w, uint64 min_postings);

/* Returns the posting writer for this term; finish it with sift_segw_end_term */
extern SiftPostingWriter *sift_segw_begin_term(SiftSegmentWriter *w, const char *term, int len);
extern void sift_segw_end_term(SiftSegmentWriter *w);

/* Writes sections, LIVE pages (all live) and the header; fills the directory entry */
extern void sift_segw_finish(SiftSegmentWriter *w, SiftSegEntry *entry);

/* Reader: opened per scan or maintenance pass; everything lives in mcxt */
typedef struct SiftSegment
{
	Relation	index;
	SiftSegEntry entry;
	SiftSegHeaderData *hdr;		/* copy of the header page */
	SiftStreamReader sr;
	SiftDict   *dict;
	MemoryContext mcxt;
	void	   *docmap;			/* private: SB directory and the loaded SB */
	void	   *live;			/* private: dirty-SB bitmap */
	void	   *l2;				/* private: format 2 liveness (header, rows of the SB read last) */
	uint8		fmt;			/* segment format: SIFT_SEGFMT_* */
} SiftSegment;

/* Reads the header and the dictionary root; ERRCODE_INDEX_CORRUPTED on format errors */
extern SiftSegment *sift_seg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt);
extern void sift_seg_close(SiftSegment *seg);	/* releases buffers only */

/* Docs of one SB; false when the segment has none there */
typedef struct SiftDocChunk
{
	uint32		sb;
	uint32		ndocs;
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	uint64		rank_base;		/* segment rank of the SB's first doc */
	bool		dirty;			/* some doc in this SB was vacuumed */
} SiftDocChunk;

extern bool sift_seg_docs(SiftSegment *seg, uint32 sb, SiftDocChunk *dc);

/*
 * The SB directory of the segment's docmap (nsb entries) and the doc rank where each SB starts (nsb + 1),
 * for the caller to fill: false when it must read them, true when they are those of an earlier query
 * (sift_docdir.c). Call sift_docdir_ready(handle) once filled and checked. Both live as long as seg's memory context.
 */
extern bool sift_docdir_get(SiftSegment *seg, uint32 nsb, SiftDocSBRef **dir, uint64 **rank_base, void **handle);
extern void sift_docdir_ready(void *handle);
extern PGDLLIMPORT int sift_docdir_min_sb;

#ifdef SIFT_TESTING
/* Directories read from the segment and found in the backend's cache (sift._test_read_stats) */
typedef struct SiftDocDirStats
{
	uint64		loads;
	uint64		hits;
	uint64		bytes_read;		/* of directories, cached or not */
} SiftDocDirStats;
extern SiftDocDirStats sift_docdir_stats;
#endif

/* First SB >= sb with docs; false past the last one */
extern bool sift_seg_docs_next(SiftSegment *seg, uint32 sb, SiftDocChunk *dc);

/*
 * Present offsets of page (sb*256 + page) and their dl row (dl[off-1]);
 * present is zero and dl NULL for pages without docs (false). present and dl
 * may be NULL when the caller wants neither: the rows of format 3 are narrower than 16
 * bits on disk and expanded on request. dl valid until the next docmap call
 * on seg.
 */
extern bool sift_seg_page(SiftSegment *seg, const SiftDocChunk *dc, int page,
						  uint64 *present, const uint16 **dl);

/*
 * The dl rows of the pages of block (SIFT_BLK_PAGES pages) of the SB in dc whose bit is set in want, NULL for the pages that have no docs
 * or are not wanted; valid until the next docmap call on seg
 */
extern void sift_seg_block_dl(SiftSegment *seg, const SiftDocChunk *dc, int block, uint32 want, const uint16 **rows);

/* Live offsets of a page (== present unless dc->dirty) */
extern void sift_seg_page_live(SiftSegment *seg, const SiftDocChunk *dc, int page,
							   uint64 *live);

/* Does the SB hold vacuumed docs, known without reading its docs (false when the segment has none there) */
extern bool sift_seg_sb_dirty(SiftSegment *seg, uint32 sb);

/*
 * The docs and tokens of the segment that are live. The header's count of a segment of format 1 holds its vacuumed docs too; in
 * format 2 the liveness header keeps the dead ones, so the totals are exact.
 */
extern void sift_seg_live_totals(SiftSegment *seg, uint64 *ndocs, uint64 *sum_dl);

/*
 * Do the dfs the segment's terms carry (info.ldf) count its live docs: its table of dead postings is the one of the liveness rows
 * now and was when the segment read its first df (a VACUUM changes the rows first, the table after, in its cleanup). False for a
 * vacuumed segment of format 1
 */
extern bool sift_seg_live_stable(SiftSegment *seg);

/* Is the doc of the key live (true in a clean SB, and for a key the segment does not hold) */
extern bool sift_seg_key_live(SiftSegment *seg, SiftKey key);

/*
 * Sets info->ldf, the df of the live docs, of a term the dictionary gave (format 2: its df less the dead postings of its slot,
 * or the live ones of its inline keys). info->df, which sizes the posting lists, stays.
 */
extern void sift_seg_term_stats(SiftSegment *seg, SiftTermInfo *info);

/*
 * For a dirty SB (sift_seg_sb_dirty): the live postings of the pages of group g that pages, a subset of g's, selects, and the live offsets
 * of one page among a mask's. They read the SB's docs only in format 1; format 2 keeps the live offsets of the SB's pages in its liveness run
 */
extern uint64 sift_seg_live_count(SiftSegment *seg, const sift_grp *g, const uint64 *pages);
extern int	sift_seg_live_mask(SiftSegment *seg, uint32 sb, int page, uint64 *mask);

/* Keeps in mask, a subset of the present offsets of a page of a dirty SB (dc->dirty), the live ones; returns how many are left */
extern int	sift_seg_page_mask_live(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *mask);

extern bool sift_seg_lookup(SiftSegment *seg, const char *term, int len, SiftTermInfo *info);

/* Does the dictionary term still have a posting of a live doc (always true in a clean segment) */
extern bool sift_seg_term_live(SiftSegment *seg, const SiftTermInfo *info);

/*
 * VACUUM: calls cb for every live doc; clears liveness bits and sets dirty
 * SB bits for deleted ones (generic WAL, <= 4 pages per record). Caller holds
 * the maintenance lock. Returns docs removed; adds to stats.
 */
extern uint64 sift_seg_live_docs(SiftSegment *seg);

/*
 * The rank of the live doc key (its position among the segment's docs, which indexes the liveness bits), -1 when the
 * segment holds no live doc with it; and the deletion of that one doc, the same way VACUUM deletes (the dirty flag, then the
 * bits): true when it was live. The caller holds the maintenance lock (a host without one has a single writer).
 */
extern int64 sift_seg_live_rank(SiftSegment *seg, SiftKey key);
extern bool sift_seg_delete_key(SiftSegment *seg, SiftKey key);
/*
 * Counts the vacuumed postings of each term again, when docs were cleared since they were counted. VACUUM does it after the merges,
 * so a segment it rewrites does not pay for a table that is dropped; until then the old table stands (see sift_seg_live_stable)
 */
extern void sift_seg_refresh_dead(SiftSegment *seg);

extern int64 sift_seg_bulkdelete(SiftSegment *seg, IndexBulkDeleteCallback cb, void *cbstate,
								 IndexBulkDeleteResult *stats);

/* ---------- inspection (sift.segments, sift.verify, tests) ---------- */

#define SIFT_DF_CLASSES 9

typedef struct SiftDfClassStats
{
	uint64		terms;
	uint64		postings;
	uint64		inline_terms;
	uint64		list_terms;
	uint64		grouped_terms;
	uint64		codec_bytes;	/* inline terms: 0 */
	uint64		sidecar_bytes;	/* group scores, block bounds, tf stream, padding */
} SiftDfClassStats;

/* Where the bytes of a segment go (sift._test_segment_parts): the DOCMAP, the blobs of the terms with postings and the DICT */
typedef struct SiftPartStats
{
	uint64		docmap_dl;		/* dl rows */
	uint64		docmap_presence;	/* the codec chunk of each SB's present docs, its headers and padding */
	uint64		docmap_dir;		/* SB directory */
	uint64		list_codec;		/* LIST blobs */
	uint64		grouped_dir;	/* GROUPED blobs: header, key/poff/base */
	uint64		grouped_payload;	/* ... the containers */
	uint64		gscore;			/* SiftGroupScore */
	uint64		blkbound;		/* block bound rows */
	uint64		tf_l1;
	uint64		tf_nib;
	uint64		tf_esc;
	uint64		align_pad;		/* between the codec blob and its sidecar */
	uint64		pos_ckpt;		/* positions of the blobs: checkpoints */
	uint64		pos_records;	/* ... and records */
	uint64		dict_terms;		/* front-coded suffixes and their two length bytes */
	uint64		dict_entries;	/* df, flags, bound and, for a blob, its lengths */
	uint64		dict_inline_keys;	/* inline keys and tf bytes */
	uint64		dict_inline_pos;	/* inline position records and their length */
	uint64		dict_blocks;	/* block headers */
	uint64		dictidx;		/* DICTIDX and root array */
	uint64		inline_terms;
	uint64		blob_terms;
} SiftPartStats;

typedef struct SiftSegStats
{
	uint64		nterms;
	uint64		postings;
	uint64		dict_bytes;		/* DICT + DICTIDX + root */
	SiftDfClassStats cls[SIFT_DF_CLASSES];
	SiftPartStats parts;
} SiftSegStats;

/* df class bounds: class i holds lo[i] <= df < lo[i + 1] */
extern const uint32 sift_df_class_lo[SIFT_DF_CLASSES];
extern const char *const sift_df_class_name[SIFT_DF_CLASSES];

/*
 * One decoded term: postings in key order with tf and the doc's dl; pos / poslen (NULL without
 * positions, SIFT_TERM_POS) are the postings' position records back to back and their lengths
 */
typedef bool (*SiftTermVisit) (void *arg, const char *term, int len, const SiftTermInfo *info,
							   const SiftKey *keys, const uint8 *tf, const uint16 *dl,
							   const uint8 *pos, const uint32 *poslen);

/*
 * Walks every term of the segment (dictionary order) and decodes its
 * postings. With verify, checks the docmap, liveness, dictionary order and
 * counts, postings (subset of the docmap, count == df, tf stream lengths,
 * group and block bounds >= the true maximum computed with ref's k1 and b
 * and the segment's avgdl_ref) and raises ERRCODE_INDEX_CORRUPTED. visit
 * may be NULL. Memory: O(segment docs) for the dl map.
 */
extern void sift_seg_walk(SiftSegment *seg, const SiftScoreRef *ref, bool verify,
						  SiftSegStats *st, SiftTermVisit visit, void *arg);

/*
 * Dumps the live part of the segment: docs first (ascending key, dl), then every term with a live posting in dictionary
 * order (info->df is the live count). Memory: O(segment docs) plus the largest term. ref supplies k1 and b.
 */
typedef void (*SiftDocVisit) (void *arg, const SiftKey *keys, const uint16 *dl, int n);

extern void sift_seg_dump(SiftSegment *seg, const SiftScoreRef *ref, SiftDocVisit docs,
						  SiftTermVisit terms, void *arg);

/*
 * The same for a merge (the runs of sift_run_from_segment), in memory that does not grow with the segment. term() comes first
 * with info->df the number of live postings, then postings() until they are delivered: key order, tf, the dl of the doc, and with
 * SIFT_TERM_POS in info->flags the records back to back and their lengths. A term comes in batches of one group (a superblock)
 * at most, except a small term of a vacuumed segment, which is counted first and then comes in one. The memory is the dl of
 * the docs of the SBs a term touches, kept for the next term (budget bytes at most, 2.3 bytes a doc; a segment that does not
 * fit has its SBs read again for each term that touches them), a batch, and the blob of the term, which sift_post_open
 * copies whole: about a byte a posting of the hottest term.
 */
typedef struct SiftSegSink
{
	void		(*docs) (void *arg, const SiftKey *keys, const uint16 *dl, int n);
	void		(*term) (void *arg, const char *term, int len, const SiftTermInfo *info);
	void		(*postings) (void *arg, const SiftKey *keys, const uint8 *tf, const uint16 *dl, const uint8 *pos,
							 const uint32 *poslen, int n);
} SiftSegSink;

extern void sift_seg_stream(SiftSegment *seg, Size budget, const SiftSegSink *sink, void *arg);

#endif							/* SIFT_SEGMENT_H */
