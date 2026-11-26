/*
 * col/scope/memo.h - the rings of memos of the scopes (legs, specs, groups) in the budget of core/cachebudget.h: each entry starts with a SiftScopeMemoSlot, the context of what it keeps and
 * when it was last used, and a ring is a SiftCacheKind that gives its oldest entry that no statement of the transaction used back, whole.
 */
#ifndef SIFT_SCOPE_MEMO_H
#define SIFT_SCOPE_MEMO_H

#include "utils/memutils.h"

#include "core/cachebudget.h"

typedef struct SiftScopeMemoSlot
{
	MemoryContext cx;			/* NULL: the slot was never filled */
	uint64		used;
} SiftScopeMemoSlot;

/* The context of the slot, made under the budget on first use and emptied for what it keeps next; used is now */
extern MemoryContext sift_scope_memo_begin(SiftScopeMemoSlot *slot, const char *name);
extern uint64 sift_scope_memo_oldest(const void *ring, Size stride, int n);
extern bool sift_scope_memo_evict_oldest(void *ring, Size stride, int n);
extern void sift_scope_memo_register(bool *registered, const SiftCacheKind *kind);

#endif
