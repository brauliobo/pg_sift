/*
 * table/f2/f2_lock_ovf.c - see f2_lock_ovf.h.
 *
 * The area is made by the first stripe that fills, pinned, and lives as long as the server: the entries of a prepared transaction outlive its backend. A backend maps it for the transaction that
 * reads or changes a chunk and lets go at its end (sift_rowlock_area_detach), so that an idle backend maps nothing.
 */
#include "postgres.h"

#include "storage/lwlock.h"
#include "utils/memutils.h"

#include "col/dsmreg.h"
#include "table/f2/f2_lock_esc.h"
#include "table/f2/f2_lock_ovf.h"

SIFT_BACKEND_LOCAL dsa_area *area = NULL;

void
sift_rowlock_table_init(SiftRowLockTable *t, int tranche)
{
	memset(t, 0, sizeof(*t));
	for (int i = 0; i < SIFT_ROWLOCK_PARTITIONS; i++)
		LWLockInitialize(&t->stripe_locks[i].lock, tranche);
	LWLockInitialize(&t->area_lock, tranche);
}

/* The one area of the table, in the memory of the transaction: it is made here when create and nobody has made it */
static dsa_area *
area_of(SiftRowLockTable *t, bool create)
{
	MemoryContext old;

	if (area != NULL)
		return area;
	old = MemoryContextSwitchTo(sift_rowlock_txn_cx());
	if (create && t->area_handle == DSA_HANDLE_INVALID)
	{
		LWLockAcquire(&t->area_lock, LW_EXCLUSIVE);
		if (t->area_handle == DSA_HANDLE_INVALID)
		{
			/* MEM[MEM-SHM-16]: DSM-backed dsa_create for the row lock entries of the stripes that are full (8 KB chunks, freed when empty) */
#ifdef SIFT_TESTING
			if (sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "sift_rowlock_area") != NULL)
				ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not create the area of the row lock chunks")));
#endif
			area = dsa_create(t->area_lock.tranche);
			dsa_pin(area);
			t->area_handle = dsa_get_handle(area);
		}
		LWLockRelease(&t->area_lock);
	}
	if (area == NULL)
		area = dsa_attach(t->area_handle);
	dsa_pin_mapping(area);
	MemoryContextSwitchTo(old);
	return area;
}

void
sift_rowlock_area_detach(void)
{
	if (area != NULL)
		dsa_detach(area);
	area = NULL;
}

static SiftRowLockChunk *
chunk_at(SiftRowLockTable *t, dsa_pointer p)
{
	return dsa_get_address(area_of(t, false), p);
}

SiftRowLockEntry *
sift_rowlock_find(SiftRowLockTable *t, int si, const SiftRowLockKey *k)
{
	SiftRowLockStripe *s = &t->stripes[si];

	for (int i = 0; i < t->stripe_n[si]; i++)
		if (memcmp(&s->entries[i].key, k, sizeof(*k)) == 0)
			return &s->entries[i];
	for (dsa_pointer p = t->overflow[si]; p != InvalidDsaPointer;)
	{
		SiftRowLockChunk *c = chunk_at(t, p);

		for (uint32 i = 0; i < c->n; i++)
			if (memcmp(&c->entries[i].key, k, sizeof(*k)) == 0)
				return &c->entries[i];
		p = c->next;
	}
	return NULL;
}

bool
sift_rowlock_has_room(SiftRowLockTable *t, int si)
{
	return t->stripe_n[si] < SIFT_ROWLOCK_BUCKETS || (t->overflow[si] != InvalidDsaPointer && chunk_at(t, t->overflow[si])->n < SIFT_ROWLOCK_CHUNK_ENTRIES);
}

SiftRowLockEntry *
sift_rowlock_add(SiftRowLockTable *t, int si)
{
	SiftRowLockChunk *c;

	if (t->stripe_n[si] < SIFT_ROWLOCK_BUCKETS)
		return &t->stripes[si].entries[t->stripe_n[si]++];
	if (!sift_rowlock_has_room(t, si))
	{
		dsa_pointer p = dsa_allocate_extended(area_of(t, true), sizeof(SiftRowLockChunk), DSA_ALLOC_NO_OOM);

		if (p == InvalidDsaPointer)
			return NULL;
		c = dsa_get_address(area, p);
		Assert((uintptr_t) c % PG_CACHE_LINE_SIZE == 0);
		c->next = t->overflow[si];
		c->n = 0;
		t->overflow[si] = p;
	}
	c = chunk_at(t, t->overflow[si]);
	return &c->entries[c->n++];
}

void
sift_rowlock_remove(SiftRowLockTable *t, int si, SiftRowLockEntry *e)
{
	dsa_pointer head = t->overflow[si];
	SiftRowLockChunk *c = head == InvalidDsaPointer ? NULL : chunk_at(t, head);
	SiftRowLockEntry *last = c != NULL ? &c->entries[--c->n] : &t->stripes[si].entries[--t->stripe_n[si]];

	if (e != last)
		*e = *last;
	if (c != NULL && c->n == 0)
	{
		t->overflow[si] = c->next;
		dsa_free(area, head);
	}
}

void
sift_rowlock_sweep(SiftRowLockTable *t, int si, SiftRowLockSweepFn drop, void *arg)
{
	for (int i = 0; i < t->stripe_n[si];)
		if (drop(&t->stripes[si].entries[i], arg))
			sift_rowlock_remove(t, si, &t->stripes[si].entries[i]);
		else
			i++;
	for (dsa_pointer p = t->overflow[si]; p != InvalidDsaPointer;)
	{
		SiftRowLockChunk *c = chunk_at(t, p);
		dsa_pointer next = c->next;

		for (uint32 i = 0; c != NULL && i < c->n;)
		{
			bool		is_head = t->overflow[si] == p;

			if (!drop(&c->entries[i], arg))
				i++;
			else
			{
				sift_rowlock_remove(t, si, &c->entries[i]);
				c = is_head && t->overflow[si] != p ? NULL : c;
			}
		}
		p = next;
	}
}

SiftRowLockCount
sift_rowlock_count(SiftRowLockTable *t, int si)
{
	SiftRowLockCount n = {.in_table = t->stripe_n[si]};

	for (dsa_pointer p = t->overflow[si]; p != InvalidDsaPointer;)
	{
		SiftRowLockChunk *c = chunk_at(t, p);

		n.in_chunks += c->n;
		n.chunks++;
		p = c->next;
	}
	return n;
}
