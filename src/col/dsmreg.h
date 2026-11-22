/*
 * col/dsmreg.h - a named table in dynamic shared memory that a cache must not fail for (agg/verdict.c, col/leafshm.c, col/dictshm.c). The only code that
 * asks the registry of PostgreSQL: it keeps the tables in a directory by name, library identity and size, and gives back the ones nobody maps (col/dsmreg.c).
 */
#ifndef SIFT_COL_DSMREG_H
#define SIFT_COL_DSMREG_H

#include "postgres.h"

#include "storage/dsm.h"
#include "storage/lwlock.h"

#if PG_VERSION_NUM >= 190000
#define SIFT_DSM_INIT_ARGS		void *ptr, void *arg
#define SIFT_DSM_INIT_CALL(init, ptr)	(init) ((ptr), NULL)
#else
#define SIFT_DSM_INIT_ARGS		void *ptr
#define SIFT_DSM_INIT_CALL(init, ptr)	(init) (ptr)
#endif

typedef void (*SiftDsmInit) (SIFT_DSM_INIT_ARGS);

/* A tranche of LWLocks: 19 names it when it is made and registers nothing, 17 and 18 register the name in every backend that uses a lock of it */
#if PG_VERSION_NUM >= 190000
#define sift_tranche_new(name)					LWLockNewTrancheId(name)
#define sift_tranche_register(tranche, name)	((void) 0)
#else
static inline int
sift_tranche_new(const char *name pg_attribute_unused())
{
	return LWLockNewTrancheId();
}

#define sift_tranche_register(tranche, name)	LWLockRegisterTranche((tranche), (name))
#endif

/*
 * The table, or NULL where it cannot be had: the directory is full, it is not the directory of this library, or the system has no memory for the segment of the table or of the
 * directory (no room in /dev/shm, no more segments; sift_dsm_why says which). Never an error for a shortage, wherever it is called (a parallel worker, a background worker and a statement that writes cannot start the
 * subtransaction of sift_dsm_attach): the caller goes without the table and decides what that costs
 */
extern void *sift_dsm_try(const char *name, Size size, SiftDsmInit init);
/* Why the last sift_dsm_try of this backend gave NULL */
extern const char *sift_dsm_why(void);
/* The table as sift_dsm_try gives it, an ERROR where that gives NULL: for sift_dsm_attach, which catches it, and the tests */
extern void *sift_dsm_join(const char *name, Size size, SiftDsmInit init);
/* This backend lets go of a table it maps; the backend that lets go last gives the segment back to the system */
extern void sift_dsm_unmap(const char *name, Size size);
/* A table of this library that exists, mapped in this backend; NULL where there is none: nothing is made. *size: what the directory records (may be NULL) */
extern void *sift_dsm_find(const char *name, Size *size);
/* Entry i of the tables of this library, for sift.shared_memory(): false past the last, an empty name where the entry is free or another library's */
#define SIFT_DSM_NAME_LEN	40
extern bool sift_dsm_list(int i, char name[SIFT_DSM_NAME_LEN], uint64 *size);
/* What one table of the directory says of its use for sift.shared_memory(): the bytes in use and the reads that found and that missed; what a table does not keep is left out */
typedef struct SiftShmStat
{
	bool		has_used;
	bool		has_counts;
	int64		used;
	int64		hits;
	int64		misses;
} SiftShmStat;
/*
 * Whether the postmaster has not been told yet why this table is not shared (or has been told another reason): the caller logs it then. The record is a table of the directory
 * (col/shmstatus.c) that sift.shared_memory() reads; where even that cannot be made, every backend says it.
 */
extern bool sift_shm_first(const char *name, const char *reason);

/* NULL (and *failed set, so that the backend does not try again) where the table could not be had; `what` names what is not shared, `guc` the setting that turns it off (NULL: none) */
extern void *sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *guc, bool test_fail);

#ifdef SIFT_TESTING
/* sift.test_dsm_fail: parts of table names whose segment cannot be made, as when /dev/shm is full ("bgworker:" before a part: for background workers only) */
extern PGDLLIMPORT char *sift_test_dsm_fail;
/* The identity of the library that asks for a table, as a test sets it (sift._test_dsm_layout): a library of another layout */
extern char sift_dsm_test_build[16];
/* Entry i of the directory of the tables (sift._test_dsm_entries): false past the last; a free entry has the handle DSM_HANDLE_INVALID */
extern uint32 sift_dsm_test_magic(uint32 magic);
extern void sift_dsm_test_leak_user(void);
extern bool sift_dsm_entry(int i, const char **name, const char **build, uint64 *size, dsm_handle *handle, uint32 *users);

#define SIFT_DSM_DEFERRED_MAX	8
typedef struct SiftDsmDeferred
{
	char		name[48];
	int64		n;
} SiftDsmDeferred;
extern SiftDsmDeferred sift_dsm_deferred[SIFT_DSM_DEFERRED_MAX];
#endif

#endif
