/*
 * agg/unnest/path.h - jsonb_path_exists(doc, '$[*] ? (<filter>)') (or '$.key[*] ? (<filter>)' of the array doc -> 'key'), doc @? '..' and doc @> '[{"key": value}]' over a jsonb array of objects
 * the index keeps shredded, as a test of the elements of the scan of an unnest (agg/unnest/exists.h): the filter as quals over the element columns, true for an element exactly when PostgreSQL's jsonpath says it is.
 *
 * A member the element lacks, a member that is a JSON null and a member that holds a value of the leaf are the three kinds the scan reads for each element (SIFT_ELEM_KIND_*, agg/vec.h); a comparison of a
 * JSON null is true under != and under none of the others, of a missing key false under all, so that ! is the complement of the three. A value of another JSON type than the literal's is unknown, which the
 * quals could not complement: the literal has the type of the leaf, or the call is not planned. A row whose document the leaves cannot hold (no array, a member of another JSON type than the leaf's, or a
 * row still in the slab) is decided from the heap by the call itself, strict mode and errors included.
 */
#ifndef SIFT_AGG_V6_UNNEST_PATH_H
#define SIFT_AGG_V6_UNNEST_PATH_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/plan/int.h"
#include "agg/unnest/jsonb.h"

/* e is a call this file plans: jsonb_path_exists(doc, path[, vars[, silent]]), doc @? path, doc @> constant or constant <@ doc */
extern bool sift_v6_unnest_path_call(const Node *e);
/* the array the call tests, doc or doc -> 'key'; NULL when e is no such call */
extern const Expr *sift_v6_unnest_path_array(const Node *e);
/* the quals over the element columns of rte (which gains the members they read) that are true of a row whose document the leaves hold exactly when the call is; false: the leaves do not decide it */
extern bool sift_v6_unnest_path_quals(const V6Plan *p, SiftV6ElemRte *rte, const Node *e, List **quals);

#endif							/* SIFT_AGG_V6_UNNEST_PATH_H */
