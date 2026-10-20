/*
 * sift_sql.h - SQL-callable entry points. PG_FUNCTION_INFO_V1 for all of
 * them lives only in sift_handler.c; implementations (sift_funcs.c and the
 * owning modules) define the functions; sift_stubs.c has weak stubs.
 */
#ifndef SIFT_SQL_H
#define SIFT_SQL_H

#include "fmgr.h"

/* sift_analyze_sql.c: requires an index OID, with no default selection */
struct SiftAnalyzerConfig;
extern void sift_analyzer_config_lookup_index(Oid indexoid, struct SiftAnalyzerConfig *cfg);

/* sift_handler.c */
extern PGDLLEXPORT Datum sift_handler(PG_FUNCTION_ARGS);

/* sift_funcs.c */
extern PGDLLEXPORT Datum sift_match(PG_FUNCTION_ARGS);			/* (text, text) bool */
extern PGDLLEXPORT Datum sift_match_idx(PG_FUNCTION_ARGS);		/* (text, text, oid) bool */
extern PGDLLEXPORT Datum sift_match_support(PG_FUNCTION_ARGS);	/* (internal) internal */
extern PGDLLEXPORT Datum sift_matchsel(PG_FUNCTION_ARGS);		/* restrict estimator */
extern PGDLLEXPORT Datum sift_score(PG_FUNCTION_ARGS);			/* (tid) real, scan-projected */
extern PGDLLEXPORT Datum sift_max_score(PG_FUNCTION_ARGS);		/* (tid) real, scan-projected */
extern PGDLLEXPORT Datum sift_highlight(PG_FUNCTION_ARGS);		/* (text, text, text, text, regclass) text */
extern PGDLLEXPORT Datum sift_tokenize(PG_FUNCTION_ARGS);		/* SRF */
extern PGDLLEXPORT Datum sift_parse_query(PG_FUNCTION_ARGS);	/* (text, regclass) text */
extern PGDLLEXPORT Datum sift_field_query(PG_FUNCTION_ARGS);	/* (text, text, regclass) text */

/* sift_maint.c (sift_segments, sift_segment_postings, sift_verify: sift_segment_sql.c) */
extern PGDLLEXPORT Datum sift_promote(PG_FUNCTION_ARGS);		/* (regclass) int */
extern PGDLLEXPORT Datum sift_merge(PG_FUNCTION_ARGS);			/* (regclass) int */
extern PGDLLEXPORT Datum sift_segments(PG_FUNCTION_ARGS);		/* SRF */
extern PGDLLEXPORT Datum sift_segment_postings(PG_FUNCTION_ARGS);	/* SRF */
extern PGDLLEXPORT Datum sift_verify(PG_FUNCTION_ARGS);			/* (regclass, bool) void */

/* sift_funcs.c, sift_segment_sql.c */
extern PGDLLEXPORT Datum sift_version(PG_FUNCTION_ARGS);		/* () text */
extern PGDLLEXPORT Datum sift_index_info(PG_FUNCTION_ARGS);		/* (regclass) record */

/* sift_facet.c */
extern PGDLLEXPORT Datum sift_facet_counts(PG_FUNCTION_ARGS);	/* SRF */

#endif							/* SIFT_SQL_H */
