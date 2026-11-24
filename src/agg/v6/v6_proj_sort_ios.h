/*
 * agg/v6/v6_proj_sort_ios.h - SiftProjSortScan without the relation: when every attribute a statement reads (its output and its quals) is an integer-class column (bool, int2, int4, int8, date,
 * timestamp, timestamptz) that the projection keeps, the row of an address whose page the visibility map of the relation says is all visible is built from the cells of the projection, and
 * the relation is read only for the addresses of the other pages (the contract of an index-only scan).
 */
#ifndef SIFT_AGG_V6_PROJ_SORT_IOS_H
#define SIFT_AGG_V6_PROJ_SORT_IOS_H

#include "postgres.h"

#include "access/attnum.h"
#include "executor/tuptable.h"
#include "storage/buf.h"
#include "utils/relcache.h"

#include "agg/v6/v6_proj.h"

#define SORT_IOS_MAX	8

/* The attributes of the relation the statement reads, and the leaf of the store that holds each */
typedef struct SortIosCols
{
	int			n;
	AttrNumber	att[SORT_IOS_MAX];
	int16		leaf[SORT_IOS_MAX];
} SortIosCols;

/* Whether the cells of a column of this type are integers that make its datum */
extern bool sift_v6_sort_ios_type(Oid type);

/* The tuple of the cells of the row in slot, the attributes the statement does not read NULL; false when a cell holds a value its column could not keep, and the relation must say */
extern bool sift_v6_sort_ios_fill(TupleTableSlot *slot, const SortIosCols *cols, const SiftCell *row);

/* Whether the page of the address is all visible in rel; vmbuf is the page of the map in hand between calls (InvalidBuffer at first) */
extern bool sift_v6_sort_ios_visible(Relation rel, uint64 addr, Buffer *vmbuf);

#endif							/* SIFT_AGG_V6_PROJ_SORT_IOS_H */
