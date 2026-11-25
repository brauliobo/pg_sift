/*
 * col/f2/v6/scope/match.h - what the pairs side of a statement has to be for the summary of a scope of two paths to answer it.
 */
#ifndef SIFT_COL_F2_V6_SCOPE_MATCH_H
#define SIFT_COL_F2_V6_SCOPE_MATCH_H

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"

#include "col/scope/scope.h"

/* The key column's own types and equality: a constant or a parameter of the same kind as the column, the btree equality of the column's type under a deterministic collation, the expression under any relabel */
extern bool sift_v6_key_arg_ok(Oid key_type, Oid typid);
extern bool sift_v6_key_equality(Oid key_type, Oid opno, Oid inputcollid);
extern Node *sift_v6_strip_relabel(Node *n);

/* A sub-select or CTE with some form that sets the pairs of scope key K: the first path with its predicate, on its own */
extern Query *sift_v6_match_two_paths(const Query *q, const SiftScopeLeg legs[2]);

/* The only join of the statement is the semi join of IN or EXISTS with the relation of relid on its right */
extern bool sift_v6_semi_joins(const PlannerInfo *root, Index pairs_relid);

#endif							/* SIFT_COL_F2_V6_SCOPE_MATCH_H */
