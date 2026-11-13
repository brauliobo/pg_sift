/*
 * col/dsmreg.h - a named table in the dynamic shared memory registry that a cache must not fail for (agg/verdict.c, col/leafshm.c, col/dictshm.c). The only
 * code that asks the registry: it registers the name with the size appended (col/dsmreg.c).
 */
#ifndef SIFT_COL_DSMREG_H
#define SIFT_COL_DSMREG_H

#include "postgres.h"

#if PG_VERSION_NUM >= 190000
#define SIFT_DSM_INIT_ARGS		void *ptr, void *arg
#else
#define SIFT_DSM_INIT_ARGS		void *ptr
#endif

typedef void (*SiftDsmInit) (SIFT_DSM_INIT_ARGS);

/* The table another process made, with no protection against its failure: for a parallel worker, where the leader found that it is there (a subtransaction cannot start there) */
extern void *sift_dsm_join(const char *name, Size size, SiftDsmInit init);
/* NULL (and *failed set, so that the backend does not try again) where the table could not be had; `what` names what is not shared, `guc` the setting that turns it off (NULL: none) */
extern void *sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *guc, bool test_fail);

#ifdef SIFT_TESTING
#define SIFT_DSM_DEFERRED_MAX	8
typedef struct SiftDsmDeferred
{
	char		name[48];
	int64		n;
} SiftDsmDeferred;
extern SiftDsmDeferred sift_dsm_deferred[SIFT_DSM_DEFERRED_MAX];
#endif

#endif
