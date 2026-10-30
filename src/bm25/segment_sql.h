/*
 * bm25/segment_sql.h - the three helpers of the SQL-callable functions on segments (segment_sql.c, segment_test.c).
 */
#ifndef SIFT_BM25_SEGMENT_SQL_H
#define SIFT_BM25_SEGMENT_SQL_H

#include "postgres.h"
#include "core/ops.h"
#include "bm25/segment.h"

static inline Relation
open_sift(Oid relid)
{
	Relation	index = index_open(relid, AccessShareLock);

	if (index->rd_rel->relam != sift_catalog()->am)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift index", RelationGetRelationName(index))));
	sift_check_maintain(index);
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
	}
	return "unknown";
}

#endif							/* SIFT_BM25_SEGMENT_SQL_H */
