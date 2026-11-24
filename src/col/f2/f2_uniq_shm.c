/*
 * col/f2/f2_uniq_shm.c - the registry of the keys UNIQUE indexes of format major 2 hold in a stage (f2_uniq_shm.h).
 *
 * The registry is a table of the dynamic shared memory registry (col/dsmreg.h), made by the first backend that writes to a unique index; its entries are used from the first of their stripe on,
 * so what no writer touches stays untouched. A backend remembers the entries it made, in a list of the transaction, by the (sub)transaction that made them: the entries of an aborted subtransaction go
 * with it, those of a committed one pass to its parent, and all that are left go when the transaction commits (its stage was flushed before), prepares or aborts, or when the backend exits.
 */
#include "postgres.h"

#include "access/xact.h"
#include "varatt.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/memutils.h"

#include "col/dsmreg.h"
#include "col/f2/f2_uniq_shm.h"
#include "core/hash.h"

#define TABLE_NAME			"sift_f2_uniq_registry"
#define TRANCHE_NAME		"SiftUniqReg"
#define SLOTS_PER_STRIPE	(SIFT_UNIQ_SHM_SLOTS / SIFT_UNIQ_SHM_STRIPES)

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) Stripe
{
	LWLock		lock;
	uint32		free_head;		/* the first entry that was let go, plus one; 0: none */
	uint32		used;			/* entries of the stripe that were ever used: the others are as the segment was made */
} Stripe;

typedef struct UniqShm
{
	int			tranche;
	Stripe		stripe[SIFT_UNIQ_SHM_STRIPES] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	uint32		bucket[SIFT_UNIQ_SHM_BUCKETS];	/* the first entry of the bucket, plus one */
	SiftShmUniqEntry slot[SIFT_UNIQ_SHM_SLOTS] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
} UniqShm;

typedef struct Owned
{
	uint32		slot;
	SubTransactionId subid;
	RelFileLocator locator;
} Owned;

static UniqShm *shm;
static Owned *owned;
static int	nowned;
static int	capowned;

static UniqShm *
base_of(void *raw)
{
	return (UniqShm *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
}

static void
uniq_init(SIFT_DSM_INIT_ARGS)
{
	UniqShm    *u = base_of(ptr);

	memset(u, 0, offsetof(UniqShm, slot));
	u->tranche = sift_tranche_new(TRANCHE_NAME);
	for (int i = 0; i < SIFT_UNIQ_SHM_STRIPES; i++)
		LWLockInitialize(&u->stripe[i].lock, u->tranche);
}

static UniqShm *
registry(void)
{
	if (shm == NULL)
	{
		void	   *raw = sift_dsm_try(TABLE_NAME, sizeof(UniqShm) + PG_CACHE_LINE_SIZE, uniq_init);

		if (raw == NULL)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("could not map the registry of the keys of UNIQUE sift_col indexes"), errdetail("%s", sift_dsm_why()),
							errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm).")));
		shm = base_of(raw);
		sift_tranche_register(shm->tranche, TRANCHE_NAME);
	}
	return shm;
}

static uint64
pack_tid(ItemPointer tid)
{
	return ((uint64) ItemPointerGetBlockNumberNoCheck(tid) << 16) | ItemPointerGetOffsetNumberNoCheck(tid);
}

static void
unpack_tid(uint64 packed, ItemPointer tid)
{
	ItemPointerSet(tid, (BlockNumber) (packed >> 16), (OffsetNumber) (packed & 0xFFFF));
}

void
sift_uniq_key_begin(SiftUniqKey *k, const RelFileLocator *locator, uint32 hash, int ncols)
{
	*k = (SiftUniqKey) {.locator = *locator,.hash = hash,.fp64 = SIFT_FIB64,.ncols = ncols};
}

/* typlen: the bytes of a type of fixed width, -1 for a text or varchar (kept inline when it is the only column: the bytes of equal strings are equal), 0 for any other type (numeric) */
void
sift_uniq_key_add(SiftUniqKey *k, Datum value, bool isnull, int16 typlen, bool typbyval, uint64 hash64)
{
	const char *p = NULL;
	int			len = typlen;

	k->fp64 = sift_fmix64(k->fp64 ^ (hash64 + SIFT_FIB64 + (k->fp64 << 6) + (k->fp64 >> 2)));
	k->added++;
	if (k->len == SIFT_UNIQ_LEN_EXTERNAL || isnull || typlen == 0 || (typlen < 0 && k->ncols > 1))
		goto external;
	if (typlen < 0)
	{
		p = DatumGetPointer(value);
		if (VARATT_IS_COMPRESSED(p) || VARATT_IS_EXTERNAL(p))
			goto external;
		len = VARSIZE_ANY_EXHDR(p);
		p = VARDATA_ANY(p);
	}
	else if (!typbyval)
		p = DatumGetPointer(value);
	if (k->len + len > SIFT_UNIQ_INLINE_MAX)
		goto external;
	if (p != NULL)
		memcpy(&k->bytes[k->len], p, len);
	else
		for (int b = 0; b < len; b++)
			k->bytes[k->len + b] = (uint8) (DatumGetUInt64(value) >> (8 * b));
	k->len += len;
	return;
external:
	k->len = SIFT_UNIQ_LEN_EXTERNAL;
}

static bool
entry_is(const SiftShmUniqEntry *e, const SiftUniqKey *k)
{
	if (e->key_hash != k->hash || e->key_fp64 != k->fp64 || e->inline_len != k->len || !RelFileLocatorEquals(e->locator, k->locator))
		return false;
	return k->len == SIFT_UNIQ_LEN_EXTERNAL || memcmp(e->inline_key, k->bytes, k->len) == 0;
}

static Stripe *
stripe_of(UniqShm *u, uint32 hash)
{
	return &u->stripe[hash & (SIFT_UNIQ_SHM_STRIPES - 1)];
}

/* Takes entry i out of its bucket and gives it back to its stripe; the stripe is locked */
static void
entry_free(UniqShm *u, uint32 i)
{
	SiftShmUniqEntry *e = &u->slot[i];
	uint32	   *link = &u->bucket[e->key_hash & (SIFT_UNIQ_SHM_BUCKETS - 1)];

	while (*link != i + 1)
		link = &u->slot[*link - 1].next_slot;
	*link = e->next_slot;
	e->state = SIFT_UNIQ_FREE;
	e->next_slot = stripe_of(u, e->key_hash)->free_head;
	stripe_of(u, e->key_hash)->free_head = i + 1;
}

void
sift_uniq_shm_probe(const SiftUniqKey *k, SiftUniqHits *hits)
{
	UniqShm    *u = registry();
	Stripe	   *s = stripe_of(u, k->hash);
	uint32	   *link = &u->bucket[k->hash & (SIFT_UNIQ_SHM_BUCKETS - 1)];

	hits->n = 0;
	hits->overflow = false;
	LWLockAcquire(&s->lock, LW_EXCLUSIVE);
	while (*link != 0 && !hits->overflow)
	{
		uint32		i = *link - 1;
		SiftShmUniqEntry *e = &u->slot[i];

		if (!entry_is(e, k))
			link = &e->next_slot;
		else if (!TransactionIdIsCurrentTransactionId(e->xid) && !TransactionIdIsInProgress(e->xid))
			entry_free(u, i);
		else if (hits->n == SIFT_UNIQ_HITS)
			hits->overflow = true;
		else
		{
			SiftUniqHit *h = &hits->hit[hits->n++];

			h->xid = e->xid;
			h->external = e->inline_len == SIFT_UNIQ_LEN_EXTERNAL;
			unpack_tid(e->heap_tid, &h->tid);
			link = &e->next_slot;
		}
	}
	LWLockRelease(&s->lock);
}

static void
owned_release_where(bool (*pick) (const Owned *, void *), void *arg)
{
	int			kept = 0;

	for (int n = 0; n < nowned; n++)
	{
		if (pick(&owned[n], arg))
		{
			Stripe	   *s = stripe_of(shm, shm->slot[owned[n].slot].key_hash);

			LWLockAcquire(&s->lock, LW_EXCLUSIVE);
			entry_free(shm, owned[n].slot);
			LWLockRelease(&s->lock);
		}
		else
			owned[kept++] = owned[n];
	}
	nowned = kept;
}

static bool
pick_all(const Owned *o pg_attribute_unused(), void *arg pg_attribute_unused())
{
	return true;
}

static bool
pick_index(const Owned *o, void *arg)
{
	return RelFileLocatorEquals(o->locator, *(const RelFileLocator *) arg);
}

static bool
pick_sub(const Owned *o, void *arg)
{
	return o->subid == *(const SubTransactionId *) arg;
}

void
sift_uniq_shm_release(const RelFileLocator *locator)
{
	owned_release_where(pick_index, (void *) locator);
}

static void
uniq_exit(int code pg_attribute_unused(), Datum arg pg_attribute_unused())
{
	owned_release_where(pick_all, NULL);
}

static void
uniq_xact(XactEvent event, void *arg pg_attribute_unused())
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT)
	{
		owned_release_where(pick_all, NULL);
		owned = NULL;
		capowned = 0;
	}
}

static void
uniq_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg pg_attribute_unused())
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		owned_release_where(pick_sub, &mySubid);
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
		for (int n = 0; n < nowned; n++)
			if (owned[n].subid == mySubid)
				owned[n].subid = parentSubid;
}

static void
owned_add(const SiftUniqKey *k, uint32 slot)
{
	static bool registered;

	if (!registered)
	{
		before_shmem_exit(uniq_exit, 0);
		RegisterXactCallback(uniq_xact, NULL);
		RegisterSubXactCallback(uniq_subxact, NULL);
		registered = true;
	}
	if (owned == NULL)
		owned = MemoryContextAlloc(TopTransactionContext, sizeof(Owned) * (capowned = 256));
	else if (nowned == capowned)
		owned = repalloc(owned, sizeof(Owned) * (capowned = Min(capowned * 2, SIFT_UNIQ_SHM_SLOTS)));
	owned[nowned++] = (Owned) {.slot = slot,.subid = GetCurrentSubTransactionId(),.locator = k->locator};
}

bool
sift_uniq_shm_reserve(const SiftUniqKey *k, TransactionId xid, ItemPointer tid)
{
	UniqShm    *u = registry();
	Stripe	   *s = stripe_of(u, k->hash);
	uint32	   *head = &u->bucket[k->hash & (SIFT_UNIQ_SHM_BUCKETS - 1)];
	uint32		i;

	LWLockAcquire(&s->lock, LW_EXCLUSIVE);
	if (s->free_head != 0)
	{
		i = s->free_head - 1;
		s->free_head = u->slot[i].next_slot;
	}
	else if (s->used < SLOTS_PER_STRIPE)
		i = (k->hash & (SIFT_UNIQ_SHM_STRIPES - 1)) * SLOTS_PER_STRIPE + s->used++;
	else
	{
		LWLockRelease(&s->lock);
		return false;
	}
	u->slot[i] = (SiftShmUniqEntry) {.locator = k->locator,.key_hash = k->hash,.key_fp64 = k->fp64,.xid = xid,.heap_tid = pack_tid(tid),
		.inline_len = k->len,.state = SIFT_UNIQ_ACTIVE,.next_slot = *head};
	memcpy(u->slot[i].inline_key, k->bytes, sizeof(k->bytes));
	*head = i + 1;
	LWLockRelease(&s->lock);
	owned_add(k, i);
	return true;
}
