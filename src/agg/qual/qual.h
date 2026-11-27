/*
 * agg/qual/qual.h - the quals of the aggregate scan that no operator class of sift_col has: <> on any column and NOT LIKE '%literal%' on a text column. The scan keys of the other quals
 * come from the operator class (ExecIndexBuildScanKeys); these keys carry the strategies COL_STRAT_NE and COL_STRAT_NOT_LIKE of col/am/preds.h, which scan_preds turns into the
 * predicates SIFT_COLOP_NE and SIFT_COLOP_NOTCONTAINS. A LIKE of the class is taken only when its pattern is exactly '%literal%', so that the leaf decides it and no row needs a second look.
 */
#ifndef SIFT_AGG_V6_QUAL_H
#define SIFT_AGG_V6_QUAL_H

#include "postgres.h"

#include "access/skey.h"
#include "nodes/execnodes.h"
#include "nodes/primnodes.h"
#include "utils/rel.h"

/* COL_STRAT_NE or COL_STRAT_NOT_LIKE when opno is the negator of an equality or of LIKE of the opfamily, else 0 */
extern int	sift_v6_qual_private_strategy(Oid opno, Oid opfamily);

/* Whether the pattern of a LIKE is '%literal%' (no other wildcard, a literal of at least SIFT_LIKE_INFIX_MIN bytes) in an encoding where the leaf's bytes decide it */
extern bool sift_v6_qual_like_exact(const Const *pattern);

/*
 * The scan keys of the quals (clauses of the form index var op constant, over INDEX_VAR attributes of index) of the aggregate scan, as an index scan holds them, then those of the jsonb quals
 * (SiftV6Spec.jsonqual), in one array: the keys that read a parameter are in rtkeys, which point into it
 */
extern void sift_v6_qual_scankeys(PlanState *ps, List *jsonqual, Relation index, List *quals, ScanKey *keys, int *nkeys, IndexRuntimeKeyInfo **rtkeys, int *nrt);

/* The scan keys of the jsonb quals of the index or the sift_table reloid written at keys[*nkeys], which has room for list_length(jsonqual) more (agg/qual/jsonb.c) */
extern void sift_v6_json_append_scankeys(List *jsonqual, Oid reloid, bool is_table, ScanKey keys, int *nkeys);

#endif							/* SIFT_AGG_V6_QUAL_H */
