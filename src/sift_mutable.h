/*
 * sift_mutable.h - mutable segments: lane appends, sealing, reading and
 * deletion (DESIGN.md (c)). Records follow SiftMutDocData in sift_page.h.
 */
#ifndef SIFT_MUTABLE_H
#define SIFT_MUTABLE_H

#include "sift_host.h"

#include "sift_analyze.h"
#include "sift_store.h"

/*
 * Appends one doc to the active mutable segment (creating it if none) on
 * lane MyProcNumber % SIFT_LANES, falling back to other lanes when the tail
 * is busy. Returns true when the active segment reached mutable_limit
 * (caller asks maint to seal).
 */
extern bool sift_mut_append(Relation index, ItemPointer tid, const SiftTermFreq *terms,
							int nterms, uint32 dl);

/*
 * Seals the active segment (only above mutable_limit unless force) and opens
 * a fresh one; returns the sealed gen or 0.
 */
extern uint64 sift_mut_seal(Relation index, bool force);

/* Re-sets the sealed flag on every lane tail of a sealed segment (idempotent) */
extern void sift_mut_flag_sealed(Relation index, const SiftSegEntry *entry);

/* Doc view into reader memory, valid until the next sift_mut_next */
typedef struct SiftMutDoc
{
	ItemPointerData tid;
	uint16		dl;
	uint32		nterms;			/* may exceed 64k distinct terms */
	bool		deleted;
	const uint32 *hash;
	const uint8 *tf;
	const uint8 *len;
	const char *terms;			/* concatenated, sum(len) bytes */
} SiftMutDoc;

typedef struct SiftMutReader SiftMutReader;

/* false: the page's docs cannot match (its bloom rules them out) */
typedef bool (*SiftMutPageFilter) (void *arg, const uint64 *bloom);

extern bool sift_mut_bloom_has(const uint64 *bloom, uint32 hash);

extern SiftMutReader *sift_mut_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt,
									SiftMutPageFilter filter, void *filter_arg);
extern bool sift_mut_next(SiftMutReader *r, SiftMutDoc *doc);

/* Docs started and their dl sum over every page seen, deleted included; complete after the scan */
extern void sift_mut_totals(const SiftMutReader *r, uint64 *ndocs, uint64 *sum_dl);
extern void sift_mut_close(SiftMutReader *r);

/*
 * VACUUM: calls cb for every live doc and flags the ones it reports dead;
 * returns the docs removed. Caller holds the maintenance lock.
 */
extern int64 sift_mut_bulkdelete(Relation index, const SiftSegEntry *entry,
								 IndexBulkDeleteCallback cb, void *cbstate,
								 IndexBulkDeleteResult *stats);

/* Extents owned by a mutable segment (header and data pages), for release after promotion */
extern SiftExtent *sift_mut_pages(Relation index, const SiftSegEntry *entry, int *n);

/* Docs (deleted ones included), tokens and pages of a mutable segment; scans every page */
extern void sift_mut_summary(Relation index, const SiftSegEntry *entry, uint64 *ndocs, uint64 *sum_dl,
							 uint64 *npages);

/* Structural check of every page and record; ERRCODE_INDEX_CORRUPTED. Returns live docs */
extern uint64 sift_mut_verify(Relation index, const SiftSegEntry *entry);

#endif							/* SIFT_MUTABLE_H */
