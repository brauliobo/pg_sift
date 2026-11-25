/*
 * table/v1/tbl_ops.h - the sift_table half of the Format 1 store (col/v1/store_ops.c): which tables it reads, and the relations a source of it holds open.
 */
#ifndef SIFT_TABLE_V1_TBL_OPS_H
#define SIFT_TABLE_V1_TBL_OPS_H

#include "postgres.h"

#include "storage/lockdefs.h"
#include "utils/relcache.h"

/* The table is a sift_table of Format 1 */
extern bool sift_v1_tbl_owns(Oid relid);

/* The relation of a source (the index, or the table) and the heap its rows belong to (the table itself for a sift_table); AccessShareLock held until sift_v1_rel_close */
typedef struct SiftV1Rel
{
	Relation	rel;
	Relation	heap;
} SiftV1Rel;

extern SiftV1Rel sift_v1_rel_open(Oid relid, bool is_table);
extern void sift_v1_rel_close(const SiftV1Rel *r);

#endif							/* SIFT_TABLE_V1_TBL_OPS_H */
