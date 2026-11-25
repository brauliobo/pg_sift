/*
 * core/maint_db.c - what the queue of maint_sched.h knows of each database: the backoff after a refused connection and the hold while a statement waits for the workers of the database to leave.
 */
#include "core/maint_sched.h"

#include "datatype/timestamp.h"

/* The entry of db: its own, else a free one or one whose backoff is over; NULL when all eight are held. The caller holds the lock of the table */
static SiftMaintDbState *
entry_of(SiftMaintShmem *m, Oid db, TimestampTz now)
{
	SiftMaintDbState *spare = NULL;

	for (int i = 0; i < SIFT_MAINT_DBSTATE; i++)
	{
		SiftMaintDbState *e = &m->dbstate[i];

		if (e->db == db)
			return e;
		if (spare == NULL && (e->db == InvalidOid || e->until <= now))
			spare = e;
	}
	if (spare != NULL)
		*spare = (SiftMaintDbState) {.db = db};
	return spare;
}

bool
sift_maint_db_held(const SiftMaintShmem *m, Oid db, TimestampTz now)
{
	for (int i = 0; i < SIFT_MAINT_DBSTATE; i++)
		if (m->dbstate[i].db == db && m->dbstate[i].until > now)
			return true;
	return false;
}

uint32
sift_maint_db_connect_failed(SiftMaintShmem *m, TimestampTz now, Oid db)
{
	SiftMaintDbState *e;
	uint32		failures = 0;

	sift_shm_lock(&m->lock);
	if ((e = entry_of(m, db, now)) != NULL)
	{
		failures = ++e->failures;
		e->until = now + Min((TimestampTz) SIFT_MAINT_CONNECT_BACKOFF_US << Min(failures - 1, 5), SIFT_MAINT_CONNECT_BACKOFF_MAX_US);
	}
	sift_shm_unlock(&m->lock);
	return failures;
}

static void
forget(SiftMaintShmem *m, Oid db)
{
	for (int i = 0; i < SIFT_MAINT_DBSTATE; i++)
		if (m->dbstate[i].db == db)
			memset(&m->dbstate[i], 0, sizeof(SiftMaintDbState));
}

void
sift_maint_db_connected(SiftMaintShmem *m, Oid db)
{
	sift_shm_lock(&m->lock);
	forget(m, db);
	sift_shm_unlock(&m->lock);
}

void
sift_maint_db_hold(SiftMaintShmem *m, TimestampTz now, Oid db, bool hold)
{
	SiftMaintDbState *e;

	sift_shm_lock(&m->lock);
	forget(m, db);
	if (hold && (e = entry_of(m, db, now)) != NULL)
		e->until = SIFT_MAINT_HELD;
	sift_shm_unlock(&m->lock);
}

int
sift_maint_db_workers(SiftMaintShmem *m, Oid db)
{
	int			n = 0;

	sift_shm_lock(&m->lock);
	for (int w = 0; w < SIFT_MAINT_MAX_WORKERS; w++)
		n += m->worker[w].state != 0 && m->worker[w].db == db;
	sift_shm_unlock(&m->lock);
	return n;
}
