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

/*
 * e is a call this file plans: jsonb_path_exists(doc, path[, vars[, silent]]), doc @? path, doc @> constant array, constant array <@ doc, doc ? 'key', doc ?| keys or doc ?& keys; on a
 * sift_table (table), which reads the row again for the call to decide, a containment of any pattern
 */
extern bool sift_v6_unnest_path_call(const Node *e, bool table);
/* the kind of stream the elements the call tests are: the objects of a jsonpath or a containment, the strings of a key test or of a containment of strings */
extern SiftV6ElemFn sift_v6_unnest_path_fn(const Node *e);
/* the call is a jsonpath whose array is $.key[*] of its document: a document without the key is false, a NULL one NULL (any other call is NULL where its array is) */
extern bool sift_v6_unnest_path_keyed(const Node *e);
/* the array the call tests, doc or doc -> 'key'; NULL when e is no such call */
extern const Expr *sift_v6_unnest_path_array(const Node *e, bool table);
/*
 * The members of the object pattern of doc @> '{..}' (constant <@ doc) that are lists of strings, each as the call (doc -> 'key') @> '[..]', and in rest the containment of the other members (NULL
 * when none is left): the clause is true exactly when all of them are. NIL: the clause is no such containment, or no member is a list of strings
 */
extern List *sift_v6_unnest_path_members(const Node *clause, Expr **rest);
/* the document of the containment doc @> '{..}' (constant <@ doc); NULL: the clause is none */
extern const Expr *sift_v6_unnest_path_doc(const Node *clause);
/*
 * The parts of the test over the element columns of rte (which gains the members they read), each a list of quals some element passes: the test is true of a row whose document the leaves hold
 * exactly when the call is (every object of a containment's pattern is a part, one element passes the whole filter of a jsonpath); false: the leaves do not decide it
 */
extern bool sift_v6_unnest_path_quals(const V6Plan *p, SiftV6ElemRte *rte, const Node *e, List **parts);

#endif							/* SIFT_AGG_V6_UNNEST_PATH_H */
