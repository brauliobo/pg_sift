/*
 * core/maint/sched.c - see maint_sched.h
 */
#include "core/maint/sched.h"

#include "datatype/timestamp.h"
#include "port/pg_bitutils.h"

#define MAINT_MAGIC		0x53464D51

StaticAssertDecl(sizeof(SiftMaintSlot) == 128, "a slot is one 128 B line");
StaticAssertDecl(offsetof(SiftMaintShmem, horizon) - offsetof(SiftMaintShmem, worker) == 4 * PG_CACHE_LINE_SIZE, "the worker table fills four lines with its pad");
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
	for (int i = 0; i < SIFT_MAINT_COALESCED_SHARDS; i++)
		pg_atomic_init_u64(&m->coalesced[i].n, 0);
	for (int i = 0; i < SIFT_MAINT_HEAT; i++)
		pg_atomic_init_u32(&m->heat[i], 0);
	pg_atomic_init_u64(&m->bgw_sample_ts_us, 0);
	pg_atomic_init_u32(&m->bgw_other_cached, 0);
	pg_atomic_init_u64(&m->heat_decayed_us, 0);
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
void
sift_maint_begin(SiftMaintShmem *m)
{
	sift_shm_lock(&m->lock);
	pg_atomic_write_u32(&m->seq, pg_atomic_read_u32(&m->seq) + 1);
	pg_write_barrier();
}

void
sift_maint_end(SiftMaintShmem *m)
{
	pg_write_barrier();
	pg_atomic_write_u32(&m->seq, pg_atomic_read_u32(&m->seq) + 1);
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

void
sift_maint_requeue(SiftMaintShmem *m, SiftMaintSlot *s, TimestampTz not_before)
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

/* A request the full queue refused waits in refused[] once per key (the oldest one gives way to a newer one when that is full too) */
static void
refuse(SiftMaintShmem *m, const SiftMaintReq *r, SiftPacerTier tier, uint32 score, TimestampTz now)
{
	for (uint32 k = 0; k < m->nrefused; k++)
		if (m->refused[k].db == r->db && m->refused[k].rel == r->rel && m->refused[k].kind == r->kind)
			return;
	if (m->nrefused == SIFT_MAINT_REFUSED)
		memmove(&m->refused[0], &m->refused[1], (SIFT_MAINT_REFUSED - 1) * sizeof(SiftMaintRefused));
	else
		m->nrefused++;
	m->refused[m->nrefused - 1] = (SiftMaintRefused) {.db = r->db,.rel = r->rel,.kind = r->kind,.tier = tier,.score = score,.part_id = r->part_id,.refused_at = now};
}

static SiftMaintEnq enqueue_locked(SiftMaintShmem *m, const SiftMaintReq *r, SiftPacerTier tier, uint32 score, TimestampTz now);

/* The refused requests take the slots that are free again, the newest first */
static void
requeue_refused(SiftMaintShmem *m)
{
	while (m->nrefused > 0 && first_free(m) >= 0)
	{
		const SiftMaintRefused *f = &m->refused[--m->nrefused];
		SiftMaintReq r = {.db = f->db,.rel = f->rel,.kind = f->kind,.tier = f->tier,.part_id = f->part_id};

		(void) enqueue_locked(m, &r, (SiftPacerTier) f->tier, f->score, f->refused_at);
	}
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
		res = score > s->score && tier < (SiftPacerTier) s->tier ? SIFT_ENQ_UPGRADED : SIFT_ENQ_COALESCED;
		if (score > s->score)
		{
			s->score = score;
			s->tier = tier;
			m->stats.upgraded++;
		}
		if (s->part_id != r->part_id)
			s->part_id = 0;
		s->rerun = s->state == SIFT_SLOT_CLAIMED;
		return res;
	}
	if ((i = first_free(m)) < 0)
	{
		if ((i = eviction_victim(m, score)) < 0)
		{
			m->stats.overflowed++;
			refuse(m, r, tier, score, now);
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
	sift_maint_requeue(m, s, 0);
	m->stats.enqueued++;
	return res;
}

/* A request that a queued one already covers (its score is as high, its partition the same) changes nothing: it is found by a read of the table that no writer was changing, and counted on a line of its own */
static bool
covered(SiftMaintShmem *m, const SiftMaintReq *r, uint32 score)
{
	uint32		seq = pg_atomic_read_u32(&m->seq);
	bool		hit;
	int			i;

	if (seq & 1)
		return false;
	pg_read_barrier();
	i = find(m, key_hash(r->db, r->rel, r->kind), r);
	hit = i >= 0 && m->slot[i].state == SIFT_SLOT_QUEUED && m->slot[i].score >= score && m->slot[i].part_id == r->part_id;
	pg_read_barrier();
	if (!hit || pg_atomic_read_u32(&m->seq) != seq)
		return false;
	pg_atomic_fetch_add_u64(&m->coalesced[sift_shm_my_procno() % SIFT_MAINT_COALESCED_SHARDS].n, 1);
	return true;
}

SiftMaintEnq
sift_maint_enqueue(SiftMaintShmem *m, const SiftMaintHost *host, const SiftMaintReq *req)
{
	SiftPacerTier tier = request_tier(req);
	TimestampTz now = host->now();
	SiftMaintEnq res;
	uint32		score;

	if (m == NULL)
		return SIFT_ENQ_UNAVAILABLE;
	sift_maint_heat_decay_due(m, now);
	score = request_score(m, req, tier);
	if (covered(m, req, score))
		return SIFT_ENQ_COALESCED;
	sift_maint_begin(m);
	res = enqueue_locked(m, req, tier, score, now);
	sift_maint_end(m);
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
	PICK_UNSERVED,				/* as PICK_BEST, among the databases that have no worker and are not held or backing off: a worker with nothing to do leaves for them */
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

/* A bit for each database (by its oid mod 64) that may be held or backing off now: the scan of the slots asks sift_maint_db_held only of those */
static uint64
held_mask(const SiftMaintShmem *m, TimestampTz now)
{
	uint64		mask = 0;

	for (int i = 0; i < SIFT_MAINT_DBSTATE; i++)
		if (sift_maint_db_state_blocks(&m->dbstate[i], now))
			mask |= UINT64CONST(1) << (m->dbstate[i].db % 64);
	return mask;
}

static bool
held(const SiftMaintShmem *m, uint64 mask, Oid db, TimestampTz now)
{
	return (mask & (UINT64CONST(1) << (db % 64))) != 0 && sift_maint_db_held(m, db, now);
}

static int
pick(const SiftMaintShmem *m, Oid db, bool allow_cold, TimestampTz now, PickMode mode)
{
	int			best = -1;
	uint32		best_score = 0;
	Oid			dbs[SIFT_MAINT_MAX_WORKERS];
	bool		spawn = mode == PICK_UNSERVED || mode == PICK_STARVED;
	int			ndbs = spawn ? active_dbs(m, dbs) : 0;
	uint64		hmask = spawn ? held_mask(m, now) : 0;

	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
	{
		const SiftMaintSlot *s = &m->slot[i];
		uint32		score;

		if (m->khash[i] == 0 || !runnable(s, mode == PICK_STARVED ? InvalidOid : db, allow_cold, now))
			continue;
		if (spawn && (served(dbs, ndbs, s->db) || held(m, hmask, s->db, now) || (mode == PICK_STARVED && s->db == db)))
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
	sift_maint_end(m);
	return true;
}

TimestampTz
sift_maint_backoff_us(const SiftMaintShmem *m, uint32 failures)
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

	sift_maint_begin(m);
	if (!owns(s, claim, host->self_pid()))
	{
		sift_maint_end(m);
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
			sift_maint_requeue(m, s, 0);
			break;
		case SIFT_MSTEP_MORE:
			s->failures = s->crashes = 0;
			sift_maint_requeue(m, s, 0);
			break;
		case SIFT_MSTEP_BUSY:
			sift_maint_requeue(m, s, now + SIFT_MAINT_BUSY_US);
			break;
		case SIFT_MSTEP_DEFERRED:
			sift_maint_requeue(m, s, now + SIFT_MAINT_DEFER_US);
			break;
		case SIFT_MSTEP_FAILED:
			s->failures++;
			sift_maint_requeue(m, s, now + sift_maint_backoff_us(m, s->failures));
			break;
		case SIFT_MSTEP_GONE:
			slot_free(m, claim->slot);
			break;
	}
	requeue_refused(m);
	sift_maint_end(m);
	return true;
}

void
sift_maint_progress(SiftMaintShmem *m, const SiftMaintClaim *claim, SiftMaintPhase phase, uint64 done, uint64 total, uint64 bytes)
{
	SiftMaintSlot *s = &m->slot[claim->slot];

	sift_maint_begin(m);
	if (s->state == SIFT_SLOT_CLAIMED && s->gen == claim->gen)
	{
		s->phase = phase;
		s->done = done;
		s->total = total;
		s->bytes_written = bytes;
	}
	sift_maint_end(m);
}

static bool
has_cold(const SiftMaintShmem *m, Oid db, TimestampTz now)
{
	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
		if (m->khash[i] != 0 && m->slot[i].tier > SIFT_TIER_P1 && runnable(&m->slot[i], db, true, now))
			return true;
	return false;
}

/* The pacer is asked whether P2 and P3 wait only when one of them is there to wait: a table with none is no load for it to measure */
static bool
cold_allowed(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, TimestampTz now)
{
	bool		cold;

	sift_shm_lock(&m->lock);
	cold = has_cold(m, db, now);
	sift_shm_unlock(&m->lock);
	return !cold || !host->defer_cold();
}

/* A request of another database has waited SIFT_MAINT_STARVED_NAPS naps without a worker (waited: no wait at all, for a worker that has nothing to do): the worker that serves db leaves and the next one goes to that database */
static bool
other_db_waits(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, PickMode mode)
{
	TimestampTz now = host->now();
	bool		cold_ok = cold_allowed(m, host, InvalidOid, now);
	bool		waits;

	sift_shm_lock(&m->lock);
	waits = pick(m, db, cold_ok, now, mode) >= 0;
	sift_shm_unlock(&m->lock);
	return waits;
}

int
sift_maint_run(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, int max_steps, SiftMaintStepFn step, void *arg, bool *yielded)
{
	SiftMaintClaim c;
	int			n = 0;

	*yielded = false;
	while (n < max_steps && !*yielded && sift_maint_claim(m, host, db, cold_allowed(m, host, db, host->now()), &c))
	{
		sift_maint_complete(m, host, &c, step(&c, arg));
		if (host->pace != NULL)
			host->pace(c.tier);
		*yielded = host->should_yield(c.tier, host->now()) || other_db_waits(m, host, db, PICK_STARVED);
		n++;
	}
	if (n == 0)
		*yielded = other_db_waits(m, host, InvalidOid, PICK_UNSERVED);
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

/* A request of the database is queued to run within SIFT_MAINT_DEFER_US (a step that found its lock busy or deferred itself): a dynamic worker that left now would leave it to no one */
bool
sift_maint_due_soon(SiftMaintShmem *m, const SiftMaintHost *host, Oid db)
{
	TimestampTz horizon = host->now() + SIFT_MAINT_DEFER_US;
	bool		found = false;

	sift_shm_lock(&m->lock);
	for (int i = 0; i < SIFT_MAINT_SLOTS && !found; i++)
		found = m->khash[i] != 0 && runnable(&m->slot[i], db, true, horizon);
	sift_shm_unlock(&m->lock);
	return found;
}

void
sift_maint_worker_free(SiftMaintShmem *m, int w)
{
	m->worker[w] = (SiftMaintWorkerSlot) {.gen = m->worker[w].gen};
	pg_atomic_fetch_sub_u32(&m->active_workers, 1);
}

int
sift_maint_effective_max(int configured, int max_worker_processes, int bgworkers_other)
{
	int			reserve = Max(2, max_worker_processes / 4);

	return Max(0, Min(Min(sift_maint_workers_cap(configured, max_worker_processes), SIFT_MAINT_MAX_WORKERS), max_worker_processes - reserve - bgworkers_other));
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

static SiftMaintTicket
reserve_locked(SiftMaintShmem *m, Oid db, TimestampTz now, int effective_max)
{
	if (workers_taken(m) >= Min(effective_max, (int) pg_atomic_read_u32(&m->max_workers)))
		return (SiftMaintTicket) {-1};
	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
	{
		if (m->worker[w].state != 0)
			continue;
		m->worker[w] = (SiftMaintWorkerSlot) {.state = 1, .gen = m->worker[w].gen + 1, .db = db, .lease_until = now + SIFT_MAINT_LEASE_US};
		pg_atomic_fetch_add_u32(&m->active_workers, 1);
		return (SiftMaintTicket) {w, m->worker[w].gen};
	}
	return (SiftMaintTicket) {-1};
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
		sift_maint_begin(m);
		sift_maint_worker_free(m, w);
		sift_maint_end(m);
	}
	return over;
}

/* The cap on workers, asked of the host only when a worker is to be started */
static int
effective_max_of(SiftMaintShmem *m, const SiftMaintHost *host)
{
	return host->effective_max != NULL ? host->effective_max(m) : (int) pg_atomic_read_u32(&m->max_workers);
}

SiftMaintTicket
sift_maint_worker_reserve(SiftMaintShmem *m, const SiftMaintHost *host, Oid db)
{
	TimestampTz now = host->now();
	int			cap = effective_max_of(m, host);
	SiftMaintTicket t;

	sift_maint_begin(m);
	t = reserve_locked(m, db, now, cap);
	sift_maint_end(m);
	return t.worker >= 0 && reserve_over_shared_limit(m, host, t.worker) ? (SiftMaintTicket) {-1} : t;
}

bool
sift_maint_worker_started(SiftMaintShmem *m, SiftMaintTicket t, Oid db, int pid, int procno)
{
	SiftMaintWorkerSlot *ws = &m->worker[t.worker];
	bool		ok;

	sift_maint_begin(m);
	ok = ws->gen == t.gen && ws->state != 2;
	if (ok && ws->state == 0)
		pg_atomic_fetch_add_u32(&m->active_workers, 1);
	if (ok)
		*ws = (SiftMaintWorkerSlot) {.state = 2, .gen = t.gen, .pid = pid, .procno = procno, .db = db};
	sift_maint_end(m);
	return ok;
}

void
sift_maint_worker_exit(SiftMaintShmem *m, int worker, int pid)
{
	SiftMaintWorkerSlot *ws = &m->worker[worker];

	sift_maint_begin(m);
	if (ws->state == 2 && ws->pid == pid)
	{
		for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
			if (m->slot[i].state == SIFT_SLOT_CLAIMED && m->slot[i].pid == pid)
			{
				m->nclaimed--;
				sift_maint_requeue(m, &m->slot[i], 0);
				m->stats.reclaimed++;
			}
		sift_maint_worker_free(m, worker);
	}
	sift_maint_end(m);
}

void
sift_maint_worker_unconnected(SiftMaintShmem *m, const SiftMaintHost *host, SiftMaintTicket t, Oid db)
{
	TimestampTz now = host->now();

	sift_maint_begin(m);
	if (m->worker[t.worker].state == 1 && m->worker[t.worker].gen == t.gen)
		sift_maint_worker_free(m, t.worker);
	sift_maint_end(m);
	if (sift_maint_db_connect_failed(m, now, db) >= SIFT_MAINT_CONNECT_GIVE_UP)
		sift_maint_db_purge(m, db);
}

/* The queued requests of db (of its relation rel, when valid) and the refused ones are dropped; the slots go to the requests refused for other relations */
static void
purge(SiftMaintShmem *m, Oid db, Oid rel)
{
	uint32		kept = 0;

	sift_maint_begin(m);
	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
		if (m->khash[i] != 0 && m->slot[i].state == SIFT_SLOT_QUEUED && m->slot[i].db == db && (!OidIsValid(rel) || m->slot[i].rel == rel))
		{
			m->nqueued--;
			slot_free(m, i);
		}
	for (uint32 k = 0; k < m->nrefused; k++)
		if (m->refused[k].db != db || (OidIsValid(rel) && m->refused[k].rel != rel))
			m->refused[kept++] = m->refused[k];
	m->nrefused = kept;
	requeue_refused(m);
	sift_maint_end(m);
}

void
sift_maint_db_purge(SiftMaintShmem *m, Oid db)
{
	purge(m, db, InvalidOid);
}

void
sift_maint_rel_purge(SiftMaintShmem *m, Oid db, Oid rel)
{
	purge(m, db, rel);
}

/* The workers of db, starting ones included */
static int
workers_of(const SiftMaintShmem *m, Oid db)
{
	int			n = 0;

	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
		n += m->worker[w].state != 0 && m->worker[w].db == db;
	return n;
}

/* Does db have more runnable requests than workers, and is it neither held nor backing off: another worker of its own has something to claim */
static bool
needs_worker(const SiftMaintShmem *m, Oid db, bool allow_cold, TimestampTz now)
{
	int			runnable_n = 0,
				workers = workers_of(m, db);

	if (sift_maint_db_held(m, db, now))
		return false;
	for (int i = 0; i < SIFT_MAINT_SLOTS && runnable_n <= workers; i++)
		runnable_n += m->khash[i] != 0 && runnable(&m->slot[i], db, allow_cold, now);
	return runnable_n > workers;
}

/*
 * Wakes the worker of the database that has the best request waiting and starts one for a database that has none, or, when every database that waits has its worker, another one for the database
 * of the best request while it has more runnable requests than workers (no slot for that one is no starvation: the database is served). The cap on workers is asked of the host only when that is
 * to be done. The workers that are gone are given back first: a server without a launcher has nothing else that looks
 */
SiftMaintKick
sift_maint_kick(SiftMaintShmem *m, const SiftMaintHost *host)
{
	TimestampTz now = host->now();
	bool		cold_ok;
	SiftPacerTier tier = SIFT_TIER_P0;
	SiftMaintKick res = SIFT_KICK_WOKE;
	Oid			db = InvalidOid;
	bool		busy = false;
	SiftMaintTicket t = {-1};
	int			i, j, procno = -1, cap;

	sift_maint_reap(m, host);
	cold_ok = cold_allowed(m, host, InvalidOid, now);
	sift_maint_begin(m);
	if ((i = pick(m, InvalidOid, cold_ok, now, PICK_BEST)) < 0)
	{
		res = !cold_ok && pick(m, InvalidOid, true, now, PICK_BEST) >= 0 ? SIFT_KICK_DEFERRED : SIFT_KICK_NONE;
		sift_maint_end(m);
		return res;
	}
	if ((j = worker_for(m, m->slot[i].db)) >= 0 && m->worker[j].state == 2)
		procno = m->worker[j].procno;
	if ((j = pick(m, InvalidOid, cold_ok, now, PICK_STARVED)) < 0 && (j = pick(m, InvalidOid, cold_ok, now, PICK_UNSERVED)) < 0)
		j = (busy = needs_worker(m, m->slot[i].db, cold_ok, now)) ? i : -1;
	if (j >= 0)
	{
		db = m->slot[j].db;
		tier = (SiftPacerTier) m->slot[j].tier;
	}
	sift_maint_end(m);
	if (j >= 0)
	{
		cap = effective_max_of(m, host);
		sift_maint_begin(m);
		res = !needs_worker(m, db, cold_ok, now) ? SIFT_KICK_WOKE : (t = reserve_locked(m, db, now, cap)).worker >= 0 ? SIFT_KICK_SPAWNED : SIFT_KICK_NOSLOT;
		sift_maint_end(m);
	}
	if (res == SIFT_KICK_SPAWNED && reserve_over_shared_limit(m, host, t.worker))
		res = SIFT_KICK_NOSLOT;
	if (procno >= 0)
		host->wake(procno);
	if (res == SIFT_KICK_NOSLOT && busy)
		return SIFT_KICK_WOKE;
	if (res == SIFT_KICK_NOSLOT)
		host->note_starved(tier, now);
	if (res != SIFT_KICK_SPAWNED)
		return res;
	if (host->spawn(db, t))
	{
		sift_maint_begin(m);
		m->stats.spawned++;
		sift_maint_end(m);
		return res;
	}
	sift_maint_begin(m);
	sift_maint_worker_free(m, t.worker);
	m->stats.spawn_failed++;
	sift_maint_end(m);
	if (busy)
		return SIFT_KICK_WOKE;
	host->note_starved(tier, now);
	return SIFT_KICK_NOSLOT;
}

/*
 * The queued request of the key is runnable and its database has no worker: without a launcher nothing else starts one (an earlier kick found no slot, or the worker left after a busy step).
 * A worker seen without the lock is enough to answer no: it drains the request, and the coalesced requests that find it take no lock
 */
static bool
stranded(SiftMaintShmem *m, const SiftMaintHost *host, const SiftMaintReq *r)
{
	bool		out;
	int			i;

	if (worker_for(m, r->db) >= 0)
		return false;
	sift_shm_lock(&m->lock);
	i = find(m, key_hash(r->db, r->rel, r->kind), r);
	out = i >= 0 && runnable(&m->slot[i], r->db, true, host->now()) && worker_for(m, r->db) < 0;
	sift_shm_unlock(&m->lock);
	return out;
}

SiftMaintEnq
sift_maint_request(SiftMaintShmem *m, const SiftMaintHost *host, SiftMaintMode mode, const SiftMaintReq *req)
{
	SiftMaintEnq res = sift_maint_enqueue(m, host, req);
	uint32		launcher;

	if (mode == SIFT_MAINT_MODE_OFF || mode == SIFT_MAINT_MODE_LOCAL || res == SIFT_ENQ_FULL || res == SIFT_ENQ_UNAVAILABLE)
		return res;
	if (res == SIFT_ENQ_COALESCED && (mode != SIFT_MAINT_MODE_DYNAMIC || !stranded(m, host, req)))
		return res;
	launcher = mode == SIFT_MAINT_MODE_LAUNCHER ? pg_atomic_read_u32(&m->launcher) : 0;
	if (launcher != 0)
		host->wake((int) launcher - 1);
	else
		sift_maint_kick(m, host);
	return res;
}
