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
#include "utils/memutils.h"
#include "utils/resowner.h"

#include "col/dsmreg.h"

void *
sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *guc, bool test_fail)
{
	MemoryContext cx = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	void	   *volatile s = NULL;
	bool		found;

	/* a subtransaction cannot start where there is no transaction or a parallel operation; the next statement of the backend tries again */
	if (!IsTransactionState() || IsInParallelMode())
		return NULL;
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cx);
	PG_TRY();
	{
		if (test_fail)
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift: test: the table of %s fails", what)));
#if PG_VERSION_NUM >= 190000
		s = GetNamedDSMSegment(name, size, init, &found, NULL);
#else
		s = GetNamedDSMSegment(name, size, init, &found);
#endif
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
				 errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm), or set %s to 0.", guc)));
		FreeErrorData(e);
	}
	PG_END_TRY();
	return s;
}
