/*
 * col/dsmreg.c - see dsmreg.h.
 *
 * The registry is the first backend that asks creating the table (nothing is allocated at postmaster start). A cache must not turn into an error:
 * whatever fails here (no room in /dev/shm, no more segments, a segment of another size) is caught in a subtransaction, which also gives back the
 * locks the registry holds, and the backend goes without the table.
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/dsm_registry.h"
#include "utils/resowner.h"

#include "col/am.h"
#include "col/dsmreg.h"

void *
sift_dsm_join(const char *name, Size size, SiftDsmInit init)
{
	bool		found;

#if PG_VERSION_NUM >= 190000
	return GetNamedDSMSegment(name, size, init, &found, NULL);
#else
	return GetNamedDSMSegment(name, size, init, &found);
#endif
}

#ifdef SIFT_TESTING
SiftDsmDeferred sift_dsm_deferred[SIFT_DSM_DEFERRED_MAX];

/* The tables this backend went without because a statement was writing (sift._test_dsm_deferred): the test that a write statement reached an attach point */
static void
deferred_note(const char *name)
{
	for (int i = 0; i < SIFT_DSM_DEFERRED_MAX; i++)
		if (sift_dsm_deferred[i].n == 0 || strcmp(sift_dsm_deferred[i].name, name) == 0)
		{
			strlcpy(sift_dsm_deferred[i].name, name, sizeof(sift_dsm_deferred[i].name));
			sift_dsm_deferred[i].n++;
			return;
		}
}
#endif

void *
sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *guc, bool test_fail)
{
	MemoryContext cx = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	void	   *volatile s = NULL;

	/* a subtransaction cannot start where there is no transaction or a parallel operation, and must not under a write (sift_col_writing); the next statement of the backend tries again */
	if (!IsTransactionState() || IsInParallelMode())
		return NULL;
	if (sift_col_writing())
	{
#ifdef SIFT_TESTING
		deferred_note(name);
#endif
		return NULL;
	}
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cx);
	PG_TRY();
	{
		if (test_fail)
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift: test: the table of %s fails", what)));
		s = sift_dsm_join(name, size, init);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cx);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(cx);
		e = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cx);
		CurrentResourceOwner = owner;
		*failed = true;
		s = NULL;
		ereport(LOG,
				(errmsg("sift: %s are not shared with other backends: %s", what, e->message),
				 guc ? errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm), or set %s to 0.", guc)
				 : errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm).")));
		FreeErrorData(e);
	}
	PG_END_TRY();
	return s;
}
