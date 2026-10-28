/*
 * core/sql.h - SQL-callable entry points. PG_FUNCTION_INFO_V1 for all of
 * them lives only in core/handler.c; implementations (bm25/funcs.c and the
 * owning modules) define the functions; sift_stubs.c has weak stubs.
 */
#ifndef SIFT_SQL_H
#define SIFT_SQL_H

#include "fmgr.h"
#include "nodes/parsenodes.h"

/* bm25/analyze_sql.c: requires an index OID, with no default selection */
struct SiftAnalyzerConfig;
extern void sift_analyzer_config_lookup_index(Oid indexoid, struct SiftAnalyzerConfig *cfg);

/* core/handler.c */
extern PGDLLEXPORT Datum sift_handler(PG_FUNCTION_ARGS);
/* The result of a SQL function that returns a row type: the values and nulls in the columns its declaration gives */
extern Datum sift_result_row(FunctionCallInfo fcinfo, Datum *values, bool *nulls);

/* bm25/funcs.c */
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

/* bm25/maint.c (sift_segments, sift_segment_postings, sift_verify: bm25/segment_sql.c) */
extern PGDLLEXPORT Datum sift_promote(PG_FUNCTION_ARGS);		/* (regclass) int */
extern PGDLLEXPORT Datum sift_merge(PG_FUNCTION_ARGS);			/* (regclass) int */
extern PGDLLEXPORT Datum sift_segments(PG_FUNCTION_ARGS);		/* SRF */
extern PGDLLEXPORT Datum sift_segment_postings(PG_FUNCTION_ARGS);	/* SRF */
extern PGDLLEXPORT Datum sift_verify(PG_FUNCTION_ARGS);			/* (regclass, bool) void */

/* bm25/funcs.c, bm25/segment_sql.c */
extern PGDLLEXPORT Datum sift_version(PG_FUNCTION_ARGS);		/* () text */
extern PGDLLEXPORT Datum sift_index_info(PG_FUNCTION_ARGS);		/* (regclass) record */
extern PGDLLEXPORT Datum sift_index_format(PG_FUNCTION_ARGS);	/* (regclass) record */
extern PGDLLEXPORT Datum sift_worker_state(PG_FUNCTION_ARGS);	/* (regclass) record */

/* core/jsonbuild.c */
extern PGDLLEXPORT Datum sift_json_build_object(PG_FUNCTION_ARGS);	/* (VARIADIC "any") json */
extern void sift_json_build_object_swap(Query *q);	/* the calls of json_build_object in a query tree call sift.json_build_object */

/* bm25/facet.c */
extern PGDLLEXPORT Datum sift_facet_counts(PG_FUNCTION_ARGS);	/* SRF */

#endif							/* SIFT_SQL_H */
