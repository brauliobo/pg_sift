/*
 * agg/v6/v6_explain.h - the EXPLAIN lines of SiftAggV6 that name what the scan reads (v6_explain.c)
 */
#ifndef SIFT_AGG_V6_EXPLAIN_H
#define SIFT_AGG_V6_EXPLAIN_H

#include "postgres.h"

#include "commands/explain.h"
#include "utils/relcache.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_ext.h"

/* Filters (the quals of the scan: the index's and the batches') and Search (the text query of a sift index) of the scan of heap, read through index (NULL for a sift_table) */
extern void sift_v6_explain_reads(const SiftV6Spec *spec, Relation heap, Relation index, ExplainState *es);
/* Group Keys: the columns the groups are of, when the statement names them as they are */
extern void sift_v6_explain_group_keys(const SiftV6Spec *spec, ExplainState *es);
/* Lookup Joins: the relations the plans of the dimensions of a star join read */
extern void sift_v6_explain_lookup_joins(CustomScanState *css, const SiftV6Ext *ext, ExplainState *es);

/* Top-N: the sort keys and the limit of the groups (the spec of the aggregate scan, when it gives the best groups in order) or of the rows (the Top-N scan, once the tournament has a limit) */
extern void sift_v6_explain_topn_groups(const SiftV6Spec *spec, ExplainState *es);
struct SiftV6TopNSpec;
extern void sift_v6_explain_topn_rows(const struct SiftV6TopNSpec *spec, ExplainState *es);

#endif							/* SIFT_AGG_V6_EXPLAIN_H */
