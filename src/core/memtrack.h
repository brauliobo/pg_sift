/*
 * core/memtrack.h - the memory a path holds, measured by the allocator and held to the budget its setting gives.
 *
 * Every byte a path allocates belongs to one of three budgets: work_mem (hash_mem for hash tables), maintenance_work_mem, or the caches of core/cachebudget.h.
 * A tracker names the memory context tree of a path and the slice of a budget it may hold; the path asks it for its pressure at a batch boundary (a row group, a
 * unit, a run, a pass) and cuts, spills or flushes when the tree has reached the soft limit. It reads MemoryContextMemAllocated, never a formula of its own.
 *
 * A budget is divided by the processes that share it and the operators that hold it at once, and never raised: a slice is under one unit of I/O only when the setting is.
 * The contexts a tracker owns have blocks of at most 256 kB, so that what they free goes back to the allocator, and a statement that held a lot trims the heap once.
 */
#ifndef SIFT_MEMTRACK_H
#define SIFT_MEMTRACK_H

#include "core/host.h"

typedef enum SiftMemPressure
{
	SIFT_MEM_OK = 0,			/* under the soft limit (80% of the budget) */
	SIFT_MEM_SOFT = 1,			/* the soft limit or over, under the hard limit (95%): cut, spill or flush now */
	SIFT_MEM_HARD = 2			/* the hard limit or over: spill at once or refuse */
} SiftMemPressure;

typedef enum SiftBudgetClass
{
	SIFT_BUDGET_WORK_MEM = 0,	/* work_mem */
	SIFT_BUDGET_HASH_MEM = 1,	/* work_mem * hash_mem_multiplier */
	SIFT_BUDGET_MAINT_MEM = 2	/* maintenance_work_mem */
} SiftBudgetClass;

/* the least a slice is when a budget is divided among many: one block of I/O; a setting under it is the slice */
#define SIFT_MEM_MIN_QUERY_BYTES	((Size) 256 * 1024)
#define SIFT_MEM_MIN_MAINT_BYTES	((Size) 1024 * 1024)

#define SIFT_MEM_INIT_BLOCK			(8 * 1024)
#define SIFT_MEM_GEN_BLOCK			(64 * 1024)
#define SIFT_MEM_MAX_BLOCK			(256 * 1024)

#define SIFT_MEM_SOFT_PCT			80
#define SIFT_MEM_HARD_PCT			95

typedef struct SiftMemTracker
{
	const char *name;			/* what it measures: the row of sift._test_mem_highwater() */
	MemoryContext root_cx;		/* the tree MemoryContextMemAllocated measures */
	SiftBudgetClass cls;
	Size		budget;
	Size		soft_limit;
	Size		hard_limit;
	Size		peak_bytes;
	uint64		checks;			/* exact walks of the tree */
	uint32		sample_mask;	/* a power of two minus one: an inexact check in a row loop walks the tree once in sample_mask + 1 calls */
	uint32		sample_cnt;
	Size		last_used;
	bool		owns_cx;		/* sift_mem_end() deletes root_cx */
	struct SiftMemTracker *parent;	/* the tracker of the tree this one is part of: measured with it */
	Size		base;			/* what the tree held when the tracker began (sift_mem_rebase): not counted */
} SiftMemTracker;

extern Size sift_mem_budget_bytes(SiftBudgetClass cls, int nparticipants, int noperators);

/* a tracker over parent_cx itself (create_child false) or over a child of it that sift_mem_end() deletes, with the slice of cls that nparticipants processes and noperators operators leave */
extern void sift_mem_init(SiftMemTracker *t, const char *name, MemoryContext parent_cx, bool create_child,
						  SiftBudgetClass cls, int nparticipants, int noperators);

/* a tracker over cx with an explicit slice of a larger budget */
extern void sift_mem_init_sub(SiftMemTracker *t, const char *name, MemoryContext cx, bool owns_cx, Size slice_bytes);

/* the tree already holds what its owner built before this path began: only what it grows by counts */
extern void sift_mem_rebase(SiftMemTracker *t);
extern void sift_mem_end(SiftMemTracker *t);
extern Size sift_mem_used(SiftMemTracker *t);
extern SiftMemPressure sift_mem_pressure(SiftMemTracker *t, bool exact);
extern bool sift_mem_would_exceed(SiftMemTracker *t, Size extra);
extern void *sift_mem_alloc(SiftMemTracker *t, MemoryContext cx, Size bytes);

static inline Size
sift_mem_remaining(SiftMemTracker *t, bool exact)
{
	Size		used = exact ? sift_mem_used(t) : t->last_used;

	return used < t->hard_limit ? t->hard_limit - used : 0;
}

extern MemoryContext sift_mem_allocset_create(MemoryContext parent, const char *name);
extern MemoryContext sift_mem_generation_create(MemoryContext parent, const char *name);

/* after a context tree that held freed_bytes is deleted: gives the heap back to the system once enough has been freed, never inside a loop */
extern void sift_mem_release_os(Size freed_bytes);
/* hands the free heap of the process back to the system now */
extern void sift_mem_trim_os(void);

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

#ifdef SIFT_TESTING
extern void sift_mem_assert_within(SiftMemTracker *t);
extern void sift_memtrack_define_test_gucs(void);
extern void sift_memtrack_begin(SiftMemTracker *t);
extern void sift_memtrack_note(SiftMemTracker *t, Size used);
#define sift_mem_assert_sampled(t) \
	do { if (((t)->sample_cnt++ & (t)->sample_mask) == 0) sift_mem_assert_within(t); } while (0)
#else
#define sift_mem_assert_within(t) ((void) 0)
#define sift_mem_assert_sampled(t) ((void) 0)
#define sift_memtrack_begin(t) ((void) 0)
#define sift_memtrack_note(t, used) ((void) 0)
#endif

#endif							/* SIFT_MEMTRACK_H */
