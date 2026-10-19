/*
 * sift_query.h - SiftQL parsing, analysis and document evaluation
 * (DESIGN.md (h)).
 *
 * Lifecycle: sift_query_parse() builds a raw tree (syntax only),
 * sift_query_analyze() runs leaves through the index analyzer (multi-token
 * words become phrases, empty leaves vanish, constants fold). All nodes are
 * allocated in CurrentMemoryContext. Syntax errors raise
 * ERRCODE_SYNTAX_ERROR with a 1-based character position; limits raise
 * ERRCODE_PROGRAM_LIMIT_EXCEEDED.
 *
 * Analyzed (normalized) trees: boosts live on positive leaves only (the
 * product of the boosts above them, 1 under NOT); AND/OR are flattened,
 * have >= 2 children, sorted canonically (positives first, NOTs last);
 * duplicate leaves merge by adding boosts, duplicate NOTs merge; x AND NOT x
 * folds to SQ_NOTHING. SQ_NOT appears only under an SQ_AND that has a
 * positive child; SQ_NOTHING appears only as the root. Leaves are numbered
 * 0..nleaves-1 in deparse order (leafno).
 *
 * A word, prefix, pattern or fuzzy term may be qualified, name:word*, and so may a group,
 * name:(a b) or name:{a b}; analysis moves the qualifier to the leaves (the innermost wins) and
 * resolves it against the index's fields; a name the index lacks is an error (ERRCODE_SYNTAX_ERROR
 * listing the fields). Without fields a colon is text, as before fields existed: name:word is the
 * word "name:word", and name:(a b) the words "name:a" and "name:b".
 *
 * Leaf text after analysis: SQ_TERM holds an index term; SQ_PREFIX and
 * SQ_FUZZY hold folded, unstemmed literals; SQ_WILDCARD holds a folded
 * pattern where '*' is any run, '?' one character and '\c' the literal c;
 * SQ_REGEX holds the pattern as written and re, the compiled anchored regex.
 *
 * Relaxed (index) semantics: a positive phrase is the AND of its terms, a
 * phrase under NOT restricts nothing; candidates then need the recheck.
 */
#ifndef SIFT_QUERY_H
#define SIFT_QUERY_H

#include "sift.h"
#include "sift_analyze.h"

#define SIFT_QUERY_MAX_BYTES	(256 * 1024)
#define SIFT_MAX_BOOST			10000.0
#define SIFT_MAX_REGEX_BYTES	1000	/* as Elasticsearch's max_regex_length */

typedef enum SiftQKind
{
	SQ_TERM,					/* analyzed term */
	SQ_PREFIX,					/* folded literal prefix: foo* */
	SQ_WILDCARD,				/* folded pattern with * and ? */
	SQ_REGEX,					/* POSIX ARE, whole-term match */
	SQ_FUZZY,					/* term~1 / term~2 */
	SQ_PHRASE,					/* analyzed terms, adjacency rechecked */
	SQ_AND,
	SQ_OR,
	SQ_NOT,						/* only as a child of SQ_AND */
	SQ_NOTHING					/* matches no document */
} SiftQKind;

typedef struct SiftQNode
{
	SiftQKind	kind;
	float4		boost;			/* 1.0 unless ^n given */
	int			loc;			/* 1-based char position in the source */
	int			fuzzy;			/* SQ_FUZZY: 1 or 2 */
	char	   *text;			/* leaf text (NUL-terminated, length textlen) */
	int			textlen;
	int			nterms;			/* SQ_PHRASE: analyzed terms */
	char	  **terms;
	int		   *termlens;
	int			nchild;			/* SQ_AND / SQ_OR / SQ_NOT */
	struct SiftQNode **child;
	char	   *fieldname;		/* name: qualifier of a leaf or group, else NULL */
	uint8		field;			/* analyzed leaves: the index field it searches (1..), 0 = all text */
	bool		gram;			/* analyzed SQ_PREFIX of at most the index's prefix_index characters: the term SEP text */
	int			leafno;			/* analyzed leaves: canonical number, else -1 */
	void	   *re;				/* analyzed SQ_REGEX: compiled regex_t */
} SiftQNode;

typedef struct SiftQuery
{
	SiftQNode  *root;
	int			nleaves;
	bool		analyzed;
	bool		needs_recheck;	/* contains a phrase */
} SiftQuery;

extern SiftQuery *sift_query_parse(const char *src, int len);
extern SiftQuery *sift_query_analyze(const SiftQuery *raw, SiftAnalyzer *a);

/* Canonical form: deterministic, re-parseable, used by tests and EXPLAIN */
extern char *sift_query_deparse(const SiftQuery *q);

/*
 * Does an analyzed expansion leaf (SQ_PREFIX, SQ_WILDCARD, SQ_REGEX,
 * SQ_FUZZY) or SQ_TERM match one analyzed term? Shared by document
 * evaluation and dictionary expansion.
 */
extern bool sift_query_leaf_match(const SiftQNode *leaf, const char *term, int len);

/*
 * Evaluate an analyzed query against raw document text without an index
 * (operator function, bitmap recheck, phrase verification). Expansion leaves
 * are matched against the document's own tokens.
 */
extern bool sift_query_match_doc(const SiftQuery *q, SiftAnalyzer *a,
								 const char *text, int len);

/*
 * Recheck of a candidate that satisfies the relaxed query: evaluates exactly
 * only the parts the index relaxed (subtrees holding phrases).
 */
extern bool sift_query_recheck_doc(const SiftQuery *q, SiftAnalyzer *a,
								   const char *text, int len);

/*
 * A document analyzed once for recheck and exact scoring: its tokens, grouped
 * by distinct term on first use. Allocated in CurrentMemoryContext; the terms stay valid
 * until the analyzer runs again.
 */
typedef struct SiftDoc SiftDoc;

extern SiftDoc *sift_doc_open(SiftAnalyzer *a, const char *text, int len);
extern int	sift_doc_ntokens(const SiftDoc *d);
extern int	sift_doc_nterms(SiftDoc *d);
extern const char *sift_doc_term(SiftDoc *d, int i, int *len);
extern int	sift_doc_tf(SiftDoc *d, int i);	/* occurrences of distinct term i */
extern int	sift_doc_find(SiftDoc *d, const char *term, int len);	/* -1 when absent */
extern int	sift_doc_term_tf(SiftDoc *d, const char *term, int len);	/* occurrences of an index term, a prefix term (SEP prefix) included; 0 when absent */
extern int	sift_doc_leaf_tf(SiftDoc *d, const SiftQNode *leaf);	/* occurrences of every word an expansion leaf matches, in its field */
extern bool sift_doc_match(SiftDoc *d, const SiftQNode *n);	/* exact evaluation of one node */
extern bool sift_doc_recheck(const SiftQuery *q, SiftDoc *d);

#endif							/* SIFT_QUERY_H */
