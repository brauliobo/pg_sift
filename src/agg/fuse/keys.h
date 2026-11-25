/*
 * agg/fuse/keys.h - what arms of different scan keys need to read the index in one pass (v6_fuse.h): each arm says which values of the lead key column its quals allow and which quals
 * it has besides; the pass reads with the other quals as they are (the arms have equal ones) and with the union of the lead key values, and each arm then takes of the rows of the pass those
 * its own quals on the lead key allow (its gate, a SiftClause over the columns the quals read).
 */
#ifndef SIFT_AGG_V6_FUSE_KEYS_H
#define SIFT_AGG_V6_FUSE_KEYS_H

#include "postgres.h"

#include "utils/rel.h"

#include "agg/engine.h"
#include "agg/fuse/fuse.h"

struct SiftClause;

/* Fills the key set and the quals of the arm from the quals of the spec, and the check of arms that may fuse with different keys */
extern void sift_v6_fuse_describe(SiftV6FuseArm *arm, Relation index, const SiftV6Spec *spec);

/* The quals of the pass of a cohort of different keys: the quals every arm has, and the lead key in the union of the values when they are few; common: the first of them */
extern List *sift_v6_fuse_union_quals(const SiftV6FuseCohort *c, Relation index, List **common);

/* The spec of an arm for the pass (the spec with the columns its gate reads added) and its gate: the quals of the arm that are not common; false when the arm has none */
extern bool sift_v6_fuse_gate(MemoryContext cx, const SiftV6FuseArm *arm, const List *common, const SiftV6Spec *spec, SiftV6Spec **pass_spec, struct SiftClause **gate);

#endif							/* SIFT_AGG_V6_FUSE_KEYS_H */
