/*
 * sift_dict.h - per-segment term dictionary.
 *
 * DICT: front-coded blocks of SIFT_DICT_BLOCK terms in byte order. Block:
 *   u16 block bytes, u8 nterms, varint post_base (offset of the block's first
 *   term blob), then per term: u8 shared prefix, u8 suffix length, suffix,
 *   varint df, u8 flags, u8 max_bound, and either the inline postings
 *   (varint key0 [, varint key1 - key0], tf bytes unless SIFT_TERM_ALL_TF1,
 *   then if SIFT_TERM_POS varint pos_len and the position records of the postings)
 *   or varint post_len, varint post_len - codec_len, [varint nnib unless
 *   SIFT_TERM_ALL_TF1] [varint pos_len if SIFT_TERM_POS]. Term blobs follow each other in term
 *   order, each at the next 4-byte aligned offset, so post_off is implied.
 * DICTIDX: SiftDictIdx per block (slot_base: the number of terms with a counter slot before the block, see SiftTermInfo.slot; 0 in
 * segments written before format 2, whose terms have no slot); every 256th prefix forms the root array
 * (u64[dictroot_n]) kept in memory with the opened dictionary. A lookup
 * touches the root array, <= 1 DICTIDX page (binary search) and one block.
 */
#ifndef SIFT_DICT_H
#define SIFT_DICT_H

#include "sift.h"
#include "sift_query.h"
#include "sift_store.h"

#define SIFT_DICT_BLOCK			32
#define SIFT_DICT_ROOT_FANOUT	256
#define SIFT_INLINE_MAX			2	/* df <= this: postings inline in the entry */
#define SIFT_INLINE_POS_MAX		256	/* longer position records are dropped: a dictionary block stays under 64 KB */

#define SIFT_TERM_SPARSE		0x01	/* LIST codec blob (no group scores) */
#define SIFT_TERM_INLINE		0x02	/* postings in inline_keys/inline_tf */
#define SIFT_TERM_ALL_TF1		0x04	/* every tf == 1: no tf stream */
#define SIFT_TERM_POS			0x08	/* the postings have position records (sift_posting.h): at the end of the term blob, or inline */

/* A segment's DICT section starts its last block within this many bytes (offsets are uint32); sift.test_dict_max lowers it in SIFT_TESTING builds */
#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_dict_max;
#define SIFT_DICT_MAX	((uint64) (sift_test_dict_max > 0 ? sift_test_dict_max : PG_UINT32_MAX))
#else
#define SIFT_DICT_MAX	((uint64) PG_UINT32_MAX)
#endif

typedef struct SiftTermInfo
{
	uint32		df;
	uint8		flags;
	uint8		max_bound;		/* u8 bound over all postings */
	uint16		reserved;
	uint64		post_off;		/* term blob in the POSTINGS section */
	uint32		post_len;
	uint32		codec_len;		/* codec blob at the start of the term blob */
	uint32		nnib;			/* L1 members (tf > 1) */
	uint32		pos_len;		/* SIFT_TERM_POS: bytes at the end of the term blob, or of inline_pos */
	const uint8 *inline_pos;	/* inline term: its records back to back, in the dictionary block or the writer */
	SiftKey		inline_keys[SIFT_INLINE_MAX];
	uint8		inline_tf[SIFT_INLINE_MAX];
	uint32		ldf;			/* df of the live docs: the dictionary's df until sift_seg_term_stats takes the vacuumed postings off */
	int32		slot;			/* of the term in the segment's dead-postings table, -1 for an inline term (set by the reader) */
} SiftTermInfo;

/* On-disk DICTIDX entry */
typedef struct SiftDictIdx
{
	uint64		prefix;			/* first term's first 8 bytes, big-endian, zero padded */
	uint32		block_off;		/* from the DICT section start */
	uint32		slot_base;		/* terms with a slot before the block (0 before segment format 2) */
} SiftDictIdx;

/*
 * Writer: terms must arrive in strictly increasing byte order, right after
 * their postings (term blobs back to back from the stream position at
 * begin). DICT bytes are spooled to a temporary file (bounded memory) and
 * copied into sw by finish.
 */
typedef struct SiftDictWriter SiftDictWriter;

extern SiftDictWriter *sift_dict_writer_begin(SiftStreamWriter *sw);
extern void sift_dict_add(SiftDictWriter *w, const char *term, int len, const SiftTermInfo *info);

/* Terms added so far that are not inline: each has a slot (0, 1, ...) in dictionary order */
extern uint32 sift_dict_writer_nslots(const SiftDictWriter *w);

/* Writes DICT then DICTIDX then the root array into sw; fills the header fields */
extern void sift_dict_writer_finish(SiftDictWriter *w, SiftSegHeaderData *hdr);

/* Reader, opened per segment by sift_seg_open() (allocates in mcxt) */
typedef struct SiftDict SiftDict;

extern SiftDict *sift_dict_open(SiftStreamReader *sr, const SiftSegHeaderData *hdr, MemoryContext mcxt);
extern bool sift_dict_lookup(SiftDict *d, const char *term, int len, SiftTermInfo *info);

/*
 * Visits terms in [lo, hi) (lo NULL = from the start, hi NULL = unbounded)
 * in byte order; return false from fn to stop. fn may use the stream
 * reader (blocks are copied before the visit).
 */
typedef bool (*SiftDictVisit) (void *arg, const char *term, int len, const SiftTermInfo *info);
extern void sift_dict_scan(SiftDict *d, const char *lo, int lolen, const char *hi, int hilen,
						   SiftDictVisit fn, void *arg);

/*
 * Term matchers for expansion leaves (SQ_PREFIX, SQ_WILDCARD, SQ_REGEX,
 * SQ_FUZZY, also SQ_TERM). bounds give the byte range implied by the
 * literal prefix (NULL: unbounded); allocates in CurrentMemoryContext.
 */
#ifdef SIFT_TESTING
/* Blocks a term lookup read to find its block (sift._test_read_stats) */
extern uint64 sift_dict_probes;
#endif

typedef struct SiftTermMatcher SiftTermMatcher;

extern SiftTermMatcher *sift_matcher_create(const SiftQNode *leaf);
extern void sift_matcher_bounds(const SiftTermMatcher *m, const char **lo, int *lolen,
								const char **hi, int *hilen);
extern bool sift_matcher_match(const SiftTermMatcher *m, const char *term, int len);

#endif							/* SIFT_DICT_H */
