/*
 * table/v1/v1_tbl_ops.c - the sift_table half of the Format 1 store: a table is read through the same column source as an index (agg/colsrc.h), so what differs is only which
 * relations are the format's and which relation the rows belong to.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "utils/rel.h"

#include "core/meta_peek.h"
#include "core/sift.h"
#include "table/dispatch.h"
#include "table/v1/v1_tbl_ops.h"

bool
sift_v1_tbl_owns(Oid relid)
{
	Relation	rel = table_open(relid, NoLock);
	bool		owns = sift_tbl_is_table(rel) && sift_meta_resolve_major(rel) == SIFT_FORMAT_MAJOR;

	table_close(rel, NoLock);
	return owns;
}

SiftV1Rel
sift_v1_rel_open(Oid relid, bool is_table)
{
	SiftV1Rel	r;

	r.rel = is_table ? table_open(relid, AccessShareLock) : index_open(relid, AccessShareLock);
	r.heap = is_table ? r.rel : table_open(r.rel->rd_index->indrelid, AccessShareLock);
	return r;
}

void
sift_v1_rel_close(const SiftV1Rel *r)
{
	if (r->heap != r->rel)
		table_close(r->heap, NoLock);
	relation_close(r->rel, NoLock);
}
