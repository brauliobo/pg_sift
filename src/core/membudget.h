/*
 * core/membudget.h - the expected memory of a main path, asserted wherever a test runs it (docs/SIZING.md "Memory budgets", test/mem-budgets.tsv).
 *
 * SIFT_MEM_SCOPE(name, context) at the entry of a path names the memory context tree that holds the path's own allocations; its budget is the formula of the row
 * `name` of test/mem-budgets.tsv (settings and a constant, never a number of rows). SIFT_MEM_CHECKPOINT() at a batch boundary (a run, a unit, a group, a merge pass,
 * a statement end) sums that tree and fails with `sift memory budget exceeded: <name>: <actual> > <budget> bytes (<formula>)` when it is over, and records the
 * high-water mark of the scope, which the log shows at backend exit (SIFTMEM_SCOPE lines) and sift._test_mem_highwater() returns.
 *
 * Both compile to nothing without SIFT_TESTING: the release library has no budget code, no symbol and no instruction of it.
 */
#ifndef SIFT_MEMBUDGET_H
#define SIFT_MEMBUDGET_H

#ifdef SIFT_TESTING

#include <stddef.h>

typedef struct MemoryContextData *MemoryContext;

typedef struct SiftMemScope
{
	int			depth;
} SiftMemScope;

extern SiftMemScope sift_mem_scope_begin(const char *name, MemoryContext cx);
extern void sift_mem_scope_end(SiftMemScope *scope);
extern void sift_mem_checkpoint(void);

#define SIFT_MEM_CAT_(a, b) a##b
#define SIFT_MEM_CAT(a, b) SIFT_MEM_CAT_(a, b)
#define SIFT_MEM_SCOPE(name, cx) \
	SiftMemScope SIFT_MEM_CAT(sift_mem_scope_, __LINE__) __attribute__((cleanup(sift_mem_scope_end))) = sift_mem_scope_begin(#name, (cx))
#define SIFT_MEM_CHECKPOINT() sift_mem_checkpoint()

#else

#define SIFT_MEM_SCOPE(name, cx) _Static_assert(1, "no memory scope in the release library")
#define SIFT_MEM_CHECKPOINT() ((void) 0)

#endif							/* SIFT_TESTING */

#endif							/* SIFT_MEMBUDGET_H */
