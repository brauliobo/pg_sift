/*
 * bm25/query_int.h - shared between the SiftQL files (query.c, query_analyze.c, query_match.c).
 */
#ifndef SIFT_QUERY_INT_H
#define SIFT_QUERY_INT_H

#include "bm25/query.h"

#define QERROR(code, loc, ...) \
	ereport(ERROR, (errcode(code), \
					errmsg("sift query: %s at position %d", psprintf(__VA_ARGS__), (loc))))
#define SYNTAX(loc, ...)	QERROR(ERRCODE_SYNTAX_ERROR, loc, __VA_ARGS__)
#define LIMIT(loc, ...)		QERROR(ERRCODE_PROGRAM_LIMIT_EXCEEDED, loc, __VA_ARGS__)

static inline int
char_len(const char *s, const char *end)
{
	return (unsigned char) *s < 0x80 ? 1 : pg_mblen_range(s, end);
}

/* bm25/query.c */
extern SiftQNode *sift_query_new_node(SiftQKind kind, int loc);
extern SiftQNode *sift_query_new_not(SiftQNode *child, int loc);
extern List *sift_query_add_child(List *kids, SiftQNode *c, SiftQKind kind);
extern int	sift_query_canon_pattern(StringInfo pat, bool *trailing);

#endif							/* SIFT_QUERY_INT_H */
