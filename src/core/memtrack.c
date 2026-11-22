/*
 * core/memtrack.c - the trackers of the memory a path holds (core/memtrack.h).
 *
 * The test telemetry of the trackers (high-water marks, census of the contexts) is core/memtrack_test.c.
 */
#include "postgres.h"

#include "executor/nodeHash.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "core/memtrack.h"

/* MEM[MEM-IDLE-14]: glibc malloc_trim thresholds (1 MB peak / 2 MB accum release-to-OS) */
#define TRIM_PEAK_BYTES		((Size) 1024 * 1024)
#define TRIM_ACCUM_BYTES	((Size) 2 * 1024 * 1024)

static Size trim_accum;

Size
sift_mem_budget_bytes(SiftBudgetClass cls, int nparticipants, int noperators)
{
	Size		raw;
	Size		floor;
	Size		divisor = (Size) Max(nparticipants, 1) * (Size) Max(noperators, 1);

	switch (cls)
	{
		case SIFT_BUDGET_WORK_MEM:
			raw = (Size) work_mem * 1024;
			floor = SIFT_MEM_MIN_QUERY_BYTES;
			break;
		case SIFT_BUDGET_HASH_MEM:
			raw = get_hash_memory_limit();
			floor = SIFT_MEM_MIN_QUERY_BYTES;
			break;
		default:
			raw = (Size) maintenance_work_mem * 1024;
			floor = SIFT_MEM_MIN_MAINT_BYTES;
			break;
	}
	return Max(raw / divisor, Min(raw, floor));
}

static void
tracker_set(SiftMemTracker *t, const char *name, MemoryContext cx, bool owns_cx, SiftBudgetClass cls, Size budget)
{
	*t = (SiftMemTracker) {
		.name = name, .root_cx = cx, .cls = cls, .budget = budget,
		.soft_limit = budget / 100 * SIFT_MEM_SOFT_PCT, .hard_limit = budget / 100 * SIFT_MEM_HARD_PCT,
		.sample_mask = 255, .owns_cx = owns_cx
	};
	sift_memtrack_begin(t);
}

void
sift_mem_init(SiftMemTracker *t, const char *name, MemoryContext parent_cx, bool create_child,
			  SiftBudgetClass cls, int nparticipants, int noperators)
{
	MemoryContext cx = create_child ? sift_mem_allocset_create(parent_cx, name) : parent_cx;

	tracker_set(t, name, cx, create_child, cls, sift_mem_budget_bytes(cls, nparticipants, noperators));
}

void
sift_mem_init_sub(SiftMemTracker *t, const char *name, MemoryContext cx, bool owns_cx, Size slice_bytes)
{
	tracker_set(t, name, cx, owns_cx, SIFT_BUDGET_MAINT_MEM, slice_bytes);
}

Size
sift_mem_used(SiftMemTracker *t)
{
	Size		used = MemoryContextMemAllocated(t->root_cx, true);

	used = used > t->base ? used - t->base : 0;
	t->checks++;
	t->last_used = used;
	t->peak_bytes = Max(t->peak_bytes, used);
	sift_memtrack_note(t, used);
	return used;
}

SiftMemPressure
sift_mem_pressure(SiftMemTracker *t, bool exact)
{
	Size		used = t->last_used;

	if (exact || (t->sample_cnt++ & t->sample_mask) == 0)
		used = sift_mem_used(t);
	return used >= t->hard_limit ? SIFT_MEM_HARD : used >= t->soft_limit ? SIFT_MEM_SOFT : SIFT_MEM_OK;
}

bool
sift_mem_would_exceed(SiftMemTracker *t, Size extra)
{
	return sift_mem_used(t) + extra > t->hard_limit;
}

void *
sift_mem_alloc(SiftMemTracker *t, MemoryContext cx, Size bytes)
{
	if (bytes > MaxAllocSize || bytes > t->budget)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("%s needs an allocation of %zu bytes, more than its memory budget of %zu bytes", t->name, bytes, t->budget),
						errhint("Raise %s.", t->cls == SIFT_BUDGET_MAINT_MEM ? "maintenance_work_mem" : "work_mem")));
	return MemoryContextAlloc(cx, bytes);
}

void
sift_mem_rebase(SiftMemTracker *t)
{
	t->base = MemoryContextMemAllocated(t->root_cx, true);
}

void
sift_mem_end(SiftMemTracker *t)
{
	Size		peak = t->peak_bytes;

	if (t->owns_cx && t->root_cx != NULL)
		MemoryContextDelete(t->root_cx);
	t->root_cx = NULL;
	sift_mem_release_os(peak);
}

/* MEM[MEM-CORE-03]: bounded AllocSet factory (8 KB init / 256 KB max block; SIFT_MEM_MAX_BLOCK) */
MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return AllocSetContextCreateInternal(parent, name, SIFT_MEM_INIT_BLOCK, SIFT_MEM_INIT_BLOCK, SIFT_MEM_MAX_BLOCK);
}

MemoryContext
sift_mem_generation_create(MemoryContext parent, const char *name)
{
	return GenerationContextCreate(parent, name, SIFT_MEM_INIT_BLOCK, SIFT_MEM_GEN_BLOCK, SIFT_MEM_MAX_BLOCK);
}

void
sift_mem_trim_os(void)
{
#ifdef __GLIBC__
	malloc_trim(0);
#endif
	trim_accum = 0;
}

void
sift_mem_trim_if_free(Size min_free_bytes)
{
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 33)
	if (mallinfo2().fordblks >= min_free_bytes)
		sift_mem_trim_os();
#endif
}

void
sift_mem_release_os(Size freed_bytes)
{
	trim_accum += freed_bytes;
	if (freed_bytes >= TRIM_PEAK_BYTES || trim_accum >= TRIM_ACCUM_BYTES)
		sift_mem_trim_os();
}
