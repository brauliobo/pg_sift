/*
 * agg/par/stats.c - testing: what each participant of a parallel scan cost (sift.test_par_stats)
 * A participant (the leader from the start of its scan to the end of it, a worker from its entry point to its last message) logs one line: the user instructions it ran (a counter of
 * its own), the shared blocks it hit and read, its memory (peak and current resident set, the proportional set and its anonymous part from /proc/self/smaps_rollup, which is
 * the memory the participant added to the host) and the memory of its group table. The leader logs the sizes of the shared memory of the statement as well. The bench step
 * bench/steps/scope_parallel.py reads these lines; a release build has none of this.
 */
#include "postgres.h"

#include "core/visibility.h"
#include "agg/par/int.h"

#if defined(SIFT_TESTING) && defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "executor/instrument.h"
#include "utils/memutils.h"

struct ParStats
{
	int			fd;
	BufferUsage bu;
	long		anon_kb;
};

static ParStats *leader_stats;

static long
rollup_kb(const char *key)
{
	char		line[256];
	size_t		n = strlen(key);
	long		kb = -1;
	FILE	   *f = fopen("/proc/self/smaps_rollup", "r");

	while (f != NULL && fgets(line, sizeof(line), f))
		if (strncmp(line, key, n) == 0 && line[n] == ':')
		{
			kb = strtol(line + n + 1, NULL, 10);
			break;
		}
	if (f != NULL)
		fclose(f);
	return kb;
}

static long
status_kb(const char *key)
{
	char		line[256];
	size_t		n = strlen(key);
	long		kb = -1;
	FILE	   *f = fopen("/proc/self/status", "r");

	while (f != NULL && fgets(line, sizeof(line), f))
		if (strncmp(line, key, n) == 0 && line[n] == ':')
		{
			kb = strtol(line + n + 1, NULL, 10);
			break;
		}
	if (f != NULL)
		fclose(f);
	return kb;
}

ParStats *
par_stats_begin(bool leader)
{
	struct perf_event_attr attr = {.type = PERF_TYPE_HARDWARE,.size = sizeof(attr),.config = PERF_COUNT_HW_INSTRUCTIONS,.exclude_kernel = 1,.exclude_hv = 1};
	ParStats   *s;
	FILE	   *f;

	if (!sift_test_par_stats)
		return NULL;
	s = MemoryContextAllocZero(TopMemoryContext, sizeof(ParStats));
	if (leader && (f = fopen("/proc/self/clear_refs", "w")) != NULL)
	{
		fputs("5", f);			/* the peak resident set starts from now */
		fclose(f);
	}
	s->anon_kb = rollup_kb("Pss_Anon");
	s->bu = pgBufferUsage;
	s->fd = (int) syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
	if (leader)
		leader_stats = s;
	return s;
}

void
par_stats_end(ParStats *s, AggExec *ex, const char *role)
{
	uint64		instr = 0;

	if (s == NULL)
		return;
	if (s->fd >= 0)
	{
		if (read(s->fd, &instr, sizeof(instr)) != sizeof(instr))
			instr = 0;
		close(s->fd);
	}
	ereport(LOG, (errmsg("sift par stats: role=%s pid=%d instr=%llu hit=%lld read=%lld rss_peak_kb=%ld rss_kb=%ld pss_kb=%ld pss_anon_kb=%ld pss_anon_start_kb=%ld pss_shmem_kb=%ld pss_file_kb=%ld table_bytes=%llu node_bytes=%llu",
						  role, MyProcPid, (unsigned long long) instr, (long long) (pgBufferUsage.shared_blks_hit - s->bu.shared_blks_hit),
						  (long long) (pgBufferUsage.shared_blks_read - s->bu.shared_blks_read), status_kb("VmHWM"), status_kb("VmRSS"), rollup_kb("Pss"), rollup_kb("Pss_Anon"), s->anon_kb,
						  rollup_kb("Pss_Shmem"), rollup_kb("Pss_File"), ex->tcxt ? (unsigned long long) MemoryContextMemAllocated(ex->tcxt, true) : 0ULL,
						  ex->cxt ? (unsigned long long) MemoryContextMemAllocated(ex->cxt, true) : 0ULL)));
	if (s == leader_stats)
		leader_stats = NULL;
	pfree(s);
}

/* the leader's end of the scan, which may be reached from either of its scans (items, top-N) */
void
par_stats_leader_end(AggExec *ex)
{
	par_stats_end(leader_stats, ex, "leader");
}

void
par_stats_dsm(const ParStatsDsm *d)
{
	if (sift_test_par_stats)
		ereport(LOG, (errmsg("sift par dsm: workers=%d segment=%zu shared=%zu spec=%zu params=%zu snapshot=%zu view=%zu queues=%zu maps=%zu scope=%zu other=%zu",
							 d->launched, d->segment, d->shared, d->spec, d->params, d->snapshot, d->view, d->queues, d->maps, d->scope, d->other)));
}

#else

ParStats *
par_stats_begin(bool leader)
{
	return NULL;
}

void
par_stats_end(ParStats *s, AggExec *ex, const char *role)
{
}

void
par_stats_leader_end(AggExec *ex)
{
}

void
par_stats_dsm(const ParStatsDsm *d)
{
}

#endif
