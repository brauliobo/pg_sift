/*
 * table/f2/lock/esc.c - the partition and relation locks of a sift_table over its rows (f2_lock_esc.h), their subtransactions and their prepare.
 *
 * A request for a coarse lock waits inside the lock manager of the server while the entries of the row lockers it must wait for are still held: a cycle through it and a row wait is a
 * cycle of the deadlock detector. The entries are given back only once the coarse lock is held, and the caller reads the row on disk again then, as after any wait.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/dsmreg.h"
#include "core/memtrack.h"
#include "core/part_lock.h"
#include "table/f2/lock/2pc.h"
#include "table/f2/lock/esc.h"

/* ---- the table in shared memory ---- */

/* MEM[MEM-SHM-05]: DSM row lock table sift_rowlock (1101312 B packed; 128x64x128 B ABI entries, and the heads of the chunks of the stripes that fill) */
#define TABLE_BYTES		(sizeof(SiftRowLockShmLayout) + PG_CACHE_LINE_SIZE)

SIFT_BACKEND_LOCAL SiftRowLockShmLayout *shm = NULL;
SIFT_BACKEND_LOCAL bool pglm = false;


static void
shm_init(SIFT_DSM_INIT_ARGS)
{
	SiftRowLockShmLayout *s = (SiftRowLockShmLayout *) TYPEALIGN(PG_CACHE_LINE_SIZE, ptr);
	int			rows = sift_tranche_new(SIFT_ROWLOCK_TRANCHE);
	int			escs = sift_tranche_new(SIFT_ESC_TRANCHE);

	memset(s, 0, sizeof(*s));
	sift_rowlock_table_init(&s->rows, rows);
	for (int i = 0; i < SIFT_ESC_STRIPES; i++)
		LWLockInitialize(&s->esc_locks[i].lock, escs);
}

SiftRowLockShmLayout *
sift_lock_shm_attach(void)
{
	if (shm == NULL && !pglm)
	{
		void	   *raw = sift_dsm_try(SIFT_ROWLOCK_DSM_NAME, TABLE_BYTES, shm_init);

		if (raw == NULL)
		{
			pglm = true;
			if (sift_shm_first(SIFT_ROWLOCK_DSM_NAME, sift_dsm_why()))
				ereport(WARNING, (errmsg("sift_table row locks use the lock manager of the server: %s", sift_dsm_why())));
		}
		else
		{
			shm = (SiftRowLockShmLayout *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
			sift_tranche_register(shm->rows.area_lock.tranche, SIFT_ROWLOCK_TRANCHE);
			sift_tranche_register(shm->esc_locks[0].lock.tranche, SIFT_ESC_TRANCHE);
		}
	}
	return shm;
}

bool
sift_lock_pglm_only(void)
{
	return sift_lock_shm_attach() == NULL;
}

/* ---- the directory of the coarse holders ---- */

static uint32
dir_stripe(const RelFileLocator *loc, uint64 part_id)
{
	return hash_combine(hash_bytes((const unsigned char *) loc, sizeof(*loc)), (uint32) (part_id ^ (part_id >> 32))) %
		SIFT_ESC_STRIPES;
}

static bool
dir_match(const SiftPartEscEntry *e, const RelFileLocator *loc, uint64 part_id, TransactionId top)
{
	return e->part_id == part_id && e->top_xid == top && RelFileLocatorEquals(e->locator, *loc);
}

static void
dir_compact_ended(SiftEscStripe *s)
{
	uint16	   *n = &shm->esc_n[s - shm->esc_stripes];

	for (int i = 0; i < *n;)
	{
		if (!TransactionIdIsInProgress(s->entries[i].top_xid))
			s->entries[i] = s->entries[--*n];
		else
			i++;
	}
}

typedef enum DirOp
{
	DIR_PUBLISH,
	DIR_REMOVE,
	DIR_MODE,
	DIR_RETAG,
	DIR_PREPARED
} DirOp;

typedef struct DirArg
{
	const RelFileLocator *loc;
	uint64		part_id;
	TransactionId top;
	TransactionId cur;
	SubTransactionId subxid;
	LockTupleMode mode;
} DirArg;

static bool
dir_apply(DirOp op, const DirArg *a)
{
	uint32		si = dir_stripe(a->loc, a->part_id);
	SiftEscStripe *s;
	uint16	   *n;
	SiftPartEscEntry *e = NULL;
	bool		ok = true;

	if (shm == NULL)
		return false;
	s = &shm->esc_stripes[si];
	n = &shm->esc_n[si];
	LWLockAcquire(&shm->esc_locks[si].lock, LW_EXCLUSIVE);
	for (int i = 0; i < *n && e == NULL; i++)
		if (dir_match(&s->entries[i], a->loc, a->part_id, a->top))
			e = &s->entries[i];
	if (op == DIR_PUBLISH && e == NULL)
	{
		if (*n == SIFT_ESC_BUCKETS)
			dir_compact_ended(s);
		ok = *n < SIFT_ESC_BUCKETS;
		e = ok ? &s->entries[(*n)++] : NULL;
		if (ok)
			*e = (SiftPartEscEntry) {.locator = *a->loc,.part_id = a->part_id,.top_xid = a->top,.flags = SIFT_ESC_FLAG_ACTIVE};
	}
	ok = ok && e != NULL;
	if (ok && op == DIR_REMOVE)
		*e = s->entries[--*n];
	else if (ok)
	{
		e->mode = op == DIR_MODE || op == DIR_PUBLISH ? a->mode : e->mode;
		e->sub_eff_xid = op == DIR_PUBLISH ? a->cur : e->sub_eff_xid;
		e->subxid = op == DIR_PUBLISH || op == DIR_RETAG ? a->subxid : e->subxid;
		e->flags |= op == DIR_PREPARED ? SIFT_ESC_FLAG_PREPARED : 0;
	}
	LWLockRelease(&shm->esc_locks[si].lock);
	return ok;
}

uint32
sift_esc_live_count(void)
{
	uint32		n = 0;

	if (shm == NULL)
		return 0;
	for (int si = 0; si < SIFT_ESC_STRIPES; si++)
	{
		LWLockAcquire(&shm->esc_locks[si].lock, LW_EXCLUSIVE);
		dir_compact_ended(&shm->esc_stripes[si]);
		n += shm->esc_n[si];
		LWLockRelease(&shm->esc_locks[si].lock);
	}
	return n;
}

static TransactionId
holder_in_the_way(const RelFileLocator *loc, uint64 part_id, SiftEscModes want)
{
	TransactionId found = InvalidTransactionId;
	uint32		si = dir_stripe(loc, part_id);

	if (shm == NULL)
		return found;
	LWLockAcquire(&shm->esc_locks[si].lock, LW_SHARED);
	for (int i = 0; i < shm->esc_n[si] && !TransactionIdIsValid(found); i++)
	{
		const SiftPartEscEntry *e = &shm->esc_stripes[si].entries[i];

		if (e->part_id == part_id && RelFileLocatorEquals(e->locator, *loc) && !TransactionIdIsCurrentTransactionId(e->top_xid) &&
			sift_esc_modes_conflict(want, sift_esc_coarse_modes((LockTupleMode) e->mode)) && TransactionIdIsInProgress(e->top_xid))
			found = e->top_xid;
	}
	LWLockRelease(&shm->esc_locks[si].lock);
	return found;
}

static TransactionId
conflicting_xid(const RelFileLocator *loc, uint64 part_id, SiftEscModes want)
{
	TransactionId xid = holder_in_the_way(loc, part_id, want);

	return TransactionIdIsValid(xid) || part_id == SIFT_ESC_PART_ALL ? xid : holder_in_the_way(loc, SIFT_ESC_PART_ALL, want);
}

TransactionId
sift_esc_find_conflicting_xid(const RelFileLocator *loc, uint64 part_id, LockTupleMode mode)
{
	return conflicting_xid(loc, part_id, sift_esc_intent_modes(mode));
}

void
sift_esc_drop_rel(const RelFileLocator *loc)
{
	if (shm == NULL)
		return;
	for (int si = 0; si < SIFT_ESC_STRIPES; si++)
	{
		SiftEscStripe *s = &shm->esc_stripes[si];
		uint16	   *n = &shm->esc_n[si];

		LWLockAcquire(&shm->esc_locks[si].lock, LW_EXCLUSIVE);
		for (int i = 0; i < *n;)
			if (RelFileLocatorEquals(s->entries[i].locator, *loc))
				s->entries[i] = s->entries[--*n];
			else
				i++;
		LWLockRelease(&shm->esc_locks[si].lock);
	}
}

/* ---- what the transaction holds, by relation and partition ---- */

typedef struct SiftLocalLock
{
	RelFileLocator loc;
	uint64		part_id;		/* SIFT_ESC_PART_ALL: the relation, which also counts the partitions below it */
	Oid			dbid;
	Oid			relid;
	uint32		tier1_rows;
	uint32		retry_at;
	uint32		n_esc_parts;
	int8		intent_mode;	/* -1: none */
	int8		esc_mode;		/* -1: no coarse lock */
	int8		held_max;		/* the strongest entry taken, -1 for none */
	bool		split_pinned;
	bool		published;
	bool		prepared_marked;	/* PREPARE took the page lock that says the relation has row locks in the lock manager */
	uint8		prepared_probe;	/* of a relation: 0 not asked, 1 no prepared transaction holds row locks of it in the lock manager, 2 some does */
	SubTransactionId esc_sub;
	struct SiftLocalLock *rel_parts;	/* of a relation: the locks of its partitions, linked by rel_next */
	struct SiftLocalLock *rel_next;
} SiftLocalLock;

SIFT_BACKEND_LOCAL MemoryContext txn_cx = NULL;
SIFT_BACKEND_LOCAL SiftLocalLock **locks = NULL;
SIFT_BACKEND_LOCAL uint32 *lock_ix = NULL;
SIFT_BACKEND_LOCAL uint32 nlocks = 0,
			caplocks = 0,
			ixcap = 0;
SIFT_BACKEND_LOCAL SiftEscSubRecord *sublog = NULL;
SIFT_BACKEND_LOCAL uint32 *sub_bucket = NULL;
SIFT_BACKEND_LOCAL int nsub = 0,
			capsub = 0;

MemoryContext
sift_rowlock_txn_cx(void)
{
	if (txn_cx == NULL)
		/* MEM[MEM-TBL-09]: TopTransactionContext xact row-lock tracker (freed at xact end, 0 B idle) */
		txn_cx = sift_mem_allocset_create(TopTransactionContext, "sift f2 row locks of the transaction");
	return txn_cx;
}

static uint32
lock_hash(const RelFileLocator *loc, uint64 part_id)
{
	return hash_combine(hash_bytes((const unsigned char *) loc, sizeof(*loc)), (uint32) (part_id ^ (part_id >> 32)));
}

static SiftLocalLock *
find_lock(const RelFileLocator *loc, uint64 part_id)
{
	for (uint32 p = lock_hash(loc, part_id) & (ixcap - 1); ixcap > 0 && lock_ix[p] != 0; p = (p + 1) & (ixcap - 1))
	{
		SiftLocalLock *l = locks[lock_ix[p] - 1];

		if (l->part_id == part_id && RelFileLocatorEquals(l->loc, *loc))
			return l;
	}
	return NULL;
}

static void
index_lock(uint32 i)
{
	uint32		p = lock_hash(&locks[i]->loc, locks[i]->part_id) & (ixcap - 1);

	while (lock_ix[p] != 0)
		p = (p + 1) & (ixcap - 1);
	lock_ix[p] = i + 1;
}

/* check-memory: allow the locks of a transaction are the relations and partitions it holds, at most SIFT_ROWLOCK_REL_ROW_CAP and the escalated ones for each relation */
static void
grow_locks(void)
{
	MemoryContext cx = sift_rowlock_txn_cx();
	SiftLocalLock **old = locks;
	uint32	   *old_ix = lock_ix;

	caplocks = Max(64, caplocks * 2);
	ixcap = caplocks * 2;
	locks = MemoryContextAllocZero(cx, sizeof(SiftLocalLock *) * caplocks);
	lock_ix = MemoryContextAllocZero(cx, sizeof(uint32) * ixcap);
	if (old != NULL)
	{
		memcpy(locks, old, sizeof(SiftLocalLock *) * nlocks);
		pfree(old);
		pfree(old_ix);
	}
	for (uint32 i = 0; i < nlocks; i++)
		index_lock(i);
}

static SiftLocalLock *
lock_of(Relation rel, uint64 part_id)
{
	SiftLocalLock *l = find_lock(&rel->rd_locator, part_id),
			   *rl;

	if (l != NULL)
		return l;
	rl = part_id == SIFT_ESC_PART_ALL ? NULL : lock_of(rel, SIFT_ESC_PART_ALL);
	if (nlocks == caplocks)
		grow_locks();
	l = MemoryContextAllocZero(sift_rowlock_txn_cx(), sizeof(SiftLocalLock));
	*l = (SiftLocalLock) {.loc = rel->rd_locator,.part_id = part_id,.dbid = rel->rd_lockInfo.lockRelId.dbId,
		.relid = rel->rd_lockInfo.lockRelId.relId,.intent_mode = -1,.esc_mode = -1,.held_max = -1};
	if (rl != NULL)
	{
		l->rel_next = rl->rel_parts;
		rl->rel_parts = l;
	}
	locks[nlocks] = l;
	nlocks++;
	index_lock(nlocks - 1);
	return l;
}

static bool
rel_escalated(const SiftLocalLock *rl)
{
	return rl != NULL && (rl->esc_mode >= 0 || rl->n_esc_parts > 0);
}

bool
sift_esc_rel_is_escalated(const RelFileLocator *loc)
{
	return rel_escalated(find_lock(loc, SIFT_ESC_PART_ALL));
}

static void note_transition(const SiftLocalLock *l);

void
sift_esc_note_tier1_acquire(const RelFileLocator *loc, uint64 part_id, LockTupleMode mode, bool new_row)
{
	SiftLocalLock *pl = find_lock(loc, part_id),
			   *rl = find_lock(loc, SIFT_ESC_PART_ALL);

	Assert(pl != NULL && rl != NULL);
	pl->tier1_rows += new_row;
	rl->tier1_rows += new_row;
	if ((int8) mode > pl->held_max || (int8) mode > rl->held_max)
	{
		note_transition(pl);
		note_transition(rl);
	}
	pl->held_max = Max(pl->held_max, (int8) mode);
	rl->held_max = Max(rl->held_max, (int8) mode);
}

bool
sift_esc_prepared_locks_possible(Relation rel)
{
	SiftLocalLock *rl;

	if (max_prepared_xacts == 0)
		return false;
	rl = lock_of(rel, SIFT_ESC_PART_ALL);
	if (rl->prepared_probe == 0)
		rl->prepared_probe = sift_lock_2pc_present(rl->dbid, rl->relid) ? 2 : 1;
	return rl->prepared_probe == 2;
}

bool
sift_esc_prepared_ids(const RelFileLocator *loc, Oid *dbid, Oid *relid, bool *first)
{
	SiftLocalLock *rl = find_lock(loc, SIFT_ESC_PART_ALL);

	if (rl == NULL)
		return false;
	*dbid = rl->dbid;
	*relid = rl->relid;
	*first = !rl->prepared_marked;
	rl->prepared_marked = true;
	return true;
}

void
sift_esc_note_tier1_release(const RelFileLocator *loc, uint64 part_id, uint32 count)
{
	SiftLocalLock *pl = find_lock(loc, part_id),
			   *rl = find_lock(loc, SIFT_ESC_PART_ALL);

	if (pl != NULL)
		pl->tier1_rows -= Min(pl->tier1_rows, count);
	if (rl != NULL)
		rl->tier1_rows -= Min(rl->tier1_rows, count);
}

/* ---- the undo log of the subtransactions ---- */

/* A record of a subtransaction for a lock is found by a hash of both, in chains through the records: the records of the subtransaction that ends are the last ones, so each is the head of its chain */
static uint32
sub_bucket_of(SubTransactionId sub, const RelFileLocator *loc, uint64 part_id)
{
	return hash_combine(lock_hash(loc, part_id), sub * UINT32_C(0x9E3779B1)) & (pg_nextpower2_32(capsub * 2) - 1);
}

static void
sub_chain_push(uint32 i)
{
	uint32		b = sub_bucket_of(sublog[i].subxid, &sublog[i].locator, sublog[i].part_id);

	sublog[i].chain = sub_bucket[b];
	sub_bucket[b] = i + 1;
}

static void
sub_chain_pop(uint32 i)
{
	uint32		b = sub_bucket_of(sublog[i].subxid, &sublog[i].locator, sublog[i].part_id);

	SIFT_INVARIANT(sub_bucket[b] == i + 1, "the record %u of the row lock undo log is not the head of its chain", i);
	sub_bucket[b] = sublog[i].chain;
}

static SiftEscSubRecord *
sub_find(SubTransactionId sub, const RelFileLocator *loc, uint64 part_id)
{
	if (sub_bucket == NULL)
		return NULL;
	for (uint32 c = sub_bucket[sub_bucket_of(sub, loc, part_id)]; c != 0; c = sublog[c - 1].chain)
		if (sublog[c - 1].subxid == sub && sublog[c - 1].part_id == part_id && RelFileLocatorEquals(sublog[c - 1].locator, *loc))
			return &sublog[c - 1];
	return NULL;
}

/* check-memory: allow one record for each relation and partition a subtransaction changed, bounded like the locks */
static void
grow_sublog(void)
{
	capsub = Max(8, capsub * 2);
	sublog = sublog == NULL ? MemoryContextAlloc(sift_rowlock_txn_cx(), sizeof(SiftEscSubRecord) * capsub) : repalloc(sublog, sizeof(SiftEscSubRecord) * capsub);
	if (sub_bucket != NULL)
		pfree(sub_bucket);
	sub_bucket = MemoryContextAllocZero(sift_rowlock_txn_cx(), sizeof(uint32) * pg_nextpower2_32(capsub * 2));
	for (int i = 0; i < nsub; i++)
		sub_chain_push(i);
}

/* The record of the current subtransaction for the lock, made when the subtransaction first changes it: what it held before. The top level has none, it ends with everything */
static void
note_transition(const SiftLocalLock *l)
{
	SubTransactionId sub = GetCurrentSubTransactionId();

	if (GetCurrentTransactionNestLevel() <= 1 || sub_find(sub, &l->loc, l->part_id) != NULL)
		return;
	if (nsub == capsub)
		grow_sublog();
	sublog[nsub] = (SiftEscSubRecord) {.locator = l->loc,.part_id = l->part_id,.subxid = sub,.holder_xid = GetCurrentTransactionId(),
		.prev_intent = l->intent_mode,.prev_esc = l->esc_mode,.prev_held_max = l->held_max,.prev_esc_sub = l->esc_sub,
		.took_split_pin = false};
	sub_chain_push(nsub++);
}

static SiftEscSubRecord *
record_of(const SiftLocalLock *l)
{
	return sub_find(GetCurrentSubTransactionId(), &l->loc, l->part_id);
}

static DirArg
dir_arg(const SiftLocalLock *l)
{
	return (DirArg) {.loc = &l->loc,.part_id = l->part_id,.top = GetTopTransactionIdIfAny(),.cur = InvalidTransactionId,
		.subxid = GetCurrentSubTransactionId(),.mode = (LockTupleMode) Max(l->esc_mode, 0)};
}

static void
undo_record(const SiftEscSubRecord *r)
{
	SiftLocalLock *l = find_lock(&r->locator, r->part_id);
	SiftLocalLock *rl = find_lock(&r->locator, SIFT_ESC_PART_ALL);
	DirArg		a;

	if (l == NULL)
		return;
	a = dir_arg(l);
	a.mode = (LockTupleMode) Max(r->prev_esc, 0);
	if (r->prev_esc < 0 && l->esc_mode >= 0)
	{
		if (l->published)
			dir_apply(DIR_REMOVE, &a);
		if (r->part_id != SIFT_ESC_PART_ALL && rl != NULL)
			rl->n_esc_parts--;
		l->published = false;
	}
	else if (r->prev_esc >= 0 && l->esc_mode != r->prev_esc && l->published)
		dir_apply(DIR_MODE, &a);
	l->esc_mode = r->prev_esc;
	l->esc_sub = r->prev_esc_sub;
	l->held_max = r->prev_held_max;
	l->intent_mode = r->prev_intent;
	if (r->took_split_pin && rl != NULL)
		rl->split_pinned = false;
}

/* The coarse locks the subtransaction took are its parent's now */
static void
retag_coarse(const SiftEscSubRecord *r, SubTransactionId mysub, SubTransactionId parent)
{
	SiftLocalLock *l = find_lock(&r->locator, r->part_id);

	if (l != NULL && l->esc_sub == mysub && l->esc_mode >= 0)
	{
		DirArg		a = dir_arg(l);

		l->esc_sub = parent;
		a.subxid = parent;
		if (l->published)
			dir_apply(DIR_RETAG, &a);
	}
}

/* The records of the subtransaction and of those released into it are the last of the log: undone when it rolls back, the parent's when it commits (the parent's own record of a lock stands) */
static void
drop_sub_records(SubTransactionId mysub, SubTransactionId parent, bool abort)
{
	int			first = nsub,
				kept;

	while (first > 0 && sublog[first - 1].subxid == mysub)
		first--;
	for (int i = nsub - 1; i >= first; i--)
	{
		if (abort)
			undo_record(&sublog[i]);
		else
			retag_coarse(&sublog[i], mysub, parent);
		sub_chain_pop(i);
	}
	kept = first;
	for (int i = first; i < nsub && !abort; i++)
	{
		SiftEscSubRecord r = sublog[i];

		r.subxid = parent;
		if (sub_find(parent, &r.locator, r.part_id) != NULL)
			continue;
		sublog[kept] = r;
		sub_chain_push(kept++);
	}
	nsub = kept;
}

void
sift_esc_subxact_event(SubXactEvent event, SubTransactionId mysubid, SubTransactionId parentsubid)
{
	if (event == SUBXACT_EVENT_ABORT_SUB || event == SUBXACT_EVENT_COMMIT_SUB)
		drop_sub_records(mysubid, parentsubid, event == SUBXACT_EVENT_ABORT_SUB);
}

/* ---- the locks of the lock manager ---- */

static LOCKTAG
page_tag(const SiftLocalLock *l, BlockNumber blkno)
{
	LOCKTAG		tag;

	SET_LOCKTAG_PAGE(tag, l->dbid, l->relid, blkno);
	return tag;
}

static BlockNumber
block_of(const SiftLocalLock *l)
{
	return l->part_id == SIFT_ESC_PART_ALL ? SIFT_LOCK_REL_BULK_BLKNO : SIFT_LOCK_PART_ESC_BLKNO(l->part_id);
}

static BlockNumber
key_block_of(const SiftLocalLock *l)
{
	return l->part_id == SIFT_ESC_PART_ALL ? SIFT_LOCK_REL_KEY_BLKNO : SIFT_LOCK_PART_KEY_BLKNO(l->part_id);
}

/* The lock if it is free, else, where the policy waits, after the wait (*waited); TM_BeingModified where it does not */
static TM_Result
take(const LOCKTAG *tag, LOCKMODE mode, LockWaitPolicy wait_policy, bool *waited)
{
	if (LockAcquire(tag, mode, false, true) != LOCKACQUIRE_NOT_AVAIL)
		return TM_Ok;
	if (wait_policy != LockWaitBlock)
		return TM_BeingModified;
	(void) LockAcquire(tag, mode, false, false);
	*waited = true;
	return TM_Ok;
}

/* The modes on the key block, then on the row block: a request that is refused on the row block gives back what it took on the key block */
static TM_Result
take_modes(const SiftLocalLock *l, SiftEscModes modes, LockWaitPolicy wait_policy, bool *waited)
{
	LOCKTAG		key = page_tag(l, key_block_of(l));
	LOCKTAG		row = page_tag(l, block_of(l));
	TM_Result	r = TM_Ok;
	bool		key_taken = false;

	if (modes.key != NoLock)
	{
		r = take(&key, modes.key, wait_policy, waited);
		key_taken = r == TM_Ok;
	}
	if (r == TM_Ok && modes.row != NoLock)
	{
		r = take(&row, modes.row, wait_policy, waited);
		if (r != TM_Ok && key_taken)
			(void) LockRelease(&key, modes.key, false);
	}
	return r;
}

static TM_Result
take_intent(SiftLocalLock *l, LockTupleMode mode, LockWaitPolicy wait_policy, bool *reread, TM_FailureData *tmfd)
{
	SiftEscModes want = sift_esc_intent_modes(mode);
	TM_Result	r;

	if (l->intent_mode >= (int8) mode)
		return TM_Ok;
	r = take_modes(l, want, wait_policy, reread);
	if (r != TM_Ok)
	{
		tmfd->xmax = conflicting_xid(&l->loc, l->part_id, want);
		return r;
	}
	note_transition(l);
	l->intent_mode = Max(l->intent_mode, (int8) mode);
	return TM_Ok;
}

TM_Result
sift_esc_before_row_lock(Relation rel, uint64 part_id, LockTupleMode mode, LockWaitPolicy wait_policy, bool *covered,
						 bool *reread, TM_FailureData *tmfd)
{
	SiftLocalLock *rl = lock_of(rel, SIFT_ESC_PART_ALL);
	SiftLocalLock *pl = lock_of(rel, part_id);
	TM_Result	r;

	*covered = rl->esc_mode >= (int) mode || pl->esc_mode >= (int) mode;
	*reread = false;
	if (*covered)
		return TM_Ok;
	if (sift_lock_shm_attach() == NULL || rl->esc_mode >= 0 || pl->esc_mode >= 0)
		return TM_Ok;
	r = take_intent(rl, mode, wait_policy, reread, tmfd);
	return r == TM_Ok ? take_intent(pl, mode, wait_policy, reread, tmfd) : r;
}

bool
sift_esc_should_escalate(const RelFileLocator *loc, uint64 part_id, LockTupleMode mode, bool stripe_full)
{
	const SiftLocalLock *pl = find_lock(loc, part_id),
			   *rl = find_lock(loc, SIFT_ESC_PART_ALL);

	if (pglm || stripe_full)
		return true;
	if ((rl->esc_mode >= 0 && (int) mode > rl->esc_mode) || (pl->esc_mode >= 0 && (int) mode > pl->esc_mode))
		return true;
	return (pl->tier1_rows >= SIFT_ROWLOCK_ESCALATE_AT && pl->tier1_rows >= pl->retry_at) ||
		(rl->tier1_rows >= SIFT_ROWLOCK_REL_ROW_CAP && rl->tier1_rows >= rl->retry_at);
}

/* ---- holding a partition or a relation ---- */

/* The split pin: no split or merge of the partitions of the relation while the transaction holds a partition (core/part_lock.h) */
static TM_Result
pin_splits(SiftLocalLock *rl, LockWaitPolicy wait_policy, bool *waited)
{
	LOCKTAG		tag = page_tag(rl, SIFT_F2_SPLIT_LOCK_BLKNO);
	TM_Result	r;

	if (rl->split_pinned)
		return TM_Ok;
	r = take(&tag, ShareLock, wait_policy, waited);
	if (r == TM_Ok)
	{
		SiftEscSubRecord *rec;

		note_transition(rl);
		rec = record_of(rl);
		if (rec != NULL)
			rec->took_split_pin = true;
		rl->split_pinned = true;
	}
	return r;
}

static void
absorb_partitions(const SiftLocalLock *rl)
{
	SubTransactionId sub = GetCurrentSubTransactionId();

	for (SiftLocalLock *l = rl->rel_parts; l != NULL; l = l->rel_next)
	{
		if (l->published && l->esc_sub == sub)
		{
			DirArg		a = dir_arg(l);

			dir_apply(DIR_REMOVE, &a);
			l->published = false;
		}
	}
}

static void
publish(SiftLocalLock *t)
{
	DirArg		a = dir_arg(t);

	a.cur = GetCurrentTransactionId();
	t->published = dir_apply(DIR_PUBLISH, &a);
}

TM_Result
sift_esc_promote(Relation rel, uint64 part_id, uint32 nparts, LockTupleMode mode, LockWaitPolicy wait_policy, bool stripe_full,
				 SiftTier1ReleaseCb release_cb, bool *escalated, bool *reread, TM_FailureData *tmfd)
{
	SiftLocalLock *rl = lock_of(rel, SIFT_ESC_PART_ALL);
	SiftLocalLock *pl = lock_of(rel, part_id);
	bool		whole = rl->esc_mode >= 0 || rl->n_esc_parts >= SIFT_ROWLOCK_PART_TO_REL_AT || nparts <= 1;
	SiftLocalLock *t = whole ? rl : pl;
	LockTupleMode esc_mode = (LockTupleMode) Max(Max((int) mode, t->held_max), t->esc_mode);
	bool		must = stripe_full || pglm || (t->esc_mode >= 0 && (int) mode > t->esc_mode);
	SiftEscModes want = sift_esc_coarse_modes(esc_mode);
	TM_Result	r = TM_Ok;

	*escalated = false;
	*reread = false;
	if (!whole && !rl->split_pinned)
	{
		r = pin_splits(rl, wait_policy, reread);
		*reread = r == TM_Ok;
		if (r == TM_Ok)
			return TM_Ok;
	}
	if (r == TM_Ok)
		r = take_modes(t, want, wait_policy, reread);
	if (r != TM_Ok && !must)
	{
		pl->retry_at = pl->tier1_rows + SIFT_ROWLOCK_RETRY_ROWS;
		rl->retry_at = rl->tier1_rows + SIFT_ROWLOCK_RETRY_ROWS;
		return TM_Ok;
	}
	if (r != TM_Ok)
	{
		tmfd->xmax = conflicting_xid(&t->loc, t->part_id, want);
		return r;
	}
	note_transition(t);
	if (!whole && t->esc_mode < 0)
		rl->n_esc_parts++;
	t->esc_mode = esc_mode;
	t->esc_sub = GetCurrentSubTransactionId();
	publish(t);
	if (whole)
		absorb_partitions(rl);
	release_cb(&t->loc, t->part_id, GetCurrentSubTransactionId());
	*escalated = true;
	*reread = true;
	return TM_Ok;
}

/* ---- the transaction ends or prepares ---- */

void
sift_esc_pre_prepare(SiftTier1ReleaseCb release_cb)
{
	bool		waited = false;

	for (uint32 i = 0; i < nlocks; i++)
	{
		SiftLocalLock *l = locks[i];
		SiftLocalLock *rl = find_lock(&l->loc, SIFT_ESC_PART_ALL);

		if (l->part_id == SIFT_ESC_PART_ALL || l->tier1_rows == 0 || l->esc_mode >= 0 || rl->esc_mode >= 0 || shm == NULL)
			continue;
		if (pin_splits(rl, LockWaitSkip, &waited) == TM_Ok)
		{
			LockTupleMode mode = (LockTupleMode) l->held_max;

			if (take_modes(l, sift_esc_coarse_modes(mode), LockWaitSkip, &waited) == TM_Ok)
			{
				l->esc_mode = mode;
				l->esc_sub = GetCurrentSubTransactionId();
				rl->n_esc_parts++;
				publish(l);
				release_cb(&l->loc, l->part_id, GetCurrentSubTransactionId());
			}
		}
	}
	for (uint32 i = 0; i < nlocks; i++)
		if (locks[i]->published)
		{
			DirArg		a = dir_arg(locks[i]);

			dir_apply(DIR_PREPARED, &a);
		}
}

void
sift_esc_xact_end(XactEvent event)
{
	for (uint32 i = 0; i < nlocks && event != XACT_EVENT_PREPARE; i++)
		if (locks[i]->published)
		{
			DirArg		a = dir_arg(locks[i]);

			dir_apply(DIR_REMOVE, &a);
		}
	sift_rowlock_area_detach();
	if (txn_cx != NULL)
		MemoryContextDelete(txn_cx);
	txn_cx = NULL;
	locks = NULL;
	lock_ix = NULL;
	sublog = NULL;
	sub_bucket = NULL;
	nlocks = caplocks = ixcap = 0;
	nsub = capsub = 0;
	pglm = false;
}
