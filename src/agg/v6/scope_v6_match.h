/*
 * agg/v6/scope_v6_match.h - what the pairs side of a statement has to be for the summary of a scope of two paths to answer it.
 */
#ifndef SIFT_AGG_V6_SCOPE_MATCH_H
#define SIFT_AGG_V6_SCOPE_MATCH_H

#include "postgres.h"

#include "nodes/parsenodes.h"

#include "col/scope.h"

/* A sub-select or CTE with some form that sets the pairs of scope key K: the first path with its predicate, on its own */
extern Query *sift_v6_match_two_paths(const Query *q, const SiftScopeLeg legs[2]);

#endif							/* SIFT_AGG_V6_SCOPE_MATCH_H */
