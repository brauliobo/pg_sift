/*
 * bm25/facet_rls.h - the matches of sift.facets() that the row-level security policies of the caller let it see.
 *
 * The policies are PostgreSQL's own: the visible version of each match is asked of the table the way a SELECT of the caller asks it
 * (SELECT ctid FROM ONLY t WHERE ctid = ANY ($1)), so a policy with a sub-select, a restrictive policy, FORCE ROW LEVEL SECURITY and a security
 * barrier read exactly as they do in any other statement, and a row the policies hide counts in no facet value.
 */
#ifndef SIFT_BM25_FACET_RLS_H
#define SIFT_BM25_FACET_RLS_H

#include "core/host.h"

#include "core/visibility.h"

/* A page of matches that the policies let through: its offsets (bit i is offset i + 1) */
typedef void (*SiftFacetRlsFn) (void *arg, BlockNumber blk, const uint64 *offmask);

typedef struct SiftFacetRls SiftFacetRls;

/* NULL when the caller's reads of heap are subject to no policy */
extern SiftFacetRls *sift_facet_rls_begin(Relation heap, Snapshot snapshot, MemoryContext mcxt, SiftFacetRlsFn out, void *arg);

/* The matches of one page, in the order of the pages; out gets them once the batch they are in is decided */
extern void sift_facet_rls_page(SiftFacetRls *r, BlockNumber blk, const uint64 *offmask);

/* Decides the last batch; the matches that passed; frees r */
extern int64 sift_facet_rls_finish(SiftFacetRls *r);

#endif							/* SIFT_BM25_FACET_RLS_H */
