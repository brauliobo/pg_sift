/*
 * col/f2/uniq/shm.c - the registry of the keys UNIQUE indexes of format major 2 hold in a stage (f2_uniq_shm.h).
 *
 * The registry is a table of the dynamic shared memory registry (col/dsmreg.h), made by the first backend that writes to a unique index; its entries are used from the first of their stripe on,
 * so what no writer touches stays untouched. A backend remembers the entries it made, in a list of the transaction, by the (sub)transaction that made them: the entries of an aborted subtransaction go
 * with it, those of a committed one pass to its parent, and all that are left go when the transaction commits (its stage was flushed before), prepares or aborts, or when the backend exits.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "varatt.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/memutils.h"

#include "col/dsmreg.h"
#include "col/f2/uniq/shm.h"
#include "core/f2/gate.h"
#include "core/hash.h"

#define UNIQ_ABORT_GATE		20	/* testing: a rolled back subtransaction has left the running xids and has not let go of its entries (the gates of core/f2/gate.h are 10 to 14) */
#define TABLE_NAME			"sift_f2_uniq_registry"
#define TRANCHE_NAME		"SiftUniqReg"
#define SLOTS_PER_STRIPE	(SIFT_UNIQ_SHM_SLOTS / SIFT_UNIQ_SHM_STRIPES)
#define BUCKETS_PER_STRIPE	(SIFT_UNIQ_SHM_BUCKETS / SIFT_UNIQ_SHM_STRIPES)
#define STRIPE_BITS			8

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) Stripe
{
	LWLock		lock;
	uint32		free_head;		/* the first entry that was let go, plus one; 0: none */
	uint32		used;			/* entries of the stripe that were ever used: the others are as the segment was made */
	uint64		swept_at;		/* the transactions that had ended (xactCompletionCount) when the stripe was last swept */
} Stripe;

/* The spare entries follow the entries of the stripes; their own lock is taken under the lock of a stripe */
typedef struct UniqShm
{
	int			tranche;
	Stripe		stripe[SIFT_UNIQ_SHM_STRIPES] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	Stripe		spare;
	uint32		bucket[SIFT_UNIQ_SHM_BUCKETS] pg_attribute_aligned(PG_CACHE_LINE_SIZE);	/* the first entry of the bucket, plus one: the buckets of a stripe are one run, bucket_of */
	SiftShmUniqEntry slot[SIFT_UNIQ_SHM_SLOTS + SIFT_UNIQ_SHM_SPARE] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
} UniqShm;

StaticAssertDecl(SIFT_UNIQ_SHM_STRIPES == (1 << STRIPE_BITS), "the stripe of a key is the low STRIPE_BITS bits of its hash");
StaticAssertDecl(BUCKETS_PER_STRIPE * sizeof(uint32) % PG_CACHE_LINE_SIZE == 0, "the buckets of a stripe fill whole cache lines, which no other stripe shares");
StaticAssertDecl(SLOTS_PER_STRIPE * sizeof(SiftShmUniqEntry) % PG_CACHE_LINE_SIZE == 0, "the entries of a stripe fill whole cache lines, which no other stripe shares");
StaticAssertDecl(offsetof(UniqShm, bucket) % PG_CACHE_LINE_SIZE == 0 && offsetof(UniqShm, slot) % PG_CACHE_LINE_SIZE == 0, "the buckets and the entries start on a cache line");

/* The bucket of a hash is in the run of the buckets of its stripe, the low bits of the hash choose the stripe and the next ones the place in the run */
static inline uint32
bucket_of(uint32 hash)
{
	return ((hash & (SIFT_UNIQ_SHM_STRIPES - 1)) * BUCKETS_PER_STRIPE) | ((hash >> STRIPE_BITS) & (BUCKETS_PER_STRIPE - 1));
}

typedef struct Owned
{
	uint32		slot;
	uint32		stripe;			/* of the key: a spare entry is in the buckets of the stripe that borrowed it */
	TransactionId xid;
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
	LWLockInitialize(&u->spare.lock, u->tranche);
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

/* Takes entry i out of its bucket and gives it back to its stripe, or to the spares; the stripe is locked */
static void
entry_free(UniqShm *u, uint32 i)
{
	SiftShmUniqEntry *e = &u->slot[i];
	uint32	   *link = &u->bucket[bucket_of(e->key_hash)];
	Stripe	   *home = i < SIFT_UNIQ_SHM_SLOTS ? stripe_of(u, e->key_hash) : &u->spare;

	while (*link != i + 1)
		link = &u->slot[*link - 1].next_slot;
	*link = e->next_slot;
	e->state = SIFT_UNIQ_FREE;
	if (home == &u->spare)
		LWLockAcquire(&home->lock, LW_EXCLUSIVE);
	e->next_slot = home->free_head;
	home->free_head = i + 1;
	if (home == &u->spare)
		LWLockRelease(&home->lock);
}

/*
 * Whether the (sub)transaction still runs. The answer may read pg_subtrans from disk, so no lock of the registry is held: an entry is judged outside the stripe lock and freed after, and a
 * transaction that has ended makes no entry again.
 */
static bool
xid_runs(TransactionId xid)
{
	return TransactionIdIsCurrentTransactionId(xid) || TransactionIdIsInProgress(xid);
}

#define PROBE_CANDIDATES	(2 * SIFT_UNIQ_HITS)

typedef struct Candidate
{
	uint32		slot;
	TransactionId xid;
} Candidate;

/* The entries of the key, the first cap of them copied; returns how many there are (the stripe is locked) */
static int
candidates_of(const UniqShm *u, const SiftUniqKey *k, Candidate *cand, SiftUniqHit *seen, int cap)
{
	int			n = 0;

	for (uint32 link = u->bucket[bucket_of(k->hash)]; link != 0; link = u->slot[link - 1].next_slot)
	{
		const SiftShmUniqEntry *e = &u->slot[link - 1];

		if (!entry_is(e, k))
			continue;
		if (n < cap)
		{
			cand[n] = (Candidate) {link - 1, e->xid};
			seen[n] = (SiftUniqHit) {.xid = e->xid,.external = e->inline_len == SIFT_UNIQ_LEN_EXTERNAL};
			unpack_tid(e->heap_tid, &seen[n].tid);
		}
		n++;
	}
	return n;
}

/* Gives back the entries of the key that name a transaction that ended, as long as the slot still holds the entry that was judged (the stripe is locked) */
static void
free_ended(UniqShm *u, const SiftUniqKey *k, const Candidate *cand, const bool *ended, int n)
{
	for (int c = 0; c < n; c++)
		if (ended[c] && u->slot[cand[c].slot].state == SIFT_UNIQ_ACTIVE && u->slot[cand[c].slot].xid == cand[c].xid && entry_is(&u->slot[cand[c].slot], k))
			entry_free(u, cand[c].slot);
}

void
sift_uniq_shm_probe(const SiftUniqKey *k, SiftUniqHits *hits)
{
	UniqShm    *u = registry();
	Stripe	   *s = stripe_of(u, k->hash);
	int			cap = PROBE_CANDIDATES,
				n;
	Candidate  *cand = palloc(sizeof(Candidate) * cap);
	SiftUniqHit *seen = palloc(sizeof(SiftUniqHit) * cap);
	bool	   *ended;
	bool		any_ended = false;

	for (;;)
	{
		LWLockAcquire(&s->lock, LW_SHARED);
		n = candidates_of(u, k, cand, seen, cap);
		LWLockRelease(&s->lock);
		if (n <= cap)
			break;
		cap = 2 * n;
		cand = repalloc(cand, sizeof(Candidate) * cap);
		seen = repalloc(seen, sizeof(SiftUniqHit) * cap);
	}
	ended = palloc(sizeof(bool) * Max(n, 1));
	*hits = (SiftUniqHits) {.hit = seen};
	for (int c = 0; c < n; c++)
		if ((ended[c] = !xid_runs(cand[c].xid)))
			any_ended = true;
		else
			seen[hits->n++] = seen[c];
	if (any_ended)
	{
		LWLockAcquire(&s->lock, LW_EXCLUSIVE);
		free_ended(u, k, cand, ended, n);
		LWLockRelease(&s->lock);
	}
	pfree(cand);
	pfree(ended);
}

static int
owned_by_stripe(const void *a, const void *b)
{
	uint32		x = ((const Owned *) a)->stripe,
				y = ((const Owned *) b)->stripe;

	return (x > y) - (x < y);
}

/*
 * A (sub)transaction is out of the running xids before the callbacks of its end run, and a probe that finds an entry of such an xid frees it (and a writer may take its slot): the owner lets go
 * of an entry only while the slot still holds the entry of its xid, which no one makes again once that xid has ended. The entries that are let go are moved after the ones that are kept, and, when
 * there are more than a few, put in the order of their stripes: each stripe is locked once for all its entries
 */
static void
owned_release_where(bool (*pick) (const Owned *, void *), void *arg)
{
	int			kept = 0;

	for (int n = 0; n < nowned; n++)
		if (!pick(&owned[n], arg))
		{
			Owned		t = owned[kept];

			owned[kept++] = owned[n];
			owned[n] = t;
		}
	if (nowned - kept > 4)
		qsort(&owned[kept], nowned - kept, sizeof(Owned), owned_by_stripe);
	for (int n = kept; n < nowned;)
	{
		Stripe	   *s = &shm->stripe[owned[n].stripe];

		LWLockAcquire(&s->lock, LW_EXCLUSIVE);
		for (; n < nowned && &shm->stripe[owned[n].stripe] == s; n++)
			if (shm->slot[owned[n].slot].state == SIFT_UNIQ_ACTIVE && shm->slot[owned[n].slot].xid == owned[n].xid)
				entry_free(shm, owned[n].slot);
		LWLockRelease(&s->lock);
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
	{
		SIFT_F2_GATE(UNIQ_ABORT_GATE);
		owned_release_where(pick_sub, &mySubid);
	}
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
		for (int n = 0; n < nowned; n++)
			if (owned[n].subid == mySubid)
				owned[n].subid = parentSubid;
}

/* Room in the list for one more entry, and the callbacks that let the entries go: everything that can fail comes before an entry is made, so that none is made that nobody lets go */
static void
owned_ensure(void)
{
	static bool registered;

	if (!registered)
	{
		before_shmem_exit(uniq_exit, 0);
		RegisterXactCallback(uniq_xact, NULL);
		RegisterSubXactCallback(uniq_subxact, NULL);
		registered = true;
	}
#ifdef SIFT_TESTING
	if (sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "sift_uniq_owned") != NULL)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("could not list the unique key entries of the transaction (test)")));
#endif
	if (owned == NULL)
		owned = MemoryContextAlloc(TopTransactionContext, sizeof(Owned) * (capowned = 256));
	else if (nowned == capowned)
		owned = repalloc(owned, sizeof(Owned) * (capowned *= 2));
}

/* The entry in the list that owned_ensure made room in */
static void
owned_add(const SiftUniqKey *k, uint32 slot, TransactionId xid)
{
	Assert(nowned < capowned);
	owned[nowned++] = (Owned) {.slot = slot,.stripe = k->hash & (SIFT_UNIQ_SHM_STRIPES - 1),.xid = xid,.subid = GetCurrentSubTransactionId(),.locator = k->locator};
}

/* The next free entry of the stripe, plus one; 0 where it has none (the stripe is locked) */
static uint32
take_free(UniqShm *u, Stripe *s, uint32 hash)
{
	uint32		i;

	if (s->free_head != 0)
	{
		i = s->free_head;
		s->free_head = u->slot[i - 1].next_slot;
		return i;
	}
	return s->used < SLOTS_PER_STRIPE ? (hash & (SIFT_UNIQ_SHM_STRIPES - 1)) * SLOTS_PER_STRIPE + s->used++ + 1 : 0;
}

/* A spare entry, plus one; 0 where none is left (the stripe that borrows it is locked) */
static uint32
take_spare(UniqShm *u)
{
	Stripe	   *s = &u->spare;
	uint32		i = 0;

	LWLockAcquire(&s->lock, LW_EXCLUSIVE);
	if (s->free_head != 0)
	{
		i = s->free_head;
		s->free_head = u->slot[i - 1].next_slot;
	}
	else if (s->used < SIFT_UNIQ_SHM_SPARE)
		i = SIFT_UNIQ_SHM_SLOTS + s->used++ + 1;
	LWLockRelease(&s->lock);
	return i;
}

/* The top-level transactions that have ended since the server started: while it stays, no entry of a stripe names a transaction that ended since its last sweep */
static uint64
completions(void)
{
#if PG_VERSION_NUM >= 170000
	return TransamVariables->xactCompletionCount;
#else
	return ShmemVariableCache->xactCompletionCount;
#endif
}

static int
xid_cmp(const void *a, const void *b)
{
	TransactionId x = *(const TransactionId *) a,
				y = *(const TransactionId *) b;

	return (x > y) - (x < y);
}

/* The transactions of the entries of a stripe, each once, in order (the stripe is not locked on return) */
static TransactionId *
stripe_xids(UniqShm *u, Stripe *s, int *n)
{
	const SiftShmUniqEntry *first = &u->slot[(s - u->stripe) * SLOTS_PER_STRIPE];
	TransactionId *xids = palloc(sizeof(TransactionId) * SLOTS_PER_STRIPE);
	int			kept = 0;

	*n = 0;
	LWLockAcquire(&s->lock, LW_SHARED);
	for (uint32 j = 0; j < s->used; j++)
		if (first[j].state == SIFT_UNIQ_ACTIVE)
			xids[(*n)++] = first[j].xid;
	LWLockRelease(&s->lock);
	qsort(xids, *n, sizeof(TransactionId), xid_cmp);
	for (int j = 0; j < *n; j++)
		if (j == 0 || xids[j] != xids[j - 1])
			xids[kept++] = xids[j];
	*n = kept;
	return xids;
}

/* Gives back the entries of the transactions that ended, for a stripe that has none free: what a rolled back transaction of a backend that died, or one whose entries a flush cannot name, left */
static void
sweep_ended(UniqShm *u, Stripe *s)
{
	uint64		at = completions();
	int			n,
				ended = 0;
	TransactionId *xids = stripe_xids(u, s, &n);
	SiftShmUniqEntry *first = &u->slot[(s - u->stripe) * SLOTS_PER_STRIPE];

	Assert(!LWLockHeldByMe(&s->lock));
	for (int j = 0; j < n; j++)
		if (!xid_runs(xids[j]))
			xids[ended++] = xids[j];
	LWLockAcquire(&s->lock, LW_EXCLUSIVE);
	s->swept_at = at;
	for (uint32 j = 0; ended > 0 && j < s->used; j++)
		if (first[j].state == SIFT_UNIQ_ACTIVE && bsearch(&first[j].xid, xids, ended, sizeof(TransactionId), xid_cmp) != NULL)
			entry_free(u, (s - u->stripe) * SLOTS_PER_STRIPE + j);
	LWLockRelease(&s->lock);
	pfree(xids);
}

/*
 * Takes a free entry of the stripe, else a spare one. A stripe with none gives back the entries of the transactions that ended first, with no lock held, when one has ended since its last
 * sweep or no spare is left. The stripe is locked on return; 0 where no entry is free even then
 */
static uint32
reserve_slot(UniqShm *u, Stripe *s, uint32 hash)
{
	uint32		i;

	LWLockAcquire(&s->lock, LW_EXCLUSIVE);
	if ((i = take_free(u, s, hash)) != 0 || (s->swept_at == completions() && (i = take_spare(u)) != 0))
		return i;
	LWLockRelease(&s->lock);
	sweep_ended(u, s);
	LWLockAcquire(&s->lock, LW_EXCLUSIVE);
	return (i = take_free(u, s, hash)) != 0 ? i : take_spare(u);
}

bool
sift_uniq_shm_reserve(const SiftUniqKey *k, TransactionId xid, ItemPointer tid)
{
	UniqShm    *u = registry();
	Stripe	   *s = stripe_of(u, k->hash);
	uint32	   *head = &u->bucket[bucket_of(k->hash)];
	uint32		i;

	owned_ensure();
	if ((i = reserve_slot(u, s, k->hash)) == 0)
	{
		LWLockRelease(&s->lock);
		return false;
	}
	i--;
	u->slot[i] = (SiftShmUniqEntry) {.locator = k->locator,.key_hash = k->hash,.key_fp64 = k->fp64,.xid = xid,.heap_tid = pack_tid(tid),
		.inline_len = k->len,.state = SIFT_UNIQ_ACTIVE,.next_slot = *head};
	memcpy(u->slot[i].inline_key, k->bytes, sizeof(k->bytes));
	*head = i + 1;
	owned_add(k, i, xid);
	LWLockRelease(&s->lock);
	return true;
}

#ifdef SIFT_TESTING

#include "fmgr.h"

PG_FUNCTION_INFO_V1(sift_test_uniq_shm_active);
PG_FUNCTION_INFO_V1(sift_test_uniq_shm_fill);
PG_FUNCTION_INFO_V1(sift_test_uniq_shm_reserve_one);

static bool
reserve_test_key(int stripe, uint32 n, TransactionId xid)
{
	SiftUniqKey k;
	ItemPointerData tid;

	sift_uniq_key_begin(&k, &(RelFileLocator) {.spcOid = 1,.dbOid = 2,.relNumber = 990001}, (uint32) stripe | (n << 6), 1);
	k.fp64 = n;
	ItemPointerSet(&tid, 0, 1);
	return sift_uniq_shm_reserve(&k, xid, &tid);
}

/* sift._test_uniq_shm_active(): the entries of the registry that hold a key */
Datum
sift_test_uniq_shm_active(PG_FUNCTION_ARGS)
{
	UniqShm    *u = registry();
	int64		n = 0;

	for (int i = 0; i < SIFT_UNIQ_SHM_SLOTS + SIFT_UNIQ_SHM_SPARE; i++)
		n += u->slot[i].state == SIFT_UNIQ_ACTIVE;
	PG_RETURN_INT64(n);
}

/* sift._test_uniq_shm_fill(stripe int, n int, xid int8): entries of the stripe for the transaction xid, until n or the stripe is full; the entries made */
Datum
sift_test_uniq_shm_fill(PG_FUNCTION_ARGS)
{
	int			made = 0;

	while (made < PG_GETARG_INT32(1) && reserve_test_key(PG_GETARG_INT32(0), made, (TransactionId) PG_GETARG_INT64(2)))
		made++;
	PG_RETURN_INT32(made);
}

/* sift._test_uniq_shm_reserve_one(stripe int): an entry of the stripe for the current transaction, a key no other has; whether the stripe had room */
Datum
sift_test_uniq_shm_reserve_one(PG_FUNCTION_ARGS)
{
	static uint32 next = 1 << 20;

	PG_RETURN_BOOL(reserve_test_key(PG_GETARG_INT32(0), next++, GetCurrentTransactionId()));
}

#endif							/* SIFT_TESTING */
