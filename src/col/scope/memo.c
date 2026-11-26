/*
 * col/scope/memo.c - see memo.h
 */
#include "postgres.h"

#include "col/scope/memo.h"

MemoryContext
sift_scope_memo_begin(SiftScopeMemoSlot *slot, const char *name)
{
	if (slot->cx == NULL)
		slot->cx = sift_cache_create_context(name);
	MemoryContextReset(slot->cx);
	slot->used = sift_cache_tick();
	return slot->cx;
}

static SiftScopeMemoSlot *
oldest_slot(void *ring, Size stride, int n)
{
	SiftScopeMemoSlot *victim = NULL;

	for (int i = 0; i < n; i++)
	{
		SiftScopeMemoSlot *s = (SiftScopeMemoSlot *) ((char *) ring + i * stride);

		if (s->cx != NULL && !sift_cache_used_in_xact(s->used) && (victim == NULL || s->used < victim->used))
			victim = s;
	}
	return victim;
}

uint64
sift_scope_memo_oldest(const void *ring, Size stride, int n)
{
	const SiftScopeMemoSlot *s = oldest_slot((void *) ring, stride, n);

	return s == NULL ? UINT64_MAX : s->used;
}

bool
sift_scope_memo_evict_oldest(void *ring, Size stride, int n)
{
	SiftScopeMemoSlot *s = oldest_slot(ring, stride, n);

	if (s == NULL)
		return false;
	MemoryContextDelete(s->cx);
	memset(s, 0, stride);
	return true;
}

void
sift_scope_memo_register(bool *registered, const SiftCacheKind *kind)
{
	if (*registered)
		return;
	sift_cache_register(kind);
	*registered = true;
}
