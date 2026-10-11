/*
 * sift_mutable.h - mutable segments: lane appends, sealing, reading and
 * deletion. Records follow SiftMutDocData in sift_page.h.
 */
#ifndef SIFT_MUTABLE_H
#define SIFT_MUTABLE_H

#include "access/genam.h"

#include "sift_analyze.h"
#include "sift_store.h"

/*
 * Appends one doc to the active mutable segment (creating it if none) on
 * lane MyProcNumber % SIFT_LANES, falling back to other lanes when the tail
 * is busy. One generic WAL record. Returns true when the active segment
 * crossed mutable_limit (caller asks maint to seal).
 */
extern bool sift_mut_append(Relation index, ItemPointer tid, const SiftTermFreq *terms,
							int nterms, uint32 dl);

/* Seals the active segment and opens a fresh one; returns the sealed gen or 0 */
extern uint64 sift_mut_seal(Relation index);

/* Doc view into reader memory, valid until the next sift_mut_next */
typedef struct SiftMutDoc
{
	ItemPointerData tid;
	uint16		dl;
	uint16		nterms;
	bool		deleted;
	const uint32 *hash;
	const uint8 *tf;
	const uint8 *len;
	const char *terms;
} SiftMutDoc;

typedef struct SiftMutReader SiftMutReader;

extern SiftMutReader *sift_mut_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt);

/*
 * Restricts sift_mut_next to docs containing any (or all) of the hashes;
 * pages whose bloom rules them out are skipped. Hash hits still need a byte
 * comparison by the caller.
 */
extern void sift_mut_filter(SiftMutReader *r, const uint32 *hashes, int n, bool require_all);
extern bool sift_mut_next(SiftMutReader *r, SiftMutDoc *doc);

/* Totals over every lane (also for deleted docs, like immutable stats) */
extern void sift_mut_totals(SiftMutReader *r, uint64 *ndocs, uint64 *sum_dl);
extern void sift_mut_close(SiftMutReader *r);

/* VACUUM: flags deleted docs; returns docs removed */
extern int64 sift_mut_bulkdelete(Relation index, const SiftSegEntry *entry,
								 IndexBulkDeleteCallback cb, void *cbstate,
								 IndexBulkDeleteResult *stats);

/* Extents/pages owned by a mutable segment, for release after promotion */
extern SiftExtent *sift_mut_pages(Relation index, const SiftSegEntry *entry, int *n);

#endif							/* SIFT_MUTABLE_H */
