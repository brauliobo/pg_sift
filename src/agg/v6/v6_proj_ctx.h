/*
 * agg/v6/v6_proj_ctx.h - what both executors of the projections (v6_proj_exec.h for the aggregates, v6_proj_sort_exec.h for the rows) know before they read a part: the ready projections of one kind
 * that have a definition that still fits, the base parts of the relation, which of them each projection stands for under the snapshot, and the choice among the sources by cost. A projection
 * need not stand for every base part: the parts it does not stand for (a part written since, a part with a deletion vector it cannot take rows away for, a partition with deletes pending) and the
 * slabs are read from the relation as any scan does.
 */
#ifndef SIFT_AGG_V6_PROJ_CTX_H
#define SIFT_AGG_V6_PROJ_CTX_H

#include "agg/v6/v6_proj.h"
#include "agg/v6/v6_proj_host.h"
#include "col/f2/col_meta.h"

typedef struct SiftV6ProjSource
{
	const SiftManifestProjRef *ref;
	const SiftProjDef *def;
	SiftProjParts parts;
	SiftProjPartServe *serve;	/* for each base part */
	int			nserved;
} SiftV6ProjSource;

typedef struct SiftV6ProjCtx
{
	const SiftV6ProjHost *h;
	SiftProjColumns cols;
	const SiftPlanSnapshot *plan;	/* the one version of the manifest every read of the statement is of: the base parts, the projection directory's refs, the rest of the relation */
	SiftColMetaPageData meta;	/* of that version */
	SiftV6BaseParts base;
	SiftV6ProjSource *src;
	int			nsrc;
} SiftV6ProjCtx;

/*
 * The sources of the kind that stand for at least one base part; false when there are none. The stage is flushed first (never on a standby, which has none), then the plan and the
 * metapage are read until they are of one version: from there on a commit of any other backend cannot change what the statement reads, so nothing has to be checked after the scan.
 */
extern bool sift_v6_proj_ctx_open(SiftV6ProjCtx *c, const SiftV6ProjHost *h, SiftProjKind kind);

/* The cheapest source for the query: ch->cand indexes c->src, -1 for the relation */
extern SiftV6ProjChoice sift_v6_proj_ctx_choose(const SiftV6ProjCtx *c, const SiftV6ProjQuery *q);

/* The projection part a link names; the part must be there */
extern const SiftPartDesc *sift_v6_proj_part_of(const SiftV6ProjSource *s, uint64 gen);

/* The generations of the base parts the source stands for, ascending */
extern uint64 *sift_v6_proj_served_gens(const SiftV6ProjCtx *c, const SiftV6ProjSource *s, int *n);

/* The predicate of the statement as the choice sees it */
extern SiftV6ProjPred sift_v6_proj_query_pred(const SiftProjPred *p);

#endif							/* SIFT_AGG_V6_PROJ_CTX_H */
