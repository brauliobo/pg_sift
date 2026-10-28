/*
 * col/dictshm.h - the parsed dictionaries of segments, kept in shared memory for the backends of a server (col/dictshm.c).
 */
#ifndef SIFT_COL_DICTSHM_H
#define SIFT_COL_DICTSHM_H

#include "postgres.h"

#include "storage/relfilelocator.h"

#include "col/enc.h"

/* What names a parsed object of a segment for good (col/dictcache.c): the key bytes are hashed, padding included, so a key is memset first */
typedef struct DictKey
{
	RelFileLocator rel;
	uint64		gen;
	uint64		ndocs;			/* read from the segment: a damaged one must not share the entry of an intact one */
	BlockNumber header_blk;
	int32		col;
	uint32		unit;			/* UINT32_MAX: the dictionary of the leaf, else the list lengths of that unit */
} DictKey;

/* The table is there for this backend: a parallel leader asks before it enters parallel mode, where it cannot be made, and tells its workers, which join it */
extern bool sift_dictshm_prepare(void);
extern void sift_dictshm_join(void);
/*
 * The dictionary another backend kept under k, read where it lies in shared memory (no copy: the arrays of the SiftCDict point into the record, which
 * is not overwritten until the context made under parent is deleted); NULL when there is none or the table is not there.
 */
extern SiftCDict *sift_dictshm_read(const DictKey *k, MemoryContext parent);
/* Keeps d for the other backends when it is big enough to be worth it; false when it is not kept */
extern bool sift_dictshm_write(const DictKey *k, const SiftCDict *d);

#ifdef SIFT_TESTING
/* sift.test_col_dict_shared: 1 = not shared, 2 = every dictionary is shared, 3 = the table fails */
extern PGDLLIMPORT int sift_test_col_dict_shared;
extern void sift_dictshm_pinned(int64 *records, int64 *pins);
#endif

#endif
