/*
 * col/wbuf/reads.c - the rows the backends of a server decoded from a write buffer to read it.
 *
 * A backend builds the batches of a buffer in its first statement that reads it (col/wbuf/batches.c, 5,900 instructions a row) and keeps them, so that the cost is
 * paid again by each backend that comes after, and by none while the buffer is promoted away. Maintenance weighs the sum of what the backends paid against the promotion
 * (maint_wbuf_read_due): the sum is here, in a table of the DSM registry (nothing is allocated at postmaster start) keyed by the generation of the buffer, which no
 * other buffer of the index ever has. A write-only buffer has no entry.
 */
#include "postgres.h"

#include "storage/spin.h"

#include "col/dsmreg.h"
#include "col/wbuf.h"

#define READS_SLOTS		64
#define READS_NAME		"sift_col_wbuf_reads_1"

typedef struct ReadsEnt
{
	bool		used;
	Oid			db;
	RelFileNumber rel;
	uint64		gen;
	uint64		rows;
	uint64		tick;			/* the least recently noted goes when the table is full */
} ReadsEnt;

typedef struct Reads
{
	slock_t		mutex;
	uint64		tick;
	ReadsEnt	ent[READS_SLOTS];
} Reads;

static Reads *reads;
static bool reads_failed;
static bool decoded_here;		/* this backend decoded rows since the last sift_cwbuf_decoded_take */

static void
reads_init(SIFT_DSM_INIT_ARGS)
{
	Reads	   *s = ptr;

	memset(s, 0, sizeof(Reads));
	SpinLockInit(&s->mutex);
}

/* NULL where the table is not had yet: sift_dsm_attach goes without it under a write statement (the aminsert supersede reads the buffer too) and tries again in the next statement */
static Reads *
reads_get(void)
{
	if (reads == NULL && !reads_failed)
		reads = sift_dsm_attach(READS_NAME, sizeof(Reads), reads_init, &reads_failed, "the reads of the write buffers", NULL, false);
	return reads;
}

void
sift_cwbuf_decoded_note(Relation index, uint64 gen, uint64 rows)
{
	Reads	   *s = reads_get();
	ReadsEnt   *e = NULL;

	if (s == NULL || rows == 0)
		return;
	decoded_here = true;
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < READS_SLOTS; i++)
	{
		ReadsEnt   *c = &s->ent[i];

		if (c->used && c->gen == gen && c->rel == index->rd_locator.relNumber && c->db == index->rd_locator.dbOid)
		{
			e = c;
			break;
		}
		if (e == NULL || (e->used && (!c->used || c->tick < e->tick)))
			e = c;
	}
	if (!e->used || e->gen != gen || e->rel != index->rd_locator.relNumber || e->db != index->rd_locator.dbOid)
		*e = (ReadsEnt) {.used = true, .db = index->rd_locator.dbOid, .rel = index->rd_locator.relNumber, .gen = gen};
	e->rows += rows;
	e->tick = ++s->tick;
	SpinLockRelease(&s->mutex);
}

bool
sift_cwbuf_decoded_take(void)
{
	bool		was = decoded_here;

	decoded_here = false;
	return was;
}

uint64
sift_cwbuf_decoded(Relation index, uint64 gen)
{
	Reads	   *s = reads_get();
	uint64		rows = 0;

	if (s == NULL)
		return 0;
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < READS_SLOTS; i++)
		if (s->ent[i].used && s->ent[i].gen == gen && s->ent[i].rel == index->rd_locator.relNumber && s->ent[i].db == index->rd_locator.dbOid)
			rows = s->ent[i].rows;
	SpinLockRelease(&s->mutex);
	return rows;
}
