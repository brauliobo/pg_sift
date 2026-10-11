/*
 * sift_dict.h - per-segment term dictionary.
 *
 * DICT: front-coded blocks of SIFT_DICT_BLOCK terms in byte order.
 * DICTIDX: one entry per block {u64 big-endian 8-byte prefix, offsets}; every
 * 256th entry forms the root array kept in memory with the opened segment.
 * Lookup touches <= 3 pages.
 */
#ifndef SIFT_DICT_H
#define SIFT_DICT_H

#include "sift.h"
#include "sift_query.h"
#include "sift_store.h"

#define SIFT_DICT_BLOCK			32
#define SIFT_INLINE_MAX			2	/* df <= this: postings inline in the entry */

#define SIFT_TERM_SPARSE		0x01	/* sparse term blob layout */
#define SIFT_TERM_INLINE		0x02	/* postings in inline_keys/inline_tf */
#define SIFT_TERM_ALL_TF1		0x04	/* every tf == 1 */

typedef struct SiftTermInfo
{
	uint32		df;
	uint8		flags;
	uint8		max_bound;		/* u8 bound over all postings */
	uint16		reserved;
	uint64		post_off;		/* term blob in the POSTINGS section */
	uint32		post_len;
	SiftKey		inline_keys[SIFT_INLINE_MAX];
	uint8		inline_tf[SIFT_INLINE_MAX];
} SiftTermInfo;

/* Writer: terms must arrive in strictly increasing byte order */
typedef struct SiftDictWriter SiftDictWriter;

extern SiftDictWriter *sift_dict_writer_begin(SiftStreamWriter *sw);
extern void sift_dict_add(SiftDictWriter *w, const char *term, int len, const SiftTermInfo *info);

/* Writes DICT then DICTIDX then the root array into sw; fills the header fields */
extern void sift_dict_writer_finish(SiftDictWriter *w, SiftSegHeaderData *hdr);

/* Reader, opened per segment by sift_seg_open() (allocates in its context) */
typedef struct SiftDict SiftDict;

extern SiftDict *sift_dict_open(SiftStreamReader *sr, const SiftSegHeaderData *hdr, MemoryContext mcxt);
extern bool sift_dict_lookup(SiftDict *d, const char *term, int len, SiftTermInfo *info);

/* Visits terms in [lo, hi) (hi NULL = unbounded); return false from fn to stop */
typedef bool (*SiftDictVisit) (void *arg, const char *term, int len, const SiftTermInfo *info);
extern void sift_dict_scan(SiftDict *d, const char *lo, int lolen, const char *hi, int hilen,
						   SiftDictVisit fn, void *arg);

/*
 * Term matchers for expansion leaves (SQ_PREFIX, SQ_WILDCARD, SQ_REGEX,
 * SQ_FUZZY). bounds give the byte range implied by the literal prefix.
 */
typedef struct SiftTermMatcher SiftTermMatcher;

extern SiftTermMatcher *sift_matcher_create(const SiftQNode *leaf);
extern void sift_matcher_bounds(const SiftTermMatcher *m, const char **lo, int *lolen,
								const char **hi, int *hilen);
extern bool sift_matcher_match(const SiftTermMatcher *m, const char *term, int len);

#endif							/* SIFT_DICT_H */
