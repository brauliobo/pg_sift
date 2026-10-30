/*
 * agg/colsrc.h - the column source SiftAgg reads (planner metadata and executor read API).
 *
 * This is the narrow interface of the aggregate track (col-plan) to the column storage. The storage is either
 *   - a `sift_col` index over a heap table: the values are copies taken at aminsert/ambuild, a row may be used only
 *     if its heap page is all-visible (visibility map) or the heap says its tuple is visible;
 *     the executor asks for that vetting (sift_colsrc_vet) after the value filters, or
 *   - a `sift_table` TABLE (table access method): the table is the columns, its scan is MVCC by itself (sift_colsrc_vet has
 *     nothing to check).
 * Everything above this header is the same for both. The implementations are providers (SiftSrcProvider) registered by their
 * module: the sift_col index, the sift_table table, and the test
 * stub (src/agg/stub*.c, the `sift_colstub` index AM, SIFT_TESTING builds only), which reads the heap pages raw and builds
 * the same units. The functions of agg/colsrc/plan.h dispatch on the access method of the relation. Keep this header small: it is the seam.
 *
 * Model: a source has leaves (ids 0..nleaves-1). A scalar leaf is one value per row; a GROUP
 * leaf is an array (or object) of elements: per row it has an element count (NULL: absent), and its child leaves hold
 * one value per element, all elements of the unit concatenated in row order (child element j of every child of a group
 * is the same array element). Rows of a unit are numbered 0..nrow-1 in TID order (the row ordinal); a child element's
 * parent ordinal is recovered from the counts by sift_colsrc_parents().
 *
 * A unit is the rows of one segment inside one superblock (SIFT_SB_PAGES heap pages); an SB can be held by several
 * units (disjoint rows). Dictionaries of text leaves are per unit source (per segment): codes are NOT comparable
 * between units whose SiftSrcDict::id differs; the executor translates by value.
 */
#ifndef SIFT_COLSRC_H
#define SIFT_COLSRC_H

#include "agg/colsrc/meta.h"
#include "agg/colsrc/exec.h"
#include "agg/colsrc/provider.h"
#include "agg/colsrc/plan.h"

#endif							/* SIFT_COLSRC_H */
