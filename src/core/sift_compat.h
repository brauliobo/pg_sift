/*
 * sift_compat.h - the PostgreSQL 17..19 differences, in one place.
 *
 * PostgreSQL 18 is the reference. Older servers get a stand-in for what they
 * lack; newer ones get an adapter for what changed. Included by sift.h.
 */
#ifndef SIFT_COMPAT_H
#define SIFT_COMPAT_H

#include "access/genam.h"
#include "access/tableam.h"
#include "nodes/miscnodes.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/tuplestore.h"

#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif

/* Table AM scan flags (19): the plain heap scan options */
#if PG_VERSION_NUM >= 190000
#define sift_table_beginscan(rel, snapshot, nkeys, key)	table_beginscan(rel, snapshot, nkeys, key, SO_NONE)
#define sift_table_beginscan_parallel(rel, pscan)	table_beginscan_parallel(rel, pscan, SO_NONE)
#define sift_table_index_fetch_begin(rel)			table_index_fetch_begin(rel, SO_NONE)
#else
#define sift_table_beginscan(rel, snapshot, nkeys, key)	table_beginscan(rel, snapshot, nkeys, key)
#define sift_table_beginscan_parallel(rel, pscan)	table_beginscan_parallel(rel, pscan)
#define sift_table_index_fetch_begin(rel)			table_index_fetch_begin(rel)
#endif

/* index_beginscan takes the scan's instrumentation (18) and its flags (19) */
#if PG_VERSION_NUM >= 190000
#define sift_index_beginscan(heap, index, snapshot, nkeys)	index_beginscan(heap, index, snapshot, NULL, nkeys, 0, SO_NONE)
#elif PG_VERSION_NUM >= 180000
#define sift_index_beginscan(heap, index, snapshot, nkeys)	index_beginscan(heap, index, snapshot, NULL, nkeys, 0)
#else
#define sift_index_beginscan(heap, index, snapshot, nkeys)	index_beginscan(heap, index, snapshot, nkeys, 0)
#endif

/*
 * The table AM callbacks of 19: the insert options are unsigned, a delete and an update get them after the command id (a delete that
 * moves the row to another partition is TABLE_DELETE_CHANGING_PARTITION in them, not an argument), the index fetch gets the scan
 * flags, a CLUSTER copy gets the snapshot of its scan, an ANALYZE tuple no horizon and a VACUUM constant parameters. The callbacks
 * keep the 17/18 shape; SIFT_TAM_*_ADAPTER defines the 19 one in front of them and SIFT_TAM_* names what goes in the routine.
 */
#if PG_VERSION_NUM >= 190000
typedef uint32 SiftTamOptions;

#define SIFT_TAM_FLAGS_PARAM			, uint32 flags
#define SIFT_TAM_CLUSTER_SNAPSHOT		Snapshot cluster_snapshot,
#define SIFT_TAM_ANALYZE_XMIN			/* none */
#define SIFT_TAM_VACUUM_PARAMS			const struct VacuumParams *params

#define SIFT_TAM_DELETE_ADAPTER(fn) \
	static TM_Result fn##_tam(Relation rel, ItemPointer tid, CommandId cid, uint32 options, Snapshot snapshot, Snapshot crosscheck, \
							  bool wait, TM_FailureData *tmfd) \
	{ return fn(rel, tid, cid, snapshot, crosscheck, wait, tmfd, (options & TABLE_DELETE_CHANGING_PARTITION) != 0); }
#define SIFT_TAM_UPDATE_ADAPTER(fn) \
	static TM_Result fn##_tam(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, uint32 options, Snapshot snapshot, \
							  Snapshot crosscheck, bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode, \
							  TU_UpdateIndexes *update_indexes) \
	{ return fn(rel, otid, slot, cid, snapshot, crosscheck, wait, tmfd, lockmode, update_indexes); }
#define SIFT_TAM(fn)	fn##_tam
#else
typedef int SiftTamOptions;

#define SIFT_TAM_FLAGS_PARAM			/* none */
#define SIFT_TAM_CLUSTER_SNAPSHOT		/* none */
#define SIFT_TAM_ANALYZE_XMIN			TransactionId OldestXmin,
#define SIFT_TAM_VACUUM_PARAMS			struct VacuumParams *params

#define SIFT_TAM_DELETE_ADAPTER(fn)
#define SIFT_TAM_UPDATE_ADAPTER(fn)
#define SIFT_TAM(fn)	fn
#endif

/* A tuple descriptor built by hand is finalized before its first use (19) */
#if PG_VERSION_NUM >= 190000
#define sift_tupdesc_finalize(desc)	TupleDescFinalize(desc)
#else
#define sift_tupdesc_finalize(desc)	((void) (desc))
#endif

/* The planner hook and standard_planner take the EXPLAIN state too (19) */
#if PG_VERSION_NUM >= 190000
#define SIFT_PLANNER_PARAMS		Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams, ExplainState *es
#define SIFT_PLANNER_ARGS(q)	(q), query_string, cursorOptions, boundParams, es
#else
#define SIFT_PLANNER_PARAMS		Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams
#define SIFT_PLANNER_ARGS(q)	(q), query_string, cursorOptions, boundParams
#endif

/* subquery_planner names its plan and takes an alternative root (19); the name is the sub-select's alias, as PostgreSQL's own call of it makes it */
#if PG_VERSION_NUM >= 190000
#define sift_subquery_planner(glob, query, parent, name) \
	subquery_planner((glob), (query), choose_plan_name((glob), (name), false), (parent), NULL, false, 0.0, NULL)
#else
#define sift_subquery_planner(glob, query, parent, name)	subquery_planner((glob), (query), (parent), false, 0.0, NULL)
#endif

/* GlobalVisTestIsRemovableXid may update its bounds (19) */
#if PG_VERSION_NUM >= 190000
#define sift_vis_removable_xid(state, xid)	GlobalVisTestIsRemovableXid((state), (xid), true)
#else
#define sift_vis_removable_xid(state, xid)	GlobalVisTestIsRemovableXid((state), (xid))
#endif

/* The int8 value of a numeric and whether it did not fit (19 reports it through an error-saving context) */
#if PG_VERSION_NUM >= 190000
static inline int64
sift_numeric_int8(Numeric num, bool *err)
{
	ErrorSaveContext esc = {T_ErrorSaveContext};
	int64		v = numeric_int8_safe(num, (Node *) &esc);

	*err = esc.error_occurred;
	return v;
}
#else
#define sift_numeric_int8(num, err)	numeric_int8_opt_error((num), (err))
#endif

/* pg_stat's report of a VACUUM: the relation (19) or its oid and whether it is shared, and the dead rows only from 18 */
#if PG_VERSION_NUM >= 190000
#define sift_pgstat_report_vacuum(rel, live, dead, start)	pgstat_report_vacuum((rel), (live), (dead), (start))
#elif PG_VERSION_NUM >= 180000
#define sift_pgstat_report_vacuum(rel, live, dead, start) \
	pgstat_report_vacuum(RelationGetRelid(rel), (rel)->rd_rel->relisshared, (live), (dead), (start))
#else
#define sift_pgstat_report_vacuum(rel, live, dead, start) \
	pgstat_report_vacuum(RelationGetRelid(rel), (rel)->rd_rel->relisshared, (live), (dead))
#endif

/* Plain and index-only scans off while create_index_paths builds the bitmap paths: 19 reads them from the rel, not the GUCs */
#if PG_VERSION_NUM >= 190000

static inline uint64
sift_plain_index_scans_off(RelOptInfo *rel)
{
	uint64		saved = rel->pgs_mask;

	rel->pgs_mask &= ~(PGS_INDEXSCAN | PGS_INDEXONLYSCAN);
	return saved;
}

static inline void
sift_plain_index_scans_restore(RelOptInfo *rel, uint64 saved)
{
	rel->pgs_mask = saved;
}

#else

static inline uint64
sift_plain_index_scans_off(RelOptInfo *rel)
{
	int			nest = NewGUCNestLevel();

	set_config_option("enable_indexscan", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("enable_indexonlyscan", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	return nest;
}

static inline void
sift_plain_index_scans_restore(RelOptInfo *rel, uint64 saved)
{
	AtEOXact_GUC(true, (int) saved);
}

#endif

#if PG_VERSION_NUM >= 180000

/* EXPLAIN "Index Searches" of a bitmap index scan */
#define sift_count_search(scan)	do { if ((scan)->instrument) (scan)->instrument->nsearches++; } while (0)

#define sift_pathkey_descending(pk)	((pk)->pk_cmptype == COMPARE_GT)
/* The operator that orders values of type typ as the pathkey does (make_sort_from_pathkeys) */
#define sift_pathkey_sortop(pk, typ)	get_opfamily_member_for_cmptype((pk)->pk_opfamily, (typ), (typ), (pk)->pk_cmptype)

/* One page of a TIDBitmap: block, lossy, and the offsets of an exact page */
typedef TBMPrivateIterator SiftTbmIter;

#define sift_tbm_begin	tbm_begin_private_iterate
#define sift_tbm_end	tbm_end_private_iterate

static inline bool
sift_tbm_next(SiftTbmIter *it, BlockNumber *blockno, bool *lossy, OffsetNumber *offs, int *n)
{
	TBMIterateResult res;

	if (!tbm_private_iterate(it, &res))
		return false;
	*blockno = res.blockno;
	*lossy = res.lossy;
	*n = res.lossy ? 0 : tbm_extract_page_tuple(&res, offs, MaxHeapTuplesPerPage);
	return true;
}

#else							/* PostgreSQL 17 */

#define PG_MODULE_MAGIC_EXT(...)	PG_MODULE_MAGIC

#define sift_count_search(scan)		((void) 0)
#define sift_pathkey_descending(pk)	((pk)->pk_strategy == BTGreaterStrategyNumber)
#define sift_pathkey_sortop(pk, typ)	get_opfamily_member((pk)->pk_opfamily, (typ), (typ), (pk)->pk_strategy)

typedef TBMIterator SiftTbmIter;

#define sift_tbm_begin	tbm_begin_iterate
#define sift_tbm_end	tbm_end_iterate

static inline bool
sift_tbm_next(SiftTbmIter *it, BlockNumber *blockno, bool *lossy, OffsetNumber *offs, int *n)
{
	TBMIterateResult *res = tbm_iterate(it);

	if (res == NULL)
		return false;
	*blockno = res->blockno;
	*lossy = res->ntuples < 0;
	*n = Max(res->ntuples, 0);
	memcpy(offs, res->offsets, *n * sizeof(OffsetNumber));
	return true;
}

/* The equivalence members of a class that may be about relids (18 keeps child members apart) */
typedef struct EquivalenceMemberIterator
{
	List	   *members;
	int			next;
	Relids		relids;
} EquivalenceMemberIterator;

static inline void
setup_eclass_member_iterator(EquivalenceMemberIterator *it, EquivalenceClass *ec, Relids relids)
{
	*it = (EquivalenceMemberIterator) {ec->ec_members, 0, relids};
}

static inline EquivalenceMember *
eclass_member_iterator_next(EquivalenceMemberIterator *it)
{
	while (it->next < list_length(it->members))
	{
		EquivalenceMember *em = list_nth_node(EquivalenceMember, it->members, it->next++);

		if (!em->em_is_child || bms_is_subset(em->em_relids, it->relids))
			return em;
	}
	return NULL;
}

#endif

#endif							/* SIFT_COMPAT_H */
