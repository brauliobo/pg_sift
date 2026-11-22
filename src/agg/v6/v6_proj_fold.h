/*
 * agg/v6/v6_proj_fold.h - the cube of a statement made of three kinds of rows, in any order: the stored rows of the cells of a SIFT_PROJ_AGG part (rolled up into the groups of the query), the
 * rows a deletion vector has since killed in a base part the projection stands for (taken away), and the rows of everything the projection does not stand for (added). The predicates of the
 * statement are tested on the cells of the first two; the third comes from a scan that decided them. Nothing here reads a page: the host feeds the rows.
 */
#ifndef SIFT_AGG_V6_PROJ_FOLD_H
#define SIFT_AGG_V6_PROJ_FOLD_H

#include "core/host.h"

#include "agg/v6/v6_proj_distinct.h"
#include "agg/v6/v6_proj_pred.h"
#include "col/f2/f2_proj.h"

typedef struct SiftProjFold
{
	SiftProjCube *cube;
	const SiftProjSpec *query;	/* the groups and the measures of the statement: the spec of the cube */
	const SiftProjSpec *src;	/* the projection the stored rows are of */
	const SiftProjLayoutCol *lay;
	int			nlay;
	const SiftProjRollup *map;
	const SiftProjPred *preds;	/* of the statement, tested on the rows of the relation */
	int			npreds;
	SiftProjPred *stored_preds;	/* the same as a stored row is tested for them */
	SiftProjConst *stored_vals;
	int			nstored_preds;
	SiftProjDistinct *dist[SIFT_PROJ_MAX_COLS];	/* of each count(DISTINCT) measure of the statement */
	int			ncols;
	SiftCell   *row;
	bool		ok;				/* false: the answer cannot be exact from these rows, and the statement is read from the base */
	int64		nstored;
	int64		ndead;
	int64		nlive;
} SiftProjFold;

extern SiftProjFold *sift_proj_fold_new(const SiftProjSpec *query, const SiftProjSpec *src, const SiftProjLayoutCol *lay, int nlay, const SiftProjRollup *map, const SiftProjPred *preds,
										int npreds, int ncols, const SiftProjColumns *cols, int64 max_pairs);
extern void sift_proj_fold_free(SiftProjFold *f);

/* A stored row of a projection part, in the layout of the part */
extern void sift_proj_fold_stored(SiftProjFold *f, const SiftCell *stored);

/* A row of the relation (one cell per column) that has died since the projection part was written (a deletion vector, a pending delete, a heap row): taken away if the projection counted it */
extern void sift_proj_fold_dead(SiftProjFold *f, const SiftCell *row);

/* A row of the relation the scan found visible, the predicates decided (by the scan, or by the host for the cells it reads) */
extern void sift_proj_fold_live(SiftProjFold *f, const SiftCell *row);

/* The count(DISTINCT) of every group into the cube; false when the cube cannot be exact (an exception cell, the pairs of a count(DISTINCT) past their bound): read the base */
extern bool sift_proj_fold_finish(SiftProjFold *f);

#endif							/* SIFT_AGG_V6_PROJ_FOLD_H */
