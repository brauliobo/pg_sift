/*
 * agg/v6/v6_star_run.h - the dimensions of a star join read from the plans of the node (custom_plans) into the tables of agg/v6/v6_star.h, and the passes a dimension too big for its share of
 * work_mem is read in: its keys fall into as many classes as it takes for one class to fit, and each pass joins the fact rows of one class (every fact row is in one pass).
 */
#ifndef SIFT_AGG_V6_STAR_RUN_H
#define SIFT_AGG_V6_STAR_RUN_H

#include "postgres.h"

#include "executor/executor.h"

#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_pipe.h"

typedef struct SiftV6StarRun SiftV6StarRun;

typedef enum SiftStarOpen
{
	SSO_DONE,					/* no pass is left */
	SSO_EMPTY,					/* the pass joins no fact row: an inner join of a dimension with no row of it */
	SSO_SCAN					/* the dimensions of the pass are read: scan the fact table */
} SiftStarOpen;

extern SiftV6StarRun *sift_v6_star_begin(const SiftV6Ext *x, PlanState *const *children, MemoryContext cx);
/* Reads the dimensions of the pass; the first call (pass 0) chooses the classes of the ones that do not fit */
extern SiftStarOpen sift_v6_star_open(SiftV6StarRun *r, uint32 pass);
/* The dimensions of the pass for a pipe */
extern void sift_v6_star_attach(const SiftV6StarRun *r, SiftV6Pipe *p);
/* The passes the dimensions need (known after pass 0) and the bytes the largest table took */
extern uint32 sift_v6_star_passes(const SiftV6StarRun *r);
extern uint64 sift_v6_star_bytes(const SiftV6StarRun *r);
/* The dimensions read from the cache of the tables of earlier statements, and those read from their plans to be kept (false: neither, no dimension is of a table that keeps a journal) */
extern bool sift_v6_star_cache(const SiftV6StarRun *r, uint32 *hits, uint32 *built);
/* The tables are freed; the next open reads the plans again */
extern void sift_v6_star_close(SiftV6StarRun *r);

#endif							/* SIFT_AGG_V6_STAR_RUN_H */
