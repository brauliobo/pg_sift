/*
 * table/f2/scan_prune.h - the granules a plain scan of a sift_table leaves out by the zones of its quals (table/f2/scan_prune.c).
 */
#ifndef SIFT_TABLE_F2_SCAN_PRUNE_H
#define SIFT_TABLE_F2_SCAN_PRUNE_H

#include "table/f2/col_delta/col_delta.h"
#include "table/f2/tbl.h"

typedef struct SiftTblF2Prune SiftTblF2Prune;

/* The scan leaves out the granules whose zones say no row passes the predicates */
extern void sift_tbl_f2_scan_set_prune(TableScanDesc scan, const SiftTblF2Prune *prune);
/* Granule g of the part has no row that passes the predicates; gm: what the column deltas of the snapshot patch in it */
extern bool sift_tbl_f2_prune_granule(const SiftTblF2Prune *p, Relation rel, const SiftF2Store *store, SiftF2PartPin *pin, uint32 g, const SiftColDeltaGranMap *gm);
/* The executor hook that begins the scans of a plan with their predicates */
extern void sift_tbl_f2_prune_init(void);

#endif							/* SIFT_TABLE_F2_SCAN_PRUNE_H */
