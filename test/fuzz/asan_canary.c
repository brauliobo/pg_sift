/*
 * Deliberate heap overflow, built into its own module by scripts/fuzz.sh
 * (target "canary") to prove that the ASan server pipeline detects, aborts
 * and reports memory errors. Never part of sift.
 */
#include "postgres.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(sift_asan_canary);

Datum
sift_asan_canary(PG_FUNCTION_ARGS)
{
	/* malloc, not palloc: ASan has no redzones inside memory contexts */
	volatile char *p = malloc(8);

	p[PG_GETARG_INT32(0)] = 1;
	free((void *) p);
	PG_RETURN_VOID();
}
