/*
 * agg/star/run.h - the dimensions of a star join read from the plans of the node (custom_plans) into the tables of agg/star/star.h, and the passes a dimension too big for its share of
 * work_mem is read in: its keys fall into as many classes as it takes for one class to fit, and each pass joins the fact rows of one class (every fact row is in one pass). A class of an inner join
 * that is still more than work_mem (the rows of one key) is read in chunks that each fit, and the fact rows of the class are joined to each chunk in a pass of its own. A class of any other join that is still more than work_mem is read
 * whole, as the batch of a hash join that no more batches make smaller is: a fact row that matches nothing must come out once, which no chunk of the rows alone knows.
 */
#ifndef SIFT_AGG_V6_STAR_RUN_H
#define SIFT_AGG_V6_STAR_RUN_H

#include "postgres.h"

#include "executor/executor.h"

#include "agg/ext.h"
#include "agg/join/found.h"
#include "agg/pipe.h"

typedef struct SiftV6StarRun SiftV6StarRun;

typedef enum SiftStarOpen
{
	SSO_DONE,					/* no pass is left */
	SSO_EMPTY,					/* the pass joins no fact row: an inner join of a dimension with no row of it */
	SSO_SCAN					/* the dimensions of the pass are read: scan the fact table */
} SiftStarOpen;

extern SiftV6StarRun *sift_v6_star_begin(const SiftV6Ext *x, PlanState *const *children, MemoryContext cx);
/* The scan of the fact table the dimensions are joined to: the found flags of its index may answer a dimension (agg/join/found.h) from the first pass on */
extern void sift_v6_star_fact(SiftV6StarRun *r, const SiftV6Spec *spec, const SiftV6FoundFact *fact);
/* The column of the fact scan that holds the found flag dimension d is answered by; false when it is read from its plan */
extern bool sift_v6_star_found_col(const SiftV6StarRun *r, int d, SiftV6Col *col);
extern const SiftV6FoundStats *sift_v6_star_found_stats(const SiftV6StarRun *r);
/* Reads the dimensions of the pass, which the node joins in scans fact scans (one for each partition of a partitioned fact); the first call (pass 0) chooses the classes of the ones that do not fit */
extern SiftStarOpen sift_v6_star_open(SiftV6StarRun *r, uint32 pass, uint32 scans);
/* A scan of the pass has read its fact rows: the last one sends the rows of the dimensions of a RIGHT or FULL join that no scan matched through the pipe of that scan */
extern void sift_v6_star_scan_end(SiftV6StarRun *r, SiftV6Pipe *p);
/*
 * The keys of a scan of the pass of the fact index: keys and, for each inner or semi dimension keyed by a column of the index, the range of the keys of the pass on it and, when the pass holds
 * at most SIFT_STAR_IN_MAX of them, the keys themselves as an IN list (no fact row outside them finds a match), which the zones, the marks and the point directory of the parts prune by.
 * None when a join keeps the fact rows that match nothing (left, full). Palloc'd; *n of them.
 */
extern ScanKey sift_v6_star_scan_keys(const SiftV6StarRun *r, Relation index, ScanKey keys, int nkeys, int *n);
/* The dimensions of the pass for a pipe */
extern void sift_v6_star_attach(const SiftV6StarRun *r, SiftV6Pipe *p);
/* After pass 0: every dimension fits in one pass (no class of keys or chunk is left for another) */
extern bool sift_v6_star_one_pass(const SiftV6StarRun *r);
/* What the planner expects of dimension d of rows rows of width bytes: the times the node reads its plan, and in passes the passes of the fact table its parts or chunks make */
extern double sift_v6_star_reads(const SiftV6Ext *x, int d, double rows, double width, double *passes);
/* The passes the dimensions need (known after pass 0) and the bytes the largest table took */
extern uint32 sift_v6_star_passes(const SiftV6StarRun *r);
extern uint64 sift_v6_star_bytes(const SiftV6StarRun *r);
/* The dimensions read from the cache of the tables of earlier statements, and those read from their plans to be kept (false: neither, no dimension is of a table that keeps a journal) */
extern bool sift_v6_star_cache(const SiftV6StarRun *r, uint32 *hits, uint32 *built);
/* The tables of its dimensions that another node of the statement had read */
extern uint32 sift_v6_star_shared(const SiftV6StarRun *r);
/* The tables are freed; the next open reads the plans again */
extern void sift_v6_star_close(SiftV6StarRun *r);

#endif							/* SIFT_AGG_V6_STAR_RUN_H */
