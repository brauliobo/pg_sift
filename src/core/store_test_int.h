/*
 * store_test_int.h - what store_test.c and store_ops_test.c share: the test segment type and the helpers that cross them (SIFT_TESTING only).
 */
#ifndef SIFT_CORE_STORE_TEST_INT_H
#define SIFT_CORE_STORE_TEST_INT_H

#include "postgres.h"
#include "postgres.h"

#ifdef SIFT_TESTING


#include "access/generic_xlog.h"
#include "access/relation.h"
#include "funcapi.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/timestamp.h"

#include "bm25/maint.h"
#include "core/store.h"
#include "core/testrng.h"
#include "core/sql.h"


#define PAYLOAD			SIFT_PAGE_PAYLOAD
#define MAX_RANDOM_PIECE (3 * PAYLOAD)
#define MAINT_LOCK(index, mode)	LockPage(index, SIFT_MAINT_LOCK_BLKNO, mode)
#define MAINT_UNLOCK(index, mode) UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, mode)
#define ULL(x)			((unsigned long long) (x))

typedef struct TestSeg
{
	uint64		gen;
	BlockNumber header;
	uint64		size;			/* logical bytes, padding included */
	uint64		payload;		/* bytes excluding padding */
	uint64		seed;
	uint32		piece;
	uint32		align;
	int			nextents;
	SiftExtent *extents;
} TestSeg;


extern void sleep_ms(int ms);
extern bool find_segment(Relation index, uint64 gen, SiftDirSnapshot *snap, TestSeg *seg);
extern uint64 read_segment(Relation index, const TestSeg *seg, int mode);
extern uint64 seg_write(Relation index, uint64 nbytes, uint64 seed, uint32 piece, uint32 align);
extern int64 seg_drop(Relation index, uint64 gen);
extern int64 sweep(Relation index);
extern BlockNumber alloc_many(Relation index, uint32 nblocks, int count, int release);

#endif							/* SIFT_TESTING */

#endif
