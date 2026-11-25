/*
 * core/maint/sched.c - see maint_sched.h
 */
#include "core/maint/sched.h"

#include "datatype/timestamp.h"
#include "port/pg_bitutils.h"

#define MAINT_MAGIC		0x53464D51

StaticAssertDecl(sizeof(SiftMaintSlot) == 128, "a slot is one 128 B line");
StaticAssertDecl(sizeof(SiftMaintWorkerSlot) * SIFT_MAINT_MAX_WORKERS + 64 == 2 * PG_CACHE_LINE_SIZE, "the worker table fills two lines with its pad");
StaticAssertDecl(offsetof(SiftMaintShmem, stats) == PG_CACHE_LINE_SIZE, "the counters start on line 1");
StaticAssertDecl(offsetof(SiftMaintShmem, fg_write_ops) % PG_CACHE_LINE_SIZE == 0, "the foreground counters have a line of their own");
StaticAssertDecl(offsetof(SiftMaintShmem, heat) % PG_CACHE_LINE_SIZE == 0, "the heat counters are line aligned");
StaticAssertDecl(offsetof(SiftMaintShmem, horizon) % PG_CACHE_LINE_SIZE == 0, "the horizon ring starts on a line");
StaticAssertDecl(sizeof(SiftMaintShmem) % PG_CACHE_LINE_SIZE == 0, "the table is a whole number of lines");

static const struct
{
	uint32		floor;
	uint32		width;
}			tier_range[SIFT_TIERS] = {{10000, 20000}, {1000, 9000}, {200, 800}, {50, 150}};

static const struct
{
	const char *name;
	SiftPacerTier tier;
	uint32		urgency;
}			kinds[SIFT_MK_N] = {
	[SIFT_MK_QUARANTINE_MARK] = {"quarantine", SIFT_TIER_P0, 10000},
	[SIFT_MK_SLAB_FLUSH] = {"slab_flush", SIFT_TIER_P0, 0},
	[SIFT_MK_PEND_DEL_FOLD] = {"pend_del", SIFT_TIER_P0, 0},
	[SIFT_MK_COMPACT_PART] = {"compact_l1", SIFT_TIER_P1, 0},
	[SIFT_MK_SCOPE_REFRESH] = {"scope_refresh", SIFT_TIER_P1, 0},
	[SIFT_MK_BM25_MERGE] = {"bm25_merge", SIFT_TIER_P1, 0},
	[SIFT_MK_SPLIT_PART] = {"split", SIFT_TIER_P2, 0},
	[SIFT_MK_MERGE_PART] = {"merge", SIFT_TIER_P2, 0},
	[SIFT_MK_TAIL_SHRINK] = {"tail_shrink", SIFT_TIER_P3, 0},
};

SiftPacerTier
sift_maint_kind_tier(SiftMaintKind kind)
{
	return kinds[kind].tier;
}

const char *
sift_maint_job_name(SiftMaintKind kind, SiftPacerTier tier)
{
	return kind == SIFT_MK_COMPACT_PART && tier >= SIFT_TIER_P2 ? "compact_l2" : kinds[kind].name;
}

const char *
sift_maint_phase_name(SiftMaintPhase phase)
{
	static const char *const names[] = {"queued", "scan", "write", "relocate", "truncate", "waiting_pace"};

	return names[phase];
}

SiftMaintKind
sift_maint_kind_of_name(const char *job)
{
	for (int k = 0; k < SIFT_MK_N; k++)
		if (strcmp(kinds[k].name, job) == 0)
			return (SiftMaintKind) k;
	return SIFT_MK_N;
}

uint32
sift_maint_score(SiftPacerTier tier, uint32 urgency)
{
	return tier_range[tier].floor + Min(urgency, tier_range[tier].width - 1);
}

uint32
sift_maint_aged_score(uint32 score, SiftPacerTier tier, TimestampTz enqueued_at, TimestampTz now)
{
	uint64		quanta;

	if (tier == SIFT_TIER_P0 || now <= enqueued_at)
		return score;
	quanta = (uint64) (now - enqueued_at) / SIFT_MAINT_AGE_QUANTUM_US;
	return (uint32) Max(score, Min((uint64) score + quanta * SIFT_MAINT_AGE_STEP, SIFT_MAINT_AGED_MAX));
}

static uint32
key_hash(Oid db, Oid rel, uint32 kind)
{
	uint64		x = (((uint64) db << 32) | rel) * UINT64CONST(0x9E3779B97F4A7C15) ^ ((uint64) kind + 1) * UINT64CONST(0xC2B2AE3D27D4EB4F);
	uint32		h = (uint32) (x >> 32);

	return h ? h : 1;
}

Size
sift_maint_shmem_size(void)
{
	return sizeof(SiftMaintShmem);
}

/* MEM[MEM-CORE-14]: SHM maintenance scheduler lock/queue state (inside MEM-SHM-06) */
void
sift_maint_shmem_init(SiftMaintShmem *m, int tranche_id, uint32 max_workers, uint32 naptime_ms)
{
	Assert((uintptr_t) m % PG_CACHE_LINE_SIZE == 0);
	memset(m, 0, sizeof(*m));
	sift_horizon_ring_init(&m->horizon);
	sift_shm_lock_init(&m->lock, tranche_id);
	pg_atomic_init_u32(&m->seq, 0);
	pg_atomic_init_u32(&m->active_workers, 0);
	pg_atomic_init_u32(&m->max_workers, Min(max_workers, SIFT_MAINT_MAX_WORKERS));
	pg_atomic_init_u32(&m->launcher, 0);
	pg_atomic_init_u64(&m->fg_write_ops, 0);
	pg_atomic_init_u64(&m->fg_read_ops, 0);
	for (int i = 0; i < SIFT_MAINT_HEAT; i++)
		pg_atomic_init_u32(&m->heat[i], 0);
	pg_atomic_init_u64(&m->bgw_sample_ts_us, 0);
	pg_atomic_init_u32(&m->bgw_other_cached, 0);
	memset(m->hash_idx, 0xFF, sizeof(m->hash_idx));
	memset(m->free_bits, 0xFF, sizeof(m->free_bits));
	m->naptime_ms = naptime_ms;
	m->tranche = tranche_id;
	m->magic = MAINT_MAGIC;
}

void
sift_maint_set_limits(SiftMaintShmem *m, uint32 max_workers, uint32 naptime_ms)
{
	pg_atomic_write_u32(&m->max_workers, Min(max_workers, SIFT_MAINT_MAX_WORKERS));
	m->naptime_ms = naptime_ms;
}

/* The writers hold the lock; the odd value of seq tells a snapshot that what it copies is torn */
static void
begin(SiftMaintShmem *m)
{
	sift_shm_lock(&m->lock);
	pg_atomic_fetch_add_u32(&m->seq, 1);
	pg_write_barrier();
}

static void
end(SiftMaintShmem *m)
{
	pg_write_barrier();
	pg_atomic_fetch_add_u32(&m->seq, 1);
	sift_shm_unlock(&m->lock);
}

/* The slot of a key is looked up in hash_idx (open addressing, linear probing, a removal shifts its cluster back: no tombstones, so a probe never runs long) */
static int
find(const SiftMaintShmem *m, uint32 h, const SiftMaintReq *r)
{
	for (uint32 p = h & (SIFT_MAINT_HASH_SIZE - 1); m->hash_idx[p] >= 0; p = (p + 1) & (SIFT_MAINT_HASH_SIZE - 1))
	{
		int			i = m->hash_idx[p];

		if (m->khash[i] == h && m->slot[i].db == r->db && m->slot[i].rel == r->rel && m->slot[i].kind == r->kind)
			return i;
	}
	return -1;
}

static int
first_free(const SiftMaintShmem *m)
{
	for (int w = 0; w < SIFT_MAINT_FREE_WORDS; w++)
		if (m->free_bits[w] != 0)
			return w * 64 + pg_rightmost_one_pos64(m->free_bits[w]);
	return -1;
}

/* Takes slot i for the key of hash h */
static void
slot_take(SiftMaintShmem *m, int i, uint32 h)
{
	uint32		p = h & (SIFT_MAINT_HASH_SIZE - 1);

	while (m->hash_idx[p] >= 0)
		p = (p + 1) & (SIFT_MAINT_HASH_SIZE - 1);
	m->hash_idx[p] = (int16) i;
	m->khash[i] = h;
	m->free_bits[i / 64] &= ~(UINT64CONST(1) << (i % 64));
}

/* Removes slot i from hash_idx: the entries after it in its cluster move up into the hole when their probe path crosses it */
static void
hash_remove(SiftMaintShmem *m, int i)
{
	uint32		mask = SIFT_MAINT_HASH_SIZE - 1;
	uint32		hole = m->khash[i] & mask;

	while (m->hash_idx[hole] != i)
		hole = (hole + 1) & mask;
	for (uint32 p = (hole + 1) & mask; m->hash_idx[p] >= 0; p = (p + 1) & mask)
	{
		uint32		home = m->khash[m->hash_idx[p]] & mask;

		if (((p - home) & mask) >= ((p - hole) & mask))
		{
			m->hash_idx[hole] = m->hash_idx[p];
			hole = p;
		}
	}
	m->hash_idx[hole] = -1;
}

/* The queued request that gives way to one of score: the lowest score below it, the youngest of those */
static int
eviction_victim(const SiftMaintShmem *m, uint32 score)
{
	int			victim = -1;

	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
	{
		const SiftMaintSlot *s = &m->slot[i];

		if (s->state != SIFT_SLOT_QUEUED || s->score >= score)
			continue;
		if (victim < 0 || s->score < m->slot[victim].score ||
			(s->score == m->slot[victim].score && s->enqueued_at > m->slot[victim].enqueued_at))
			victim = i;
	}
	return victim;
}

static void
slot_free(SiftMaintShmem *m, int i)
{
	hash_remove(m, i);
	memset(&m->slot[i], 0, sizeof(SiftMaintSlot));
	m->khash[i] = 0;
	m->free_bits[i / 64] |= UINT64CONST(1) << (i % 64);
}

static void
requeue(SiftMaintShmem *m, SiftMaintSlot *s, TimestampTz not_before)
{
	s->state = SIFT_SLOT_QUEUED;
	s->pid = 0;
	s->rerun = 0;
	s->phase = SIFT_PHASE_QUEUED;
	s->not_before = not_before;
	m->nqueued++;
}

static SiftPacerTier
request_tier(const SiftMaintReq *r)
{
	return r->tier == SIFT_MAINT_TIER_DEFAULT ? kinds[r->kind].tier : (SiftPacerTier) r->tier;
}

/* P1 scales its urgency by the heat of the relation: (urgency + 1) * (1 + fls(heat + 1)) - 1 */
static uint32
request_score(SiftMaintShmem *m, const SiftMaintReq *r, SiftPacerTier tier)
{
	uint32		urgency = r->urgency + kinds[r->kind].urgency;

	if (tier == SIFT_TIER_P1)
		urgency = (urgency + 1) * (2 + pg_leftmost_one_pos32(sift_maint_heat(m, r->rel) + 1)) - 1;
	return sift_maint_score(tier, urgency);
}

static SiftMaintEnq
enqueue_locked(SiftMaintShmem *m, const SiftMaintReq *r, SiftPacerTier tier, uint32 score, TimestampTz now)
{
	uint32		h = key_hash(r->db, r->rel, r->kind);
	int			i = find(m, h, r);
	SiftMaintEnq res = SIFT_ENQ_QUEUED;
	SiftMaintSlot *s;

	if (i >= 0)
	{
		s = &m->slot[i];
		m->stats.coalesced++;
		if (score > s->score)
		{
			s->score = score;
			s->tier = tier;
			m->stats.upgraded++;
		}
		if (s->part_id != r->part_id)
			s->part_id = 0;
		s->rerun = s->state == SIFT_SLOT_CLAIMED;
		return SIFT_ENQ_COALESCED;
	}
	if ((i = first_free(m)) < 0)
	{
		if ((i = eviction_victim(m, score)) < 0)
		{
			m->stats.overflowed++;
			return SIFT_ENQ_FULL;
		}
		slot_free(m, i);
		m->nqueued--;
		m->stats.evicted++;
		res = SIFT_ENQ_EVICTED;
	}
	s = &m->slot[i];
	slot_take(m, i, h);
	s->db = r->db;
	s->rel = r->rel;
	s->kind = r->kind;
	s->tier = tier;
	s->score = score;
	s->part_id = r->part_id;
	s->enqueued_at = now;
	requeue(m, s, 0);
	m->stats.enqueued++;
	return res;
}

SiftMaintEnq
sift_maint_enqueue(SiftMaintShmem *m, const SiftMaintHost *host, const SiftMaintReq *req)
{
	SiftPacerTier tier = request_tier(req);
	SiftMaintEnq res;

	if (m == NULL)
		return SIFT_ENQ_UNAVAILABLE;
	begin(m);
	res = enqueue_locked(m, req, tier, request_score(m, req, tier), host->now());
	end(m);
	return res;
}

static bool
runnable(const SiftMaintSlot *s, Oid db, bool allow_cold, TimestampTz now)
{
	return s->state == SIFT_SLOT_QUEUED && s->not_before <= now && (!OidIsValid(db) || s->db == db) &&
		(allow_cold || s->tier <= SIFT_TIER_P1);
}

static int
worker_for(const SiftMaintShmem *m, Oid db)
{
	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
		if (m->worker[w].state != 0 && m->worker[w].db == db)
			return w;
	return -1;
}

typedef enum PickMode
{
	PICK_BEST,					/* the highest aged score, the older first */
	PICK_FAIR,					/* the oldest request that has aged a quantum, of any tier */
	PICK_UNSERVED,				/* as PICK_BEST, among the databases that have no worker and are not held or backing off */
	PICK_STARVED				/* the oldest request, among those databases, that waited SIFT_MAINT_STARVED_NAPS naps; db is the one that does not count (the one being served) */
} PickMode;

static int
active_dbs(const SiftMaintShmem *m, Oid dbs[SIFT_MAINT_MAX_WORKERS])
{
	int			n = 0;

	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
		if (m->worker[w].state != 0)
			dbs[n++] = m->worker[w].db;
	return n;
}

static bool
served(const Oid *dbs, int ndbs, Oid db)
{
	for (int k = 0; k < ndbs; k++)
		if (dbs[k] == db)
			return true;
	return false;
}

static int
pick(const SiftMaintShmem *m, Oid db, bool allow_cold, TimestampTz now, PickMode mode)
{
	int			best = -1;
	uint32		best_score = 0;
	Oid			dbs[SIFT_MAINT_MAX_WORKERS];
	bool		spawn = mode == PICK_UNSERVED || mode == PICK_STARVED;
	int			ndbs = spawn ? active_dbs(m, dbs) : 0;

	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
	{
		const SiftMaintSlot *s = &m->slot[i];
		uint32		score;

		if (m->khash[i] == 0 || !runnable(s, mode == PICK_STARVED ? InvalidOid : db, allow_cold, now))
			continue;
		if (spawn && (served(dbs, ndbs, s->db) || sift_maint_db_held(m, s->db, now) || (mode == PICK_STARVED && s->db == db)))
			continue;
		if (mode == PICK_STARVED)
		{
			if (now - s->enqueued_at >= SIFT_MAINT_STARVED_NAPS * (TimestampTz) m->naptime_ms * 1000 && (best < 0 || s->enqueued_at < m->slot[best].enqueued_at))
				best = i;
			continue;
		}
		if (mode == PICK_FAIR)
		{
			if (now - s->enqueued_at >= SIFT_MAINT_AGE_QUANTUM_US && (best < 0 || s->enqueued_at < m->slot[best].enqueued_at))
				best = i;
			continue;
		}
		score = sift_maint_aged_score(s->score, (SiftPacerTier) s->tier, s->enqueued_at, now);
		if (best < 0 || score > best_score || (score == best_score && s->enqueued_at < m->slot[best].enqueued_at))
		{
			best = i;
			best_score = score;
		}
	}
	return best;
}

bool
sift_maint_claim(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, bool allow_cold, SiftMaintClaim *out)
{
	TimestampTz now = host->now();
	SiftMaintSlot *s;
	int			i;
	bool		fair;

	sift_shm_lock(&m->lock);
	fair = (m->claim_seq + 1) % SIFT_MAINT_FAIR_EVERY == 0;
	i = fair ? pick(m, db, allow_cold, now, PICK_FAIR) : -1;
	if (i < 0)
		i = pick(m, db, allow_cold, now, PICK_BEST);
	else
		m->stats.fair_claims++;
	if (i < 0)
	{
		sift_shm_unlock(&m->lock);
		return false;
	}
	pg_atomic_fetch_add_u32(&m->seq, 1);
	pg_write_barrier();
	s = &m->slot[i];
	s->state = SIFT_SLOT_CLAIMED;
	s->pid = host->self_pid();
	s->gen = ++m->next_gen;
	s->claimed_at = now;
	s->phase = SIFT_PHASE_SCAN;
	s->done = s->total = s->bytes_written = 0;
	m->nqueued--;
	m->nclaimed++;
	m->claim_seq++;
	m->stats.claimed++;
	*out = (SiftMaintClaim) {i, s->gen, s->db, s->rel, (SiftMaintKind) s->kind, (SiftPacerTier) s->tier, s->part_id, s->score,
		s->failures, s->enqueued_at};
	end(m);
	return true;
}

static TimestampTz
failure_backoff_us(const SiftMaintShmem *m, uint32 failures)
{
	uint64		ms = Min(((uint64) Max(m->naptime_ms, 1)) << Min(failures, 24), SIFT_MAINT_BACKOFF_MAX_MS);

	return (TimestampTz) ms * 1000;
}

static bool
owns(const SiftMaintSlot *s, const SiftMaintClaim *c, int pid)
{
	return s->state == SIFT_SLOT_CLAIMED && s->gen == c->gen && s->pid == pid;
}

bool
sift_maint_complete(SiftMaintShmem *m, const SiftMaintHost *host, const SiftMaintClaim *claim, SiftMaintStep result)
{
	TimestampTz now = host->now();
	SiftMaintSlot *s = &m->slot[claim->slot];

	begin(m);
	if (!owns(s, claim, host->self_pid()))
	{
		end(m);
		return false;
	}
	m->nclaimed--;
	m->stats.completed++;
	switch (result)
	{
		case SIFT_MSTEP_DONE:
			s->failures = s->crashes = 0;
			if (!s->rerun)
			{
				slot_free(m, claim->slot);
				break;
			}
			s->enqueued_at = now;
			requeue(m, s, 0);
			break;
		case SIFT_MSTEP_MORE:
			s->failures = s->crashes = 0;
			requeue(m, s, 0);
			break;
		case SIFT_MSTEP_BUSY:
			requeue(m, s, now + SIFT_MAINT_BUSY_US);
			break;
		case SIFT_MSTEP_DEFERRED:
			requeue(m, s, now + SIFT_MAINT_DEFER_US);
			break;
		case SIFT_MSTEP_FAILED:
			s->failures++;
			requeue(m, s, now + failure_backoff_us(m, s->failures));
			break;
		case SIFT_MSTEP_GONE:
			slot_free(m, claim->slot);
			break;
	}
	end(m);
	return true;
}

void
sift_maint_progress(SiftMaintShmem *m, const SiftMaintClaim *claim, SiftMaintPhase phase, uint64 done, uint64 total, uint64 bytes)
{
	SiftMaintSlot *s = &m->slot[claim->slot];

	begin(m);
	if (s->state == SIFT_SLOT_CLAIMED && s->gen == claim->gen)
	{
		s->phase = phase;
		s->done = done;
		s->total = total;
		s->bytes_written = bytes;
	}
	end(m);
}

/* A request of another database has waited SIFT_MAINT_STARVED_NAPS naps without a worker: the worker that serves db leaves and the next one goes to that database */
static bool
starved_elsewhere(SiftMaintShmem *m, const SiftMaintHost *host, Oid db)
{
	TimestampTz now = host->now();
	bool		starved;

	sift_shm_lock(&m->lock);
	starved = pick(m, db, !host->defer_cold(), now, PICK_STARVED) >= 0;
	sift_shm_unlock(&m->lock);
	return starved;
}

int
sift_maint_run(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, int max_steps, SiftMaintStepFn step, void *arg, bool *yielded)
{
	SiftMaintClaim c;
	int			n = 0;

	*yielded = false;
	while (n < max_steps && !*yielded && sift_maint_claim(m, host, db, !host->defer_cold(), &c))
	{
		sift_maint_complete(m, host, &c, step(&c, arg));
		if (host->pace != NULL)
			host->pace(c.tier);
		*yielded = host->should_yield(c.tier, host->now()) || starved_elsewhere(m, host, db);
		n++;
	}
	return n;
}

bool
sift_maint_has_work(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, bool allow_cold)
{
	TimestampTz now = host->now();
	bool		found;

	sift_shm_lock(&m->lock);
	found = pick(m, db, allow_cold, now, PICK_BEST) >= 0;
	sift_shm_unlock(&m->lock);
	return found;
}

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
		requeue(m, s, failed ? now + failure_backoff_us(m, s->failures) : 0);
		m->stats.reclaimed++;
		n++;
	}
	return n;
}

int
sift_maint_release_pid(SiftMaintShmem *m, const SiftMaintHost *host, int pid, bool failed)
{
	int			n;

	begin(m);
	n = release_pid_locked(m, host, pid, failed);
	end(m);
	return n;
}

static void
worker_free(SiftMaintShmem *m, int w)
{
	memset(&m->worker[w], 0, sizeof(SiftMaintWorkerSlot));
	pg_atomic_fetch_sub_u32(&m->active_workers, 1);
}

typedef struct Pids
{
	int			n;
	int			pid[SIFT_MAINT_SLOTS + SIFT_MAINT_MAX_WORKERS];
} Pids;

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

/*
 * Liveness is asked once for each distinct pid and outside the lock (the host may take locks of its own); what is given back is then taken in one pass over the table. A claim made after
 * the owners were read (a generation past gen_seen) is never taken for the claim of a pid that is gone: the number may be a new process's.
 */
int
sift_maint_reap(SiftMaintShmem *m, const SiftMaintHost *host)
{
	TimestampTz now = host->now();
	Pids		owners;
	Pids		dead = {0};
	bool		lapsed;
	uint32		gen_seen;
	int			n = 0;

	owner_pids(m, now, &owners, &lapsed, &gen_seen);
	for (int k = 0; k < owners.n; k++)
		if (!host->pid_alive(owners.pid[k]))
			dead.pid[dead.n++] = owners.pid[k];
	if (dead.n == 0 && !lapsed)
		return 0;
	begin(m);
	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
	{
		SiftMaintWorkerSlot *ws = &m->worker[w];

		if (ws->state == 1 && ws->lease_until < now)
		{
			worker_free(m, w);
			m->stats.lease_lapsed++;
		}
		else if (ws->state == 2 && pids_has(&dead, ws->pid))
			worker_free(m, w);
	}
	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
		if (m->slot[i].state == SIFT_SLOT_CLAIMED && m->slot[i].gen <= gen_seen && pids_has(&dead, m->slot[i].pid))
		{
			m->nclaimed--;
			m->slot[i].failures++;
			m->slot[i].crashes++;
			requeue(m, &m->slot[i], now + failure_backoff_us(m, m->slot[i].failures));
			m->stats.reclaimed++;
			n++;
		}
	end(m);
	return n;
}

int
sift_maint_effective_max(int configured, int max_worker_processes, int bgworkers_other)
{
	int			reserve = Max(2, max_worker_processes / 4);

	return Max(0, Min(Min(configured, SIFT_MAINT_MAX_WORKERS), max_worker_processes - reserve - bgworkers_other));
}

SiftMaintMode
sift_maint_mode(bool async_maintenance, bool preloaded, bool table_ok, int effective_max)
{
	if (!async_maintenance || effective_max <= 0)
		return SIFT_MAINT_MODE_OFF;
	if (!table_ok)
		return SIFT_MAINT_MODE_LOCAL;
	return preloaded ? SIFT_MAINT_MODE_LAUNCHER : SIFT_MAINT_MODE_DYNAMIC;
}

static int
workers_taken(const SiftMaintShmem *m)
{
	int			n = 0;

	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
		n += m->worker[w].state != 0;
	return n;
}

static int
reserve_locked(SiftMaintShmem *m, Oid db, TimestampTz now, int effective_max)
{
	if (workers_taken(m) >= Min(effective_max, (int) pg_atomic_read_u32(&m->max_workers)))
		return -1;
	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
	{
		if (m->worker[w].state != 0)
			continue;
		m->worker[w] = (SiftMaintWorkerSlot) {.state = 1, .db = db, .lease_until = now + SIFT_MAINT_LEASE_US};
		pg_atomic_fetch_add_u32(&m->active_workers, 1);
		return w;
	}
	return -1;
}

/*
 * The workers of the other pool count against the same sift.max_maint_workers. Each pool takes its slot and then reads the count of the other, so of two that take theirs together at least
 * one sees both and gives its own back: the limit is never passed, even for a moment. The caller holds no lock of the table
 */
static bool
reserve_over_shared_limit(SiftMaintShmem *m, const SiftMaintHost *host, int w)
{
	bool		over = (int) pg_atomic_read_u32(&m->active_workers) + (host->other_workers != NULL ? host->other_workers() : 0) > (int) pg_atomic_read_u32(&m->max_workers);

	if (over)
	{
		begin(m);
		worker_free(m, w);
		end(m);
	}
	return over;
}

/* The cap on workers, asked of the host only when a worker is to be started */
static int
effective_max_of(SiftMaintShmem *m, const SiftMaintHost *host)
{
	return host->effective_max != NULL ? host->effective_max(m) : (int) pg_atomic_read_u32(&m->max_workers);
}

int
sift_maint_worker_reserve(SiftMaintShmem *m, const SiftMaintHost *host, Oid db)
{
	TimestampTz now = host->now();
	int			cap = effective_max_of(m, host);
	int			w;

	begin(m);
	w = reserve_locked(m, db, now, cap);
	end(m);
	return w >= 0 && reserve_over_shared_limit(m, host, w) ? -1 : w;
}

bool
sift_maint_worker_started(SiftMaintShmem *m, int worker, int pid, int procno)
{
	SiftMaintWorkerSlot *ws = &m->worker[worker];
	bool		ok;

	begin(m);
	ok = ws->state == 1 || ws->state == 0;
	if (ok)
	{
		if (ws->state == 0)
			pg_atomic_fetch_add_u32(&m->active_workers, 1);
		ws->state = 2;
		ws->pid = pid;
		ws->procno = procno;
	}
	end(m);
	return ok;
}

void
sift_maint_worker_exit(SiftMaintShmem *m, int worker, int pid)
{
	SiftMaintWorkerSlot *ws = &m->worker[worker];

	begin(m);
	if (ws->state == 2 && ws->pid == pid)
	{
		for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
			if (m->slot[i].state == SIFT_SLOT_CLAIMED && m->slot[i].pid == pid)
			{
				m->nclaimed--;
				requeue(m, &m->slot[i], 0);
				m->stats.reclaimed++;
			}
		worker_free(m, worker);
	}
	end(m);
}

void
sift_maint_worker_unconnected(SiftMaintShmem *m, const SiftMaintHost *host, int worker, Oid db)
{
	TimestampTz now = host->now();

	begin(m);
	if (m->worker[worker].state == 1 && m->worker[worker].db == db)
		worker_free(m, worker);
	end(m);
	if (sift_maint_db_connect_failed(m, now, db) >= SIFT_MAINT_CONNECT_GIVE_UP)
		sift_maint_db_purge(m, db);
}

void
sift_maint_db_purge(SiftMaintShmem *m, Oid db)
{
	begin(m);
	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
		if (m->khash[i] != 0 && m->slot[i].state == SIFT_SLOT_QUEUED && m->slot[i].db == db)
		{
			m->nqueued--;
			slot_free(m, i);
		}
	end(m);
}

/* Wakes the worker of the database that has the best request waiting and starts one for a database that has none: the cap on workers is asked of the host only when that is to be done */
SiftMaintKick
sift_maint_kick(SiftMaintShmem *m, const SiftMaintHost *host)
{
	TimestampTz now = host->now();
	bool		cold_ok = !host->defer_cold();
	SiftPacerTier tier = SIFT_TIER_P0;
	SiftMaintKick res = SIFT_KICK_WOKE;
	Oid			db = InvalidOid;
	int			i, j, w = -1, procno = -1, cap;

	begin(m);
	if ((i = pick(m, InvalidOid, cold_ok, now, PICK_BEST)) < 0)
	{
		res = !cold_ok && pick(m, InvalidOid, true, now, PICK_BEST) >= 0 ? SIFT_KICK_DEFERRED : SIFT_KICK_NONE;
		end(m);
		return res;
	}
	if ((j = worker_for(m, m->slot[i].db)) >= 0 && m->worker[j].state == 2)
		procno = m->worker[j].procno;
	if ((j = pick(m, InvalidOid, cold_ok, now, PICK_STARVED)) < 0)
		j = pick(m, InvalidOid, cold_ok, now, PICK_UNSERVED);
	if (j >= 0)
	{
		db = m->slot[j].db;
		tier = (SiftPacerTier) m->slot[j].tier;
	}
	end(m);
	if (j >= 0)
	{
		cap = effective_max_of(m, host);
		begin(m);
		res = worker_for(m, db) >= 0 ? SIFT_KICK_WOKE : (w = reserve_locked(m, db, now, cap)) >= 0 ? SIFT_KICK_SPAWNED : SIFT_KICK_NOSLOT;
		end(m);
	}
	if (res == SIFT_KICK_SPAWNED && reserve_over_shared_limit(m, host, w))
		res = SIFT_KICK_NOSLOT;
	if (procno >= 0)
		host->wake(procno);
	if (res == SIFT_KICK_NOSLOT)
		host->note_starved(tier, now);
	if (res != SIFT_KICK_SPAWNED)
		return res;
	if (host->spawn(db, w))
	{
		begin(m);
		m->stats.spawned++;
		end(m);
		return res;
	}
	begin(m);
	worker_free(m, w);
	m->stats.spawn_failed++;
	end(m);
	host->note_starved(tier, now);
	return SIFT_KICK_NOSLOT;
}

SiftMaintEnq
sift_maint_request(SiftMaintShmem *m, const SiftMaintHost *host, SiftMaintMode mode, const SiftMaintReq *req)
{
	SiftMaintEnq res = sift_maint_enqueue(m, host, req);
	uint32		launcher;

	if ((res != SIFT_ENQ_QUEUED && res != SIFT_ENQ_EVICTED) || mode == SIFT_MAINT_MODE_OFF || mode == SIFT_MAINT_MODE_LOCAL)
		return res;
	launcher = mode == SIFT_MAINT_MODE_LAUNCHER ? pg_atomic_read_u32(&m->launcher) : 0;
	if (launcher != 0)
		host->wake((int) launcher - 1);
	else
		sift_maint_kick(m, host);
	return res;
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
sift_maint_heat_decay(SiftMaintShmem *m)
{
	for (int i = 0; i < SIFT_MAINT_HEAT; i++)
		pg_atomic_write_u32(&m->heat[i], pg_atomic_read_u32(&m->heat[i]) >> 1);
}

void
sift_maint_fg_note(SiftMaintShmem *m, bool write)
{
	SIFT_SHM_BACKEND_LOCAL uint32 ops;

	if ((++ops & 63) == 0)
		pg_atomic_fetch_add_u64(write ? &m->fg_write_ops : &m->fg_read_ops, 1);
}
