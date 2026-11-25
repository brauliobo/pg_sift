/*
 * col/f2/scan/par_int.h - what the serial scan of a format-2 index (f2_scan.c) shares with its parallel one (f2_par.c): the units of a scan, and the part a scan has open between its runs of granules.
 */
#ifndef SIFT_COL_F2_SCAN_PAR_INT_H
#define SIFT_COL_F2_SCAN_PAR_INT_H

#include "col/f2/scan/int.h"
#include "col/f2/cache.h"
#include "col/f2/gbuf/gbuf.h"
#include "col/f2/del/pend.h"
#include "col/f2/point_dir.h"

/* The part a scan has open: its pin, and what the scan knows of its predicates; a run of granules of the part reads through it, and so does the next run of the same part */
typedef struct PartCx
{
	SiftF2PartPin *pin;
	PartPreds	pp;
	SiftPointCands pc;
} PartCx;

/* A unit of a scan: a part of a partition, or (d NULL) the L0 slab of it; px: what the snapshot sees deleted by a pending delete of the partition, or NULL */
typedef struct F2Unit
{
	const SiftPartitionEntry *e;
	const SiftPartDesc *d;
	const SiftPendDelIndex *px;
} F2Unit;

typedef void (*F2UnitFn) (F2Scan *s, Relation rel, const F2Unit *u, void *arg);

/* False when the predicates rule out every row of the part; else the scan is set to read its granules */
extern bool part_open(Relation rel, F2Scan *s, const SiftColSchema *schema, const SiftPartitionEntry *e, SiftF2PartPin *pin, Snapshot snap, PartCx *cx);
/* The granules [lo, hi) of the open part */
extern void scan_span(Relation rel, F2Scan *s, const SiftColSchema *schema, SiftF2PartPin *pin, const PartPreds *pp, uint32 lo, uint32 hi, const SiftPendDelIndex *px, Snapshot snap);
extern void scan_slab(Relation rel, F2Scan *s, const SiftF2Rel *f, const SiftColSchema *schema, const SiftPartitionEntry *e, const SiftPendDelIndex *px, Snapshot snap);
extern void compile_preds(F2Scan *s, const SiftColSchema *schema);
/* The units of the plan the key ranges, the time window and the parts to skip leave; want_px: each carries the pending deletes of its partition and staged those of the transaction */
extern void walk_units(Relation rel, F2Scan *s, const SiftPlanSnapshot *plan, Snapshot snap, const uint64 *staged, int nstaged, bool want_px, F2UnitFn fn, void *arg);

#endif
