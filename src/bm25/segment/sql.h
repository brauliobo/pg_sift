/*
 * bm25/segment/sql.h - the three helpers of the SQL-callable functions on segments (segment_sql.c, segment_test.c).
 */
#ifndef SIFT_BM25_SEGMENT_SQL_H
#define SIFT_BM25_SEGMENT_SQL_H

#include "postgres.h"
#include "core/catname.h"
#include "core/inspect.h"
#include "core/ops.h"
#include "bm25/segment/segment.h"

/* col_use: the function of a sift_col index that does what the caller does for a sift index (NULL: none) */
static inline Relation
open_sift(Oid relid, const char *col_use)
{
	Relation	index;

	sift_inspect_expect(sift_rel_must_exist(relid), "sift", col_use);
	if (!sift_may_inspect_rel(relid))
		sift_require_acl(relid, ACL_SELECT | ACL_MAINTAIN);
	index = index_open(relid, AccessShareLock);
	sift_store_require_ready(index);
	sift_check_inspect(index);
	return index;
}

static inline SiftScoreRef
meta_ref(const SiftDirSnapshot *snap)
{
	return (SiftScoreRef) {snap->meta.k1, snap->meta.b, 0};
}

static inline const char *
kind_name(uint8 kind)
{
	switch (kind)
	{
		case SIFT_SEG_IMMUTABLE:
			return "immutable";
		case SIFT_SEG_WBUF_OPEN:
			return "buffer";
		case SIFT_SEG_WBUF_SEALED:
			return "sealed buffer";
		case SIFT_SEG_TOMB:
			return "tombstones";
		case SIFT_SEG_VALRUN:
			return "value run";
	}
	return "unknown";
}

#endif							/* SIFT_BM25_SEGMENT_SQL_H */
