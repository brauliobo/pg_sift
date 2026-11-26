/*
 * core/maint/sched_reap.c - what keeps the queue of sched.h honest while nobody is looking: the claims and worker slots of processes that are gone, the heat of the relations that fades, and
 * the test that says the table has nothing in it for a launcher to look at.
 */
#include "core/maint/sched.h"

#include "datatype/timestamp.h"

/* The claim of a pid that will not complete it goes back to the queue; a failed step backs off, as an error of the step does */
static int
release_pid_locked(SiftMaintShmem *m, const SiftMaintHost *host, int pid, bool failed)
{
	TimestampTz now = host->now();
	int			n = 0;

	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
	{
		SiftMaintSlot *s = &m->slot[i];

		if (s->state != SIFT_SLOT_CLAIMED || s->pid != pid)
			continue;
		m->nclaimed--;
		if (failed)
		{
			s->failures++;
			s->crashes++;
		}
		sift_maint_requeue(m, s, failed ? now + sift_maint_backoff_us(m, s->failures) : 0);
		m->stats.reclaimed++;
		n++;
	}
	return n;
}

int
sift_maint_release_pid(SiftMaintShmem *m, const SiftMaintHost *host, int pid, bool failed)
{
	int			n;

	sift_maint_begin(m);
	n = release_pid_locked(m, host, pid, failed);
	sift_maint_end(m);
	return n;
}

typedef struct Pids
{
	int			n;
	int		   *pid;
} Pids;

static Pids
pids_new(void)
{
	return (Pids) {.pid = palloc((SIFT_MAINT_SLOTS + SIFT_MAINT_MAX_WORKERS) * sizeof(int))};
}

static int
pid_order(const void *a, const void *b)
{
	return (*(const int *) a > *(const int *) b) - (*(const int *) a < *(const int *) b);
}

static bool
pids_has(const Pids *p, int pid)
{
	if (p->n <= 4)
	{
		for (int k = 0; k < p->n; k++)
			if (p->pid[k] == pid)
				return true;
		return false;
	}
	return bsearch(&pid, p->pid, p->n, sizeof(int), pid_order) != NULL;
}

/* The distinct pids that run a worker or own a claim, sorted; lapsed says a worker that never started has overstayed its lease */
static void
owner_pids(SiftMaintShmem *m, TimestampTz now, Pids *p, bool *lapsed, uint32 *gen_seen)
{
	int			n = 0;

	*lapsed = false;
	sift_shm_lock(&m->lock);
	*gen_seen = m->next_gen;
	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
	{
		*lapsed |= m->worker[w].state == 1 && m->worker[w].lease_until < now;
		if (m->worker[w].state == 2)
			p->pid[n++] = m->worker[w].pid;
	}
	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
		if (m->slot[i].state == SIFT_SLOT_CLAIMED)
			p->pid[n++] = m->slot[i].pid;
	sift_shm_unlock(&m->lock);
	qsort(p->pid, n, sizeof(int), pid_order);
	p->n = 0;
	for (int k = 0; k < n; k++)
		if (p->n == 0 || p->pid[p->n - 1] != p->pid[k])
			p->pid[p->n++] = p->pid[k];
}

static int
reap_dead(SiftMaintShmem *m, TimestampTz now, const Pids *dead, uint32 gen_seen)
{
	int			n = 0;

	sift_maint_begin(m);
	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
	{
		SiftMaintWorkerSlot *ws = &m->worker[w];

		if (ws->state == 1 && ws->lease_until < now)
		{
			sift_maint_worker_free(m, w);
			m->stats.lease_lapsed++;
		}
		else if (ws->state == 2 && pids_has(dead, ws->pid))
			sift_maint_worker_free(m, w);
	}
	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
		if (m->slot[i].state == SIFT_SLOT_CLAIMED && m->slot[i].gen <= gen_seen && pids_has(dead, m->slot[i].pid))
		{
			m->nclaimed--;
			m->slot[i].failures++;
			m->slot[i].crashes++;
			sift_maint_requeue(m, &m->slot[i], now + sift_maint_backoff_us(m, m->slot[i].failures));
			m->stats.reclaimed++;
			n++;
		}
	sift_maint_end(m);
	return n;
}

/*
 * Liveness is asked once for each distinct pid and outside the lock (the host may take locks of its own); what is given back is then taken in one pass over the table. A claim made after
 * the owners were read (a generation past gen_seen) is never taken for the claim of a pid that is gone: the number may be a new process's.
 */
int
sift_maint_reap(SiftMaintShmem *m, const SiftMaintHost *host)
{
	TimestampTz now = host->now();
	Pids		owners;
	Pids		dead;
	bool		lapsed;
	uint32		gen_seen;
	int			n = 0;

	if (sift_maint_idle(m))
		return 0;
	owners = pids_new();
	dead = pids_new();
	owner_pids(m, now, &owners, &lapsed, &gen_seen);
	for (int k = 0; k < owners.n; k++)
		if (!host->pid_alive(owners.pid[k]))
			dead.pid[dead.n++] = owners.pid[k];
	if (dead.n > 0 || lapsed)
		n = reap_dead(m, now, &dead, gen_seen);
	pfree(owners.pid);
	pfree(dead.pid);
	return n;
}

void
sift_maint_heat_note(SiftMaintShmem *m, Oid rel)
{
	SIFT_SHM_BACKEND_LOCAL uint32 reads;

	if ((++reads & 127) == 0)
		pg_atomic_fetch_add_u32(&m->heat[rel % SIFT_MAINT_HEAT], 1);
}

uint32
sift_maint_heat(SiftMaintShmem *m, Oid rel)
{
	return pg_atomic_read_u32(&m->heat[rel % SIFT_MAINT_HEAT]);
}

void
sift_maint_heat_decay_due(SiftMaintShmem *m, TimestampTz now)
{
	uint64		at = pg_atomic_read_u64(&m->heat_decayed_us);
	uint64		naps = (uint64) (now - (TimestampTz) at) / ((uint64) Max(m->naptime_ms, 1) * 1000);

	if (naps == 0 || !pg_atomic_compare_exchange_u64(&m->heat_decayed_us, &at, (uint64) now))
		return;
	for (int i = 0; i < SIFT_MAINT_HEAT; i++)
		pg_atomic_write_u32(&m->heat[i], pg_atomic_read_u32(&m->heat[i]) >> Min(naps, 31));
}

void
sift_maint_fg_note(SiftMaintShmem *m, bool write)
{
	SIFT_SHM_BACKEND_LOCAL uint32 ops;

	if ((++ops & 63) == 0)
		pg_atomic_fetch_add_u64(write ? &m->fg_write_ops : &m->fg_read_ops, 1);
}

bool
sift_maint_idle(SiftMaintShmem *m)
{
	return m->nqueued == 0 && m->nclaimed == 0 && pg_atomic_read_u32(&m->active_workers) == 0;
}
