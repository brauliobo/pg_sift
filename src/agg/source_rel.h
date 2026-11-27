/*
 * agg/source_rel.h - the relations an operator of SiftAggV6 may scan for a base relation of the query: the relation itself when a store format owns it as a sift_table, and each sift_col index
 * a format owns on it (a sift_table has no sift_col index, so a base relation has one kind of source). The planner asks what the sources are; the executor opens one by its oid and kind.
 */
#ifndef SIFT_AGG_V6_SOURCE_REL_H
#define SIFT_AGG_V6_SOURCE_REL_H

#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "nodes/pathnodes.h"
#include "storage/lockdefs.h"
#include "utils/rel.h"

#include "agg/plan/int.h"
#include "agg/scan_src.h"
#include "agg/vexpr.h"

typedef struct SiftV6RelSource
{
	Oid			reloid;			/* what the scan reads: the sift_table, or the sift_col index */
	Oid			heapoid;		/* the table of the base relation */
	bool		is_table;
	Index		scan_relid;		/* the range table index of the base relation */
	SiftV6Input input;			/* its columns, as the batches of the scan give them */
	IndexOptInfo *ix;			/* the index; NULL for a sift_table */
} SiftV6RelSource;

/* The range table entry is a relation that stores its rows: a table or a materialized view */
static inline bool
sift_v6_rte_stored(const RangeTblEntry *rte)
{
	return rte->rtekind == RTE_RELATION && (rte->relkind == RELKIND_RELATION || rte->relkind == RELKIND_MATVIEW);
}

/* The room of the sources of a relation: the table and every index of it */
#define SIFT_V6_REL_SOURCE_CAP(rel)	(1 + list_length((rel)->indexlist))

/* The base relation is a sift_table a store format scans, or has an index of one: an operator has something to scan */
extern bool sift_v6_rel_has_source(PlannerInfo *root, RelOptInfo *rel);

/* The plan of an operator over the sources of a relation: the relation is a plain table read as a base relation, and has one */
extern bool sift_v6_base_rel_ok(PlannerInfo *root, RelOptInfo *rel);

/* An operator may scan the source: a table holds every row of itself, an index when it has no predicate the query does not imply */
extern bool sift_v6_source_usable(const SiftV6RelSource *s);

/* The price of reading the columns a scan of the source needs: of an index sift_v6_scan_price, of a sift_table the decode of those columns over the granules its quals leave (sift_v6_scan_cost) */
extern void sift_v6_source_scan_price(PlannerInfo *root, RelOptInfo *rel, const SiftV6RelSource *s, AttrNumber key, Cost *open, Cost *total);

/* The price of a scan of the source that decodes only the key and the columns of the quals, and fetches the columns of the rows it keeps by their tags (the fetches are the caller's) */
extern void sift_v6_source_filter_price(PlannerInfo *root, RelOptInfo *rel, const SiftV6RelSource *s, AttrNumber key, Cost *open, Cost *total);

/* The price of the compiled statement over the sift_table relid as its aggregate scan runs it: the columns of spec decoded over the granules the quals of rel leave, into groups */
extern void sift_v6_table_spec_cost(PlannerInfo *root, RelOptInfo *rel, Oid relid, const SiftV6Spec *spec, double groups, Cost *startup, Cost *total);

/* The sources of the relation, the table first, then its indexes; out has room for max_out (SIFT_V6_REL_SOURCE_CAP); the number of them */
extern int	sift_v6_rel_sources(PlannerInfo *root, RelOptInfo *rel, SiftV6RelSource *out, int max_out);
extern SiftV6RelSource *sift_v6_rel_sources_alloc(PlannerInfo *root, RelOptInfo *rel, int *n);

/* The relation of a source: the table or the index, with its input made when in_out is given; closed by sift_v6_source_close with the kind it was opened as */
extern Relation sift_v6_source_open(Oid reloid, bool is_table, LOCKMODE lockmode, SiftV6Input *in_out);
extern void sift_v6_source_close(Relation rel, bool is_table, LOCKMODE lockmode);

/* Binds the plan to a source of its relation, opened as rel: its oid, index and the format that owns it; false when no format scans it, or the index is not one of Format 2.0 (the format of a test may own others) */
extern bool sift_v6_plan_bind(V6Plan *p, const SiftV6RelSource *s, Relation rel);

#endif							/* SIFT_AGG_V6_SOURCE_REL_H */
