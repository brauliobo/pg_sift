/*
 * sift_analyze.h - document and query analysis (tokenize, fold, stem).
 *
 * Tokens are maximal runs of Unicode letters, digits and combining marks;
 * each emoji (Extended_Pictographic) code point is its own token. Folding is
 * full case folding (unicode_strfold) and accent stripping (NFKD minus Mn).
 * Stemming calls a text search dictionary (e.g. english_stem) via lexize.
 * SQL_ASCII databases treat bytes >= 0x80 as opaque letters. Tokens longer
 * than SIFT_MAX_TERM_BYTES are truncated at a character boundary.
 *
 * Index and query sides must use the same SiftAnalyzerConfig; its hash is
 * stored in the metapage.
 */
#ifndef SIFT_ANALYZE_H
#define SIFT_ANALYZE_H

#include "sift.h"

typedef struct SiftAnalyzerConfig
{
	bool		case_fold;
	bool		accent_fold;
	Oid			stem_dict;		/* pg_ts_dict OID, InvalidOid = no stemming */
	uint32		hash;			/* of the three fields above */
} SiftAnalyzerConfig;

/* A token in a document token stream; str points into analyzer memory */
typedef struct SiftToken
{
	const char *str;
	uint16		len;
	uint32		pos;			/* token ordinal, 0-based */
	uint32		start;			/* byte offsets into the source text */
	uint32		end;
} SiftToken;

/* A distinct term of a document with its frequency */
typedef struct SiftTermFreq
{
	const char *term;
	uint16		len;
	uint8		tf;				/* saturated at SIFT_MAX_TF */
	uint32		hash;			/* sift_term_hash(term, len) */
} SiftTermFreq;

typedef struct SiftAnalyzer SiftAnalyzer;

/* Default config: folding on, no stemming */
extern void sift_analyzer_config_default(SiftAnalyzerConfig *cfg);
extern void sift_analyzer_config_from_options(const SiftOptions *opts, SiftAnalyzerConfig *cfg);
extern void sift_analyzer_config_from_index(Relation index, SiftAnalyzerConfig *cfg);

/*
 * The analyzer owns reusable buffers allocated in mcxt. Results of the
 * functions below stay valid until the next call on the same analyzer or
 * sift_analyzer_reset().
 */
extern SiftAnalyzer *sift_analyzer_create(const SiftAnalyzerConfig *cfg, MemoryContext mcxt);
extern void sift_analyzer_reset(SiftAnalyzer *a);

/* Full token stream with positions and byte offsets (phrase, highlight, tokenize) */
extern int	sift_analyze_tokens(SiftAnalyzer *a, const char *text, int len, SiftToken **out);

/*
 * Bag of distinct terms for indexing: *out sorted by (hash, bytes), *dl =
 * token count saturated at SIFT_MAX_DL.
 */
extern int	sift_analyze_doc(SiftAnalyzer *a, const char *text, int len,
							 SiftTermFreq **out, uint32 *dl);

/* Folds without tokenizing or stemming (wildcard and prefix literals) */
extern int	sift_analyze_fold(SiftAnalyzer *a, const char *s, int len, char **out);

/* Stable 32-bit term hash, part of the on-disk format (mutable records, blooms) */
extern uint32 sift_term_hash(const char *term, int len);

#endif							/* SIFT_ANALYZE_H */
