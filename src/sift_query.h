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
 */
#ifndef SIFT_QUERY_H
#define SIFT_QUERY_H

#include "sift.h"
#include "sift_analyze.h"

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
 * Evaluate an analyzed query against raw document text without an index
 * (operator function, bitmap recheck, phrase verification). Expansion leaves
 * are matched against the document's own tokens.
 */
extern bool sift_query_match_doc(const SiftQuery *q, SiftAnalyzer *a,
								 const char *text, int len);

/* Phrase-only recheck of a candidate already known to contain all terms */
extern bool sift_query_recheck_doc(const SiftQuery *q, SiftAnalyzer *a,
								   const char *text, int len);

#endif							/* SIFT_QUERY_H */
