/*
 * sift_analyze.h - document and query analysis (tokenize, fold, stem, hash).
 *
 * Tokenization rules (deterministic, Unicode 16.0 tables frozen in
 * sift_analyze_tables.h, independent of the PostgreSQL Unicode version):
 * - A word token is a maximal run of letters (L*), numbers (N*) and combining
 *   marks (M*). Everything else separates: punctuation, symbols, spaces,
 *   apostrophes, hyphens, underscores, periods ("don't" -> don, t; "e-mail" ->
 *   e, mail; "3.14" -> 3, 14).
 * - Han ideographs and Hiragana are one token per character; other scripts
 *   without spaces (Thai, Khmer, ...) form runs like any word.
 * - Each emoji (Extended_Pictographic from U+2190 up) is one token; a pair of
 *   regional indicators (a flag) is one token. Following combining marks and
 *   default ignorables (VS16, skin tones, ZWJ) attach to the emoji and are
 *   dropped, so a ZWJ sequence yields one token per pictograph.
 * - Default ignorables (soft hyphen, ZWJ, ZWNJ, variation selectors, bidi
 *   controls) are dropped and do not break words. U+200B ZERO WIDTH SPACE
 *   separates.
 * - Invalid UTF-8 bytes separate (PostgreSQL rules: no overlongs, surrogates
 *   or code points above U+10FFFF). In SQL_ASCII databases bytes >= 0x80 are
 *   opaque letters. Other server encodings are rejected.
 *
 * Folding is per code point. case_fold: full Unicode case folding
 * (CaseFolding.txt C+F, "Straße" -> "strasse"). accent_fold: NFKC
 * compatibility folding (full-width, ligatures, superscripts) plus removal of
 * Latin, Greek, Cyrillic, Hebrew and Arabic diacritics, and stroke letters
 * (ø, ł, đ, æ, œ, þ, ı) mapped to their base. Cyrillic breve is kept (й, ў);
 * Indic vowel signs and kana voicing marks are kept. Compatibility mappings
 * producing non-letters (½) are not applied. A folding is only applied if it
 * keeps the token's shape (letters fold to letters and marks, marks to marks
 * or nothing, an ideograph to one ideograph and marks); otherwise the
 * character is kept as written (ŉ, ゟ). With both off ("preserve") text is
 * kept as written. Inputs are assumed NFC: decomposed sequences fold per code
 * point.
 *
 * Folded tokens are truncated to SIFT_MAX_TERM_BYTES at a character boundary,
 * then stemmed to a fixpoint. Tokens that fold to nothing are dropped and take
 * no position.
 *
 * Invariant, for every configuration: each emitted token t analyzes to
 * exactly [t], so typing a document term finds it and canonical query forms
 * are fixpoints (checked exhaustively by tools/analyze `harness invariant`).
 *
 * Index and query sides must use the same SiftAnalyzerConfig; its hash
 * (which includes SIFT_ANALYZER_VERSION) is stored in the metapage.
 */
#ifndef SIFT_ANALYZE_H
#define SIFT_ANALYZE_H

#include "sift.h"

/* Bump on any change to tokenization, folding, stemming or hashing output */
#define SIFT_ANALYZER_VERSION	2

typedef enum SiftStemmer
{
	SIFT_STEM_NONE = 0,
	SIFT_STEM_EN,				/* Snowball English (Porter2) */
	SIFT_STEM_COUNT
} SiftStemmer;

typedef struct SiftAnalyzerConfig
{
	bool		case_fold;
	bool		accent_fold;
	uint8		stemmer;		/* SiftStemmer */
	bool		pairs;			/* the bag also holds every adjacent token pair, SEP a SEP b */
	uint8		prefix_len;		/* the bag also holds the first 1..prefix_len characters of every word, SEP prefix */
	uint8		nfields;		/* the bag also holds the words of the first nfields sections apart (SIFT_FIELD_BYTE) */
	char		field_names[SIFT_FIELDS_BYTES];	/* their names, comma separated; not part of the hash */
	bool		keyword;		/* the whole text, folded, is the only token (none when it folds to nothing) */
	uint32		hash;			/* of the version and the fields above (pairs, prefix_len, nfields, keyword only when set) */
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

/* Cursor of sift_analyze_next(); fields are private */
typedef struct SiftTokenStream
{
	const char *text;
	int			len;
	int			off;
	uint32		pos;
} SiftTokenStream;

typedef struct SiftAnalyzer SiftAnalyzer;

/* Sets all fields including the hash */
extern void sift_analyzer_config_set(SiftAnalyzerConfig *cfg, bool case_fold, bool accent_fold,
									 SiftStemmer stemmer);

/* Turns the pair terms and the prefix terms on or off (set clears them) and refreshes the hash */
extern void sift_analyzer_config_set_synthetic(SiftAnalyzerConfig *cfg, bool pairs, int prefix_len, const char *fields);

/*
 * Keyword analyzer (facets): a text is one token, folded like a word (case_fold / accent_fold; with
 * both off it is kept byte for byte) and never stemmed. Empty text, or text that folds to nothing,
 * has no token; one longer than SIFT_MAX_TERM_BYTES raises ERRCODE_PROGRAM_LIMIT_EXCEEDED, because
 * truncating would merge distinct values. Refreshes the hash.
 */
extern void sift_analyzer_config_set_keyword(SiftAnalyzerConfig *cfg, bool keyword);

/* Longest indexed word prefix of the analyzer's index, 0 when none */
extern int	sift_analyzer_prefix_len(const SiftAnalyzer *a);

/* The field called name (len bytes, case insensitive): 1.., 0 when the index has none of that name; the number of fields */
extern int	sift_analyzer_field(const SiftAnalyzer *a, const char *name, int len);
extern int	sift_analyzer_nfields(const SiftAnalyzer *a);
extern const char *sift_analyzer_field_names(const SiftAnalyzer *a);

/* Do the analyzers answer every query alike? The pair and prefix terms only speed phrases and short prefixes up */
extern bool sift_analyzer_config_agrees(const SiftAnalyzerConfig *a, const SiftAnalyzerConfig *b);

/* Reloption validator: lowercase identifiers separated by commas, at most SIFT_MAX_FIELDS */
extern void sift_analyze_validate_fields(const char *names);

/* Default config: folding on, no stemming */
extern void sift_analyzer_config_default(SiftAnalyzerConfig *cfg);
extern void sift_analyzer_config_from_options(const SiftOptions *opts, SiftAnalyzerConfig *cfg);
extern void sift_analyzer_config_from_index(Relation index, SiftAnalyzerConfig *cfg);

/*
 * Stemmer by name: "" or "none", "en" / "english" / "english_stem". Raises
 * ERRCODE_INVALID_PARAMETER_VALUE for unknown names.
 */
extern SiftStemmer sift_analyze_stemmer(const char *name);

/* Reloption validator form of sift_analyze_stemmer */
extern void sift_analyze_validate_stemmer(const char *name);

/*
 * The analyzer owns reusable buffers allocated in mcxt. Results of the
 * functions below stay valid until the next call on the same analyzer or
 * sift_analyzer_reset(). Raises ERRCODE_FEATURE_NOT_SUPPORTED unless the
 * database encoding is UTF8 or SQL_ASCII.
 */
extern SiftAnalyzer *sift_analyzer_create(const SiftAnalyzerConfig *cfg, MemoryContext mcxt);

/* Invalidates results and frees buffers grown past a working-set size */
extern void sift_analyzer_reset(SiftAnalyzer *a);

/* Full token stream with positions and byte offsets (phrase, highlight, tokenize) */
extern int	sift_analyze_tokens(SiftAnalyzer *a, const char *text, int len, SiftToken **out);

/*
 * Streaming form of sift_analyze_tokens with identical results; tok->str is
 * valid until the next sift_analyze_next() on the same analyzer.
 */
extern void sift_analyze_begin(SiftTokenStream *ts, const char *text, int len);
extern bool sift_analyze_next(SiftAnalyzer *a, SiftTokenStream *ts, SiftToken *tok);

/*
 * Bag of distinct terms for indexing: *out sorted by (hash, bytes), *dl =
 * token count saturated at SIFT_MAX_DL.
 */
extern int	sift_analyze_doc(SiftAnalyzer *a, const char *text, int len,
							 SiftTermFreq **out, uint32 *dl);

/*
 * Folds without tokenizing or stemming (wildcard and prefix literals): token
 * characters fold as in tokens, ignorables are dropped, everything else is
 * copied. *out is NUL-terminated.
 */
extern int	sift_analyze_fold(SiftAnalyzer *a, const char *s, int len, char **out);

/*
 * Stable term hashes, part of the on-disk format (mutable records, blooms).
 * sift_term_hash64: an xxh64-style single-lane hash. h = K3 ^ len * K1; each
 * 8-byte little-endian word w, then the zero-padded tail if any:
 * h = rotl64(h + w * K2, 31) * K1; result murmur3 fmix64(h), with
 * K1 = 0x9E3779B185EBCA87, K2 = 0xC2B2AE3D27D4EB4F, K3 = 0x27D4EB2F165667C5.
 * sift_term_hash is its high 32 bits.
 */
extern uint64 sift_term_hash64(const char *term, int len);
extern uint32 sift_term_hash(const char *term, int len);

/*
 * Stemmers rewrite w[0..len) in place and return the new length, never longer
 * than len. They only stem tokens made of lowercase ASCII letters, digits and
 * apostrophes; others are returned unchanged.
 */
extern int	sift_analyze_stem_en(char *w, int len);

#endif							/* SIFT_ANALYZE_H */
