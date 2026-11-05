/*
 * agg/combo/probe_test.c - test-only: sift.test_probe_hook runs a SQL command at a stage of the direct probe of a correlated child
 * (agg/combo/probe.c), compiled only with SIFT_TESTING. The stages are the points where a concurrent session could change the index
 * with no page of it locked by the probe: 0 after a parent page was released and before the child it pointed to is read, 1 after the
 * leaf was released and before its heap tuples are fetched, 2 before the page to the right of a leaf is read. The setting is
 * 'STAGE:N:COMMAND': the command runs once, when the stage is reached for the Nth time since the setting was made. The command runs in
 * the probing backend as a later command of its transaction: what it writes is not visible to the statement's snapshot, as the writes of
 * a session that started after the statement are not.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "executor/spi.h"
#include "utils/guc.h"

#include "agg/combo/int.h"

char	   *sift_test_probe_hook = NULL;
static int	hook_hits[3];
static bool	hook_busy;

void
sift_test_probe_hook_assign(const char *newval, void *extra)
{
	memset(hook_hits, 0, sizeof(hook_hits));
}

void
combo_probe_hook(int stage)
{
	int			at,
				n,
				len;

	if (sift_test_probe_hook == NULL || sift_test_probe_hook[0] == '\0' || hook_busy)
		return;
	if (sscanf(sift_test_probe_hook, "%d:%d:%n", &at, &n, &len) != 2 || at != stage || ++hook_hits[stage] != n)
		return;
	hook_busy = true;
	PG_TRY();
	{
		SPI_connect();
		SPI_execute(sift_test_probe_hook + len, false, 0);
		SPI_finish();
	}
	PG_FINALLY();
	{
		hook_busy = false;
	}
	PG_END_TRY();
}

#endif							/* SIFT_TESTING */
