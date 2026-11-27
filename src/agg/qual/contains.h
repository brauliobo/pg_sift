/*
 * agg/qual/contains.h - jsonb containment and equality of a document of an index as tests of the leaves of its paths, which the scan runs as batch quals (agg/qual/contains.c).
 */
#ifndef SIFT_AGG_QUAL_CONTAINS_H
#define SIFT_AGG_QUAL_CONTAINS_H

#include "postgres.h"

#include "agg/engine.h"

/*
 * The conjuncts that a WHERE clause doc @> '{..}' (constant <@ doc), NOT (doc @> '{..}') or doc -> 'k' .. = 'scalar' (doc['k'] = ..) is the conjunction of: tests of the virtual attributes of the
 * leaves of the paths it names (Vars of rel->relid) and doc IS NOT NULL. NIL: the clause is none of those, or a leaf does not decide one of its members exactly.
 */
extern List *sift_v6_contains_quals(const SiftV6Rel *rel, const Expr *clause);

#endif							/* SIFT_AGG_QUAL_CONTAINS_H */
