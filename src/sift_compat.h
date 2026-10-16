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
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/tuplestore.h"

#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif

/* Table AM scan flags (19): the plain heap scan options */
#if PG_VERSION_NUM >= 190000
#define sift_table_beginscan_parallel(rel, pscan)	table_beginscan_parallel(rel, pscan, SO_NONE)
#define sift_table_index_fetch_begin(rel)			table_index_fetch_begin(rel, SO_NONE)
#else
#define sift_table_beginscan_parallel(rel, pscan)	table_beginscan_parallel(rel, pscan)
#define sift_table_index_fetch_begin(rel)			table_index_fetch_begin(rel)
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

/* IndexAmRoutine fields new in 18, all off */
#define SIFT_AM_NEW_FIELDS(am) \
	do { \
		(am)->amcanhash = false; \
		(am)->amconsistentequality = false; \
		(am)->amconsistentordering = false; \
		(am)->amgettreeheight = NULL; \
		(am)->amtranslatestrategy = NULL; \
		(am)->amtranslatecmptype = NULL; \
	} while (0)

#define sift_pathkey_descending(pk)	((pk)->pk_cmptype == COMPARE_GT)

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
#define SIFT_AM_NEW_FIELDS(am)		((void) 0)
#define sift_pathkey_descending(pk)	((pk)->pk_strategy == BTGreaterStrategyNumber)

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
