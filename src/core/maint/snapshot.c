/*
 * core/maint/snapshot.c - the coherent copy of the queue of maint_sched.h that sift.activity and sift.maint_pacer read.
 */
#include "core/maint/sched.h"

#include "datatype/timestamp.h"

static int
row_order(const void *a, const void *b)
{
	const SiftMaintRow *x = a, *y = b;

	if (x->queued != y->queued)
		return x->queued ? 1 : -1;
	return x->priority != y->priority ? (x->priority < y->priority ? 1 : -1) : (x->rel > y->rel) - (x->rel < y->rel);
}

static int
copy_rows(SiftMaintShmem *m, TimestampTz now, SiftMaintRow rows[SIFT_MAINT_SLOTS], SiftMaintStatus *st)
{
	int			n = 0;

	memset(st, 0, sizeof(*st));
	for (int i = 0; i < SIFT_MAINT_SLOTS; i++)
	{
		const SiftMaintSlot *s = &m->slot[i];

		if (m->khash[i] == 0 || s->state == SIFT_SLOT_FREE)
			continue;
		rows[n++] = (SiftMaintRow) {
			.pid = s->pid, .db = s->db, .rel = s->rel, .part_id = s->part_id, .kind = (SiftMaintKind) s->kind,
			.tier = (SiftPacerTier) s->tier, .phase = (SiftMaintPhase) s->phase, .done = s->done, .total = s->total,
			.bytes_written = s->bytes_written, .queued = s->state == SIFT_SLOT_QUEUED,
			.priority = s->state == SIFT_SLOT_QUEUED ? sift_maint_aged_score(s->score, (SiftPacerTier) s->tier, s->enqueued_at, now) : s->score,
			.started_at = s->state == SIFT_SLOT_QUEUED ? s->enqueued_at : s->claimed_at};
		if (s->state == SIFT_SLOT_QUEUED)
			st->queued[s->tier]++;
		else
			st->claimed++;
	}
	st->stats = m->stats;
	for (int i = 0; i < SIFT_MAINT_COALESCED_SHARDS; i++)
		st->stats.coalesced += pg_atomic_read_u64(&m->coalesced[i].n);
	return n;
}

int
sift_maint_snapshot(SiftMaintShmem *m, const SiftMaintHost *host, SiftMaintRow rows[SIFT_MAINT_SLOTS], SiftMaintStatus *status)
{
	TimestampTz now = host->now();
	int			n = -1;

	for (int t = 0; t < SIFT_MAINT_SNAP_TRIES && n < 0; t++)
	{
		uint32		seq = pg_atomic_read_u32(&m->seq);
		int			got;

		if (seq & 1)
			continue;
		pg_read_barrier();
		got = copy_rows(m, now, rows, status);
		pg_read_barrier();
		if (pg_atomic_read_u32(&m->seq) == seq)
			n = got;
	}
	if (n < 0)
	{
		sift_shm_lock(&m->lock);
		n = copy_rows(m, now, rows, status);
		sift_shm_unlock(&m->lock);
	}
	status->workers_active = pg_atomic_read_u32(&m->active_workers);
	status->workers_max = pg_atomic_read_u32(&m->max_workers);
	qsort(rows, n, sizeof(SiftMaintRow), row_order);
	return n;
}
