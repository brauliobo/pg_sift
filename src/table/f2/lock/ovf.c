/*
 * table/f2/lock/ovf.c - see f2_lock_ovf.h.
 *
 * The area is made by the first stripe that fills, pinned, and lives as long as the server: the entries of a prepared transaction outlive its backend. A backend maps it the first time it locks
 * a row once it exists, and keeps the mapping until it exits.
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/lwlock.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

#include "col/dsmreg.h"
#include "core/cachebudget.h"
#include "table/f2/lock/esc.h"
#include "table/f2/lock/ovf.h"

SIFT_BACKEND_LOCAL dsa_area *area = NULL;
SIFT_BACKEND_LOCAL dsa_pointer emptied = InvalidDsaPointer;	/* the chunks removes emptied under the stripe lock, linked by next */

static dsa_handle
handle_of(SiftRowLockTable *t)
{
	return pg_atomic_read_membarrier_u32(&t->area_handle);
}

void
sift_rowlock_table_init(SiftRowLockTable *t, int tranche)
{
	memset(t, 0, sizeof(*t));
	for (int i = 0; i < SIFT_ROWLOCK_PARTITIONS; i++)
	{
		LWLockInitialize(&t->hdr[i].lock, tranche);
		LWLockInitialize(&t->grow_lock[i], tranche);
		pg_atomic_init_u32(&t->hdr[i].total_n, 0);
		t->hdr[i].overflow = InvalidDsaPointer;
	}
	LWLockInitialize(&t->area_lock, tranche);
	pg_atomic_init_u32(&t->area_handle, DSA_HANDLE_INVALID);
}

/* The area made or attached, with the attempt in a resource owner of its own (the descriptor that a failed one leaves is released here); NULL where the system has no dynamic shared memory for it */
static dsa_area *
area_try(SiftRowLockTable *t, bool make)
{
	MemoryContext cx = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner,
				attempt = ResourceOwnerCreate(owner, "sift row lock area");
	dsa_area   *a = NULL;
	uint32		holdoff = InterruptHoldoffCount;

	CurrentResourceOwner = attempt;
	PG_TRY();
	{
#ifdef SIFT_TESTING
		if (make && sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "sift_rowlock_area") != NULL)
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not create the area of the row lock chunks (test)")));
		if (!make && sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "sift_rowlock_attach") != NULL)
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not attach the area of the row lock chunks (test)")));
#endif
		/* MEM[MEM-SHM-16]: DSM-backed dsa_create for the row lock entries of the stripes that are full (8 KB chunks, freed when empty) */
		a = make ? dsa_create(t->area_lock.tranche) : dsa_attach(handle_of(t));
		if (make)
			dsa_pin(a);
		dsa_pin_mapping(a);
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(cx);
		e = CopyErrorData();
		if (ERRCODE_TO_CATEGORY(e->sqlerrcode) != ERRCODE_INSUFFICIENT_RESOURCES)
		{
			FreeErrorData(e);
			PG_RE_THROW();
		}
		FlushErrorState();
		InterruptHoldoffCount = holdoff;
		ResourceOwnerRelease(attempt, RESOURCE_RELEASE_BEFORE_LOCKS, false, false);
		FreeErrorData(e);
		a = NULL;
	}
	PG_END_TRY();
	CurrentResourceOwner = owner;
	ResourceOwnerDelete(attempt);
	return a;
}

static SIFT_BACKEND_LOCAL MemoryContext area_cx;

/* The one area of the table, mapped for the life of the backend: made here when create and nobody has made it. NULL only where create and the server has no memory for it: the caller then keeps the lock in the lock manager of PostgreSQL */
static dsa_area *
area_of(SiftRowLockTable *t, bool create)
{
	MemoryContext old;

	if (area != NULL)
		return area;
	/* MEM[MEM-SHM-16]: the descriptor of the area of the row lock chunks, one for each backend that locks rows, kept until it exits */
	if (area_cx == NULL)
		area_cx = sift_cache_create_context("sift rowlock dsa");
	old = MemoryContextSwitchTo(area_cx);
	if (create && handle_of(t) == DSA_HANDLE_INVALID)
	{
		LWLockAcquire(&t->area_lock, LW_EXCLUSIVE);
		if (handle_of(t) == DSA_HANDLE_INVALID && (area = area_try(t, true)) != NULL)
			pg_atomic_write_membarrier_u32(&t->area_handle, dsa_get_handle(area));
		LWLockRelease(&t->area_lock);
	}
	if (area == NULL && handle_of(t) != DSA_HANDLE_INVALID)
	{
#ifdef SIFT_TESTING
		if (!create && sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "sift_rowlock_attach") != NULL)
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not attach the area of the row lock chunks (test)")));
#endif
		area = create ? area_try(t, false) : dsa_attach(handle_of(t));
	}
	if (area != NULL)
		dsa_pin_mapping(area);
	MemoryContextSwitchTo(old);
	return area;
}

/*
 * The area a read of the chunks goes through. The end of a transaction (commit, abort, prepare) attaches nothing: an error there is a PANIC of the server, and the area it needs is one this
 * backend mapped already when an entry of it is in a chunk (an entry moves from a chunk to the table, never the other way). A backend that maps none holds all its entries in the table, and
 * the chunks it does not see belong to others.
 */
static dsa_area *
chunk_area(SiftRowLockTable *t)
{
	return IsTransactionState() ? area_of(t, false) : area;
}

/* An attach that finds no memory maps nothing: the backend then attaches when it reads a chunk, as one does that was started before the area was made */
void
sift_rowlock_area_map(SiftRowLockTable *t)
{
	if (area == NULL && handle_of(t) != DSA_HANDLE_INVALID && IsTransactionState())
		(void) area_of(t, true);
}

static SiftRowLockChunk *
chunk_at(dsa_area *a, dsa_pointer p)
{
	return dsa_get_address(a, p);
}

#define TAG_ONES	UINT64CONST(0x0101010101010101)
#define TAG_HIGHS	UINT64CONST(0x8080808080808080)

/* The entries of the table of the stripe whose tag is the key's, eight tags to a compare: every byte of the word that is zero after the xor shows its high bit, and a byte that only looks like it is told apart by the key */
static SiftRowLockEntry *
find_in_table(SiftRowLockTable *t, int si, const SiftRowLockKey *k)
{
	const SiftRowLockStripeHdr *h = &t->hdr[si];
	uint64		want = sift_rowlock_tag(k) * TAG_ONES;

	for (int w = 0; w < h->table_n; w += 8)
	{
		uint64		tags,
					hit;

		memcpy(&tags, &h->tags[w], sizeof(tags));
		hit = tags ^ want;
		for (hit = (hit - TAG_ONES) & ~hit & TAG_HIGHS; hit != 0; hit &= hit - 1)
		{
			int			i = w + pg_rightmost_one_pos64(hit) / 8;

			if (i < h->table_n && memcmp(&t->stripes[si].entries[i].key, k, sizeof(*k)) == 0)
				return &t->stripes[si].entries[i];
		}
	}
	return NULL;
}

SiftRowLockEntry *
sift_rowlock_find(SiftRowLockTable *t, int si, const SiftRowLockKey *k)
{
	SiftRowLockEntry *e = find_in_table(t, si, k);
	dsa_area   *a;

	if (e != NULL)
		return e;
	a = t->hdr[si].overflow != InvalidDsaPointer ? chunk_area(t) : NULL;
	for (dsa_pointer p = t->hdr[si].overflow; a != NULL && p != InvalidDsaPointer;)
	{
		SiftRowLockChunk *c = chunk_at(a, p);

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
	return t->hdr[si].table_n < SIFT_ROWLOCK_BUCKETS ||
		(t->hdr[si].overflow != InvalidDsaPointer && chunk_at(area_of(t, false), t->hdr[si].overflow)->n < SIFT_ROWLOCK_CHUNK_ENTRIES);
}

dsa_pointer
sift_rowlock_chunk_alloc(SiftRowLockTable *t)
{
	dsa_area   *a = area_of(t, true);

	return a == NULL ? InvalidDsaPointer : dsa_allocate_extended(a, sizeof(SiftRowLockChunk), DSA_ALLOC_NO_OOM);
}

void
sift_rowlock_chunk_free(dsa_pointer chunk)
{
	dsa_free(area, chunk);
}

static void
count_entry(SiftRowLockTable *t, int si, int delta)
{
	pg_atomic_write_u32(&t->hdr[si].total_n, pg_atomic_read_u32(&t->hdr[si].total_n) + delta);
}

static bool
in_table(const SiftRowLockStripe *s, const SiftRowLockEntry *e)
{
	return e >= s->entries && e < s->entries + SIFT_ROWLOCK_BUCKETS;
}

void
sift_rowlock_reset(SiftRowLockTable *t, int si, SiftRowLockEntry *e, const SiftRowLockKey *k)
{
	memset(e, 0, sizeof(*e));
	e->key = *k;
	if (in_table(&t->stripes[si], e))
		t->hdr[si].tags[e - t->stripes[si].entries] = sift_rowlock_tag(k);
}

SiftRowLockEntry *
sift_rowlock_add(SiftRowLockTable *t, int si, const SiftRowLockKey *k, dsa_pointer *spare)
{
	SiftRowLockStripeHdr *h = &t->hdr[si];
	SiftRowLockChunk *c;
	SiftRowLockEntry *e;

	if (h->table_n < SIFT_ROWLOCK_BUCKETS)
		e = &t->stripes[si].entries[h->table_n++];
	else
	{
		if (!sift_rowlock_has_room(t, si))
		{
			Assert(*spare != InvalidDsaPointer);
			c = chunk_at(area, *spare);
			Assert((uintptr_t) c % PG_CACHE_LINE_SIZE == 0);
			c->next = h->overflow;
			c->n = 0;
			h->overflow = *spare;
			*spare = InvalidDsaPointer;
		}
		c = chunk_at(area, h->overflow);
		e = &c->entries[c->n++];
	}
	sift_rowlock_reset(t, si, e, k);
	count_entry(t, si, 1);
	return e;
}

void
sift_rowlock_remove(SiftRowLockTable *t, int si, SiftRowLockEntry *e)
{
	SiftRowLockStripeHdr *h = &t->hdr[si];
	SiftRowLockStripe *s = &t->stripes[si];
	dsa_pointer head = in_table(s, e) ? InvalidDsaPointer : h->overflow;
	SiftRowLockChunk *c;
	SiftRowLockEntry *last;

	Assert(head == InvalidDsaPointer || area != NULL);
	c = head == InvalidDsaPointer ? NULL : chunk_at(area, head);
	last = c != NULL ? &c->entries[--c->n] : &s->entries[--h->table_n];
	if (e != last)
	{
		*e = *last;
		if (c == NULL)
			h->tags[e - s->entries] = h->tags[last - s->entries];
	}
	count_entry(t, si, -1);
	if (c != NULL && c->n == 0)
	{
		h->overflow = c->next;
		c->next = emptied;
		emptied = head;
	}
}

void
sift_rowlock_free_emptied(void)
{
	while (emptied != InvalidDsaPointer)
	{
		dsa_pointer p = emptied;

		emptied = chunk_at(area, p)->next;
		dsa_free(area, p);
	}
}

void
sift_rowlock_sweep(SiftRowLockTable *t, int si, SiftRowLockSweepFn drop, void *arg)
{
	dsa_area   *a;

	for (int i = 0; i < t->hdr[si].table_n;)
		if (drop(&t->stripes[si].entries[i], arg))
			sift_rowlock_remove(t, si, &t->stripes[si].entries[i]);
		else
			i++;
	a = t->hdr[si].overflow != InvalidDsaPointer ? chunk_area(t) : NULL;
	for (dsa_pointer p = t->hdr[si].overflow; a != NULL && p != InvalidDsaPointer;)
	{
		SiftRowLockChunk *c = chunk_at(a, p);
		dsa_pointer next = c->next;

		for (uint32 i = 0; c != NULL && i < c->n;)
		{
			bool		is_head = t->hdr[si].overflow == p;

			if (!drop(&c->entries[i], arg))
				i++;
			else
			{
				sift_rowlock_remove(t, si, &c->entries[i]);
				c = is_head && t->hdr[si].overflow != p ? NULL : c;
			}
		}
		p = next;
	}
}

SiftRowLockCount
sift_rowlock_count(SiftRowLockTable *t, int si)
{
	SiftRowLockCount n = {.in_table = t->hdr[si].table_n};
	dsa_area   *a = t->hdr[si].overflow != InvalidDsaPointer ? chunk_area(t) : NULL;

	for (dsa_pointer p = t->hdr[si].overflow; a != NULL && p != InvalidDsaPointer;)
	{
		SiftRowLockChunk *c = chunk_at(a, p);

		n.in_chunks += c->n;
		n.chunks++;
		p = c->next;
	}
	return n;
}
