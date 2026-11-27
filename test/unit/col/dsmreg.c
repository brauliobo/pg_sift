/*
 * test/unit/col/dsmreg.c - the creation of a segment of col/dsmreg.c that fails: a shortage is no table, any other error goes on; either way the attempt gives back the resource owner it made
 * and leaves the one of the caller current.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "col/dsmreg.c"

#include "test/common/check.h"

volatile uint32 InterruptHoldoffCount;
ErrorContextCallback *error_context_stack;
sigjmp_buf *PG_exception_stack;
ResourceOwner CurrentResourceOwner;
BackendType MyBackendType = B_BACKEND;

static char caller_owner;
static char attempt_owner;
static int	owners_live;
static int	fail_code;
static ErrorData error;

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

ResourceOwner
ResourceOwnerCreate(ResourceOwner parent, const char *name)
{
	owners_live++;
	return (ResourceOwner) &attempt_owner;
}

void
ResourceOwnerRelease(ResourceOwner owner, ResourceReleasePhase phase, bool isCommit, bool isTopLevel)
{
}

void
ResourceOwnerDelete(ResourceOwner owner)
{
	SIFT_CHECKF(owner == (ResourceOwner) &attempt_owner && CurrentResourceOwner != owner, "the resource owner deleted is the attempt's, and not current");
	owners_live--;
}

/* The system has no segment to give: the error of the code the test sets */
dsm_segment *
dsm_create(Size size, int flags)
{
	error.sqlerrcode = fail_code;
	error.message = "could not create shared memory segment (test)";
	pg_re_throw();
	return NULL;
}

ErrorData *
CopyErrorData(void)
{
	ErrorData  *e = palloc(sizeof(ErrorData));

	*e = error;
	return e;
}

void
FreeErrorData(ErrorData *e)
{
	pfree(e);
}

void
FlushErrorState(void)
{
}

static dsm_segment *made;

static void
create(void)
{
	made = create_segment("sift_test_table", 1024);
}

/* Whether the creation raised an ERROR, caught as the caller of the code under test would catch it */
static bool
create_raises(int code)
{
	sigjmp_buf	jb;
	sigjmp_buf *outer = PG_exception_stack;

	fail_code = code;
	made = (dsm_segment *) &made;
	if (sigsetjmp(jb, 0) != 0)
	{
		PG_exception_stack = outer;
		return true;
	}
	PG_exception_stack = &jb;
	create();
	PG_exception_stack = outer;
	return false;
}

static void
test_failed_creation_gives_owner_back(void)
{
	static const struct
	{
		int			code;
		bool		raises;
		const char *what;
	}			cases[] = {
		{ERRCODE_DISK_FULL, false, "a shortage"},
		{ERRCODE_INTERNAL_ERROR, true, "another error"},
	};

	for (int i = 0; i < lengthof(cases); i++)
	{
		CurrentResourceOwner = (ResourceOwner) &caller_owner;
		SIFT_CHECKF(create_raises(cases[i].code) == cases[i].raises, "%s %s", cases[i].what, cases[i].raises ? "goes on" : "is no table");
		SIFT_CHECKF(cases[i].raises || made == NULL, "%s is no segment", cases[i].what);
		SIFT_CHECKF(CurrentResourceOwner == (ResourceOwner) &caller_owner, "after %s the resource owner of the attempt is left current", cases[i].what);
		SIFT_CHECKF(owners_live == 0, "after %s the resource owner of the attempt is never deleted", cases[i].what);
		owners_live = 0;
	}
}

int
main(void)
{
#ifndef __SANITIZE_THREAD__
	/* one thread; ThreadSanitizer does not find the jmp_buf the ERROR goes to */
	test_failed_creation_gives_owner_back();
#endif
	return sift_check_summary();
}
