/*
 * core/rowmem.h - the allocation of an array whose size follows the rows of a segment, a run or a unit (a sort copy, the entries of a point directory
 * or of an order array). One helper for all of them, so that the largest array a build asks for is one number, the limit array_bytes of
 * core/limits.h, and a test lowers it (sift.test_limits = 'array_bytes=N') to find the build that sizes an array by the table and not by its memory budget.
 *
 * A writer cuts a segment at its memory budget (sift_colw_memory), which counts these arrays, so they are bounded by it, whatever the table: above 1 GB the
 * array is a huge allocation (the budget says how much memory the user gave the build), and there is no other limit. A test lowers array_bytes to fail a
 * build whose accounting misses an array on a few rows, not after a long scan.
 */
#ifndef SIFT_ROWMEM_H
#define SIFT_ROWMEM_H

#include "core/host.h"
#include "core/limits.h"

static inline void *
sift_rows_alloc(Size bytes)
{
	if (bytes > (Size) sift_limit(array_bytes, SIFT_ARRAY_UNLIMITED))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("an array of %zu bytes passes the limit of %zu bytes", (size_t) bytes, (size_t) sift_limit(array_bytes, SIFT_ARRAY_UNLIMITED)),
						errhint("A segment is cut at its memory budget, which counts the arrays it builds at its end.")));
	return bytes > MaxAllocSize ? MemoryContextAllocHuge(CurrentMemoryContext, bytes) : palloc(bytes);
}

/*
 * A structure that grows with the data of one statement (the values of a facet, the rows a search holds, a verification of a segment) and that no
 * algorithm here can spill: it takes the memory its setting (work_mem, maintenance_work_mem, in kB) gives, or the statement fails naming it.
 */
static inline void
sift_budget_check(Size bytes, int budget_kb, const char *what, const char *setting, const char *hint)
{
	if (bytes / 1024 > (Size) budget_kb)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("%s needs %zu kB, more than %s (%d kB)", what, (size_t) (bytes / 1024), setting, budget_kb),
						errhint("Raise %s%s%s.", setting, hint ? ", or " : "", hint ? hint : "")));
}

#endif							/* SIFT_ROWMEM_H */
