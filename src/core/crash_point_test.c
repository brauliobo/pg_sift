/*
 * core/crash_point_test.c - sift.test_crash_point (core/crash_point.h): a tag that names no crash point is an error of the SET, so a test finds a tag its build lacks at once
 * instead of waiting for a crash that never comes. Compiled only with SIFT_TESTING.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <signal.h>

#include "miscadmin.h"
#include "utils/guc.h"

#include "core/crash_point.h"
#include "core/store/store.h"

#define NO_CRASH_POINT	(-1)

static const char *const crash_point_names[] = {
#define X(tag) #tag,
	SIFT_CRASH_POINT_LIST(X)
#undef X
};

static char *crash_point_setting = NULL;
static int	armed = NO_CRASH_POINT;

static bool
crash_point_check(char **newval, void **extra, GucSource source)
{
	int			id = NO_CRASH_POINT;
	int		   *parsed;

	if (**newval != '\0')
	{
		for (int i = 0; i < SIFT_CP_COUNT && id == NO_CRASH_POINT; i++)
			if (strcmp(*newval, crash_point_names[i]) == 0)
				id = i;
		if (id == NO_CRASH_POINT)
		{
			GUC_check_errmsg("unknown crash point \"%s\"", *newval);
			GUC_check_errhint("The crash points of this build are listed in core/crash_point.h.");
			return false;
		}
	}
	parsed = guc_malloc(LOG, sizeof(int));
	if (parsed == NULL)
		return false;
	*parsed = id;
	*extra = parsed;
	return true;
}

static void
crash_point_assign(const char *newval, void *extra)
{
	armed = *(int *) extra;
}

void
sift_crash_point_gucs(void)
{
	DefineCustomStringVariable("sift.test_crash_point",
							   "Testing: SIGKILL, the WAL flushed, at the first pass of the crash point of this tag (empty = none).",
							   NULL, &crash_point_setting, "", PGC_USERSET, GUC_NOT_IN_SAMPLE, crash_point_check, crash_point_assign, NULL);
}

void
sift_crash_point(SiftCrashPoint cp)
{
	if ((int) cp != armed)
		return;
	ereport(LOG, (errmsg("sift test: crash point %s", crash_point_names[cp])));
	sift_wal_flush_inserted();
	kill(MyProcPid, SIGKILL);
}

#endif
