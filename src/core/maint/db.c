/*
 * core/maint/db.c - what the queue of maint_sched.h knows of each database: the backoff after a refused connection and the hold while a statement waits for the workers of the database to leave.
 */
#include "core/maint/sched.h"

#include "datatype/timestamp.h"

/* The entry of db: its own, else a free one, else the backoff that ends first (a hold is never taken from a statement that waits on it); NULL when every entry is held. The caller holds the lock of the table */
static SiftMaintDbState *
entry_of(SiftMaintShmem *m, Oid db)
{
	SiftMaintDbState *spare = NULL;

	for (int i = 0; i < SIFT_MAINT_DBSTATE; i++)
	{
		SiftMaintDbState *e = &m->dbstate[i];

		if (e->db == db)
			return e;
		if (e->holds == 0 && (spare == NULL || e->until < spare->until))
			spare = e;
	}
	if (spare != NULL)
		*spare = (SiftMaintDbState) {.db = db};
	return spare;
}

bool
sift_maint_db_state_blocks(const SiftMaintDbState *e, TimestampTz now)
{
	return e->holds > 0 || e->until > now;
}

bool
sift_maint_db_held(const SiftMaintShmem *m, Oid db, TimestampTz now)
{
	for (int i = 0; i < SIFT_MAINT_DBSTATE; i++)
		if (m->dbstate[i].db == db && sift_maint_db_state_blocks(&m->dbstate[i], now))
			return true;
	return false;
}

uint32
sift_maint_db_connect_failed(SiftMaintShmem *m, TimestampTz now, Oid db)
{
	SiftMaintDbState *e;
	uint32		failures = 0;

	sift_shm_lock(&m->lock);
	if ((e = entry_of(m, db)) != NULL)
	{
		failures = ++e->failures;
		e->until = now + Min((TimestampTz) SIFT_MAINT_CONNECT_BACKOFF_US << Min(failures - 1, 5), SIFT_MAINT_CONNECT_BACKOFF_MAX_US);
	}
	sift_shm_unlock(&m->lock);
	return failures;
}

static SiftMaintDbState *
existing(SiftMaintShmem *m, Oid db)
{
	for (int i = 0; i < SIFT_MAINT_DBSTATE; i++)
		if (m->dbstate[i].db == db)
			return &m->dbstate[i];
	return NULL;
}

/* The backoff of db ends; its holds stay, and an entry with none is free again */
static void
forget_backoff(SiftMaintDbState *e)
{
	e->failures = 0;
	e->until = 0;
	if (e->holds == 0)
		e->db = InvalidOid;
}

void
sift_maint_db_connected(SiftMaintShmem *m, Oid db)
{
	SiftMaintDbState *e;

	sift_shm_lock(&m->lock);
	if ((e = existing(m, db)) != NULL)
		forget_backoff(e);
	sift_shm_unlock(&m->lock);
}

bool
sift_maint_db_hold(SiftMaintShmem *m, Oid db)
{
	SiftMaintDbState *e;

	sift_shm_lock(&m->lock);
	if ((e = entry_of(m, db)) != NULL)
		e->holds++;
	sift_shm_unlock(&m->lock);
	return e != NULL;
}

void
sift_maint_db_release(SiftMaintShmem *m, Oid db)
{
	SiftMaintDbState *e;

	sift_shm_lock(&m->lock);
	e = existing(m, db);
	Assert(e != NULL && e->holds > 0);
	if (--e->holds == 0)
		forget_backoff(e);
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
