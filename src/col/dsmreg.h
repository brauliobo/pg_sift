/*
 * col/dsmreg.h - a named table in the dynamic shared memory registry that a cache must not fail for (agg/verdict.c, col/leafshm.c).
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

/* NULL (and *failed set, so that the backend does not try again) where the table could not be had; `what` names what is not shared, `guc` the setting that turns it off */
extern void *sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *guc, bool test_fail);

#endif
