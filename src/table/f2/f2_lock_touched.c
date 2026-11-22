/*
 * table/f2/f2_lock_touched.c - the rows a transaction holds in tier 1 of the row locks (f2_lock.c): a dense array to settle at its end, a hash of it for the lookup of a row, and
 * a list for each subtransaction that holds some. Removing an entry moves the last one into its place, so that every operation costs a probe and the end of a subtransaction costs
 * the rows it holds.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "utils/memutils.h"

#include "core/host.h"
#include "table/f2/f2_lock_int.h"

#define TOUCHED_INDEXED_AT	32	/* entries from which a lookup by hash replaces the scan */

typedef struct SubList
{
	SubTransactionId sub;
	int32		head;
} SubList;

SIFT_BACKEND_LOCAL SiftTouched *touched = NULL;
SIFT_BACKEND_LOCAL uint32 ntouched = 0,
			captouched = 0,
			ixcap = 0;
SIFT_BACKEND_LOCAL uint32 *touched_ix = NULL;

/* The subtransactions that hold rows, outermost first: the one that locks or ends is the last, as nothing locks while a subtransaction it began runs */
SIFT_BACKEND_LOCAL SubList *subs = NULL;
SIFT_BACKEND_LOCAL uint32 nsubs = 0,
			capsubs = 0;

static uint32
touched_hash(const SiftRowLockKey *k)
{
	return hash_bytes((const unsigned char *) k, sizeof(*k));
}

static bool
same(const SiftTouched *t, const SiftRowLockKey *k, SubTransactionId sub)
{
	return t->subxid == sub && memcmp(&t->key, k, sizeof(*k)) == 0;
}

static void
ix_put(uint32 i)
{
	uint32		p = touched_hash(&touched[i].key) & (ixcap - 1);

	while (touched_ix[p] != 0)
		p = (p + 1) & (ixcap - 1);
	touched_ix[p] = i + 1;
}

/* The slot that holds entry i, found from its key */
static uint32
ix_slot_of(uint32 i)
{
	uint32		p = touched_hash(&touched[i].key) & (ixcap - 1);

	while (touched_ix[p] != i + 1)
		p = (p + 1) & (ixcap - 1);
	return p;
}

/* Empties slot p and moves back the entries that follow in its cluster and would not be found past the gap */
static void
ix_delete_slot(uint32 p)
{
	uint32		mask = ixcap - 1,
				i = p,
				j = p;

	for (;;)
	{
		touched_ix[i] = 0;
		for (;;)
		{
			uint32		home;

			j = (j + 1) & mask;
			if (touched_ix[j] == 0)
				return;
			home = touched_hash(&touched[touched_ix[j] - 1].key) & mask;
			if (!(i <= j ? (i < home && home <= j) : (i < home || home <= j)))
				break;
		}
		touched_ix[i] = touched_ix[j];
		i = j;
	}
}

static void
reindex(void)
{
	if (touched_ix != NULL)
		pfree(touched_ix);
	touched_ix = NULL;
	ixcap = 0;
	if (ntouched < TOUCHED_INDEXED_AT)
		return;
	ixcap = pg_nextpower2_32(Max(ntouched * 2, 128));
	touched_ix = MemoryContextAllocZero(sift_rowlock_txn_cx(), sizeof(uint32) * ixcap);
	for (uint32 i = 0; i < ntouched; i++)
		ix_put(i);
}

static bool
is_touched(const SiftRowLockKey *k, SubTransactionId sub)
{
	if (ixcap == 0)
	{
		for (uint32 i = 0; i < ntouched; i++)
			if (same(&touched[i], k, sub))
				return true;
		return false;
	}
	for (uint32 p = touched_hash(k) & (ixcap - 1); touched_ix[p] != 0; p = (p + 1) & (ixcap - 1))
		if (same(&touched[touched_ix[p] - 1], k, sub))
			return true;
	return false;
}

/* ---- the lists of the subtransactions ---- */

static SubList *
sub_list(SubTransactionId sub)
{
	for (uint32 i = nsubs; i > 0; i--)
		if (subs[i - 1].sub == sub)
			return &subs[i - 1];
	return NULL;
}

static SubList *
sub_list_for_add(SubTransactionId sub)
{
	if (nsubs > 0 && subs[nsubs - 1].sub == sub)
		return &subs[nsubs - 1];
	SIFT_INVARIANT(nsubs == 0 || subs[nsubs - 1].sub < sub, "a row lock was taken in subtransaction %u while one that began later holds rows", (unsigned) sub);
	if (nsubs == capsubs)
	{
		/* check-memory: allow the subtransactions that hold rows are bounded by the nesting of the transaction, which max_stack_depth bounds */
		capsubs = Max(8, capsubs * 2);
		subs = subs == NULL ? MemoryContextAlloc(sift_rowlock_txn_cx(), sizeof(SubList) * capsubs) : repalloc(subs, sizeof(SubList) * capsubs);
	}
	subs[nsubs] = (SubList) {sub, -1};
	return &subs[nsubs++];
}

static void
link_front(SubList *l, uint32 i)
{
	touched[i].sub_prev = -1;
	touched[i].sub_next = l->head;
	if (l->head >= 0)
		touched[l->head].sub_prev = (int32) i;
	l->head = (int32) i;
}

static void
unlink_entry(uint32 i)
{
	SiftTouched *t = &touched[i];

	if (t->sub_prev >= 0)
		touched[t->sub_prev].sub_next = t->sub_next;
	else
		sub_list(t->subxid)->head = t->sub_next;
	if (t->sub_next >= 0)
		touched[t->sub_next].sub_prev = t->sub_prev;
}

/* Entry i has moved to a new position: its neighbours and the head of its list name it there */
static void
relink_moved(uint32 i)
{
	SiftTouched *t = &touched[i];

	if (t->sub_prev >= 0)
		touched[t->sub_prev].sub_next = (int32) i;
	else
		sub_list(t->subxid)->head = (int32) i;
	if (t->sub_next >= 0)
		touched[t->sub_next].sub_prev = (int32) i;
}

/* ---- the table ---- */

bool
sift_touched_add(const SiftRowLockKey *k, uint64 part_id)
{
	SubTransactionId sub = GetCurrentSubTransactionId();

	if (is_touched(k, sub))
		return false;
	if (ntouched == captouched)
	{
		/* check-memory: allow the rows of a transaction in tier 1 are bounded by SIFT_ROWLOCK_REL_ROW_CAP for each relation, the stripes hold no more */
		captouched = Max(64, captouched * 2);
		touched = touched == NULL ? MemoryContextAlloc(sift_rowlock_txn_cx(), sizeof(SiftTouched) * captouched) : repalloc(touched, sizeof(SiftTouched) * captouched);
	}
	touched[ntouched] = (SiftTouched) {.key = *k,.part_id = part_id,.xid = GetCurrentTransactionId(),.subxid = sub};
	link_front(sub_list_for_add(sub), ntouched);
	ntouched++;
	if ((ixcap == 0 && ntouched >= TOUCHED_INDEXED_AT) || (ixcap != 0 && ntouched * 4 > ixcap * 3))
		reindex();
	else if (ixcap != 0)
		ix_put(ntouched - 1);
	return true;
}

/* The latest entry of the row, whichever subtransaction holds it, or -1 */
static int32
latest_of(const SiftRowLockKey *k)
{
	int32		at = -1;

	if (ixcap == 0)
	{
		for (int32 i = (int32) ntouched - 1; i >= 0 && at < 0; i--)
			if (memcmp(&touched[i].key, k, sizeof(*k)) == 0)
				at = i;
		return at;
	}
	for (uint32 p = touched_hash(k) & (ixcap - 1); touched_ix[p] != 0; p = (p + 1) & (ixcap - 1))
		if (memcmp(&touched[touched_ix[p] - 1].key, k, sizeof(*k)) == 0)
			at = Max(at, (int32) touched_ix[p] - 1);
	return at;
}

int32
sift_touched_remove(uint32 i)
{
	uint32		last = ntouched - 1;
	int32		moved = -1;

	unlink_entry(i);
	if (ixcap != 0)
		ix_delete_slot(ix_slot_of(i));
	if (i != last)
	{
		if (ixcap != 0)
			touched_ix[ix_slot_of(last)] = i + 1;
		touched[i] = touched[last];
		relink_moved(i);
		moved = (int32) last;
	}
	ntouched = last;
	return moved;
}

void
sift_touched_forget(const SiftRowLockKey *k)
{
	int32		at = latest_of(k);

	if (at >= 0)
		(void) sift_touched_remove((uint32) at);
}

uint32
sift_touched_count(void)
{
	return ntouched;
}

const SiftTouched *
sift_touched_at(uint32 i)
{
	return &touched[i];
}

int32
sift_touched_sub_first(SubTransactionId sub)
{
	SubList    *l = sub_list(sub);

	return l == NULL ? -1 : l->head;
}

int32
sift_touched_next(uint32 i)
{
	return touched[i].sub_next;
}

void
sift_touched_promote(SubTransactionId sub, SubTransactionId parent, void (*retag) (const SiftTouched *t, SubTransactionId parent))
{
	SubList    *l;
	SubList    *above;
	int32		tail = -1;

	if (nsubs == 0 || subs[nsubs - 1].sub != sub)
		return;
	l = &subs[nsubs - 1];
	for (int32 i = l->head; i >= 0; i = touched[i].sub_next)
	{
		retag(&touched[i], parent);
		touched[i].subxid = parent;
		tail = i;
	}
	above = nsubs >= 2 && subs[nsubs - 2].sub == parent ? &subs[nsubs - 2] : NULL;
	if (above == NULL)
	{
		l->sub = parent;
		return;
	}
	if (tail >= 0)
	{
		touched[tail].sub_next = above->head;
		if (above->head >= 0)
			touched[above->head].sub_prev = tail;
		above->head = l->head;
	}
	nsubs--;
}

void
sift_touched_sub_close(SubTransactionId sub)
{
	if (nsubs > 0 && subs[nsubs - 1].sub == sub)
	{
		SIFT_INVARIANT(subs[nsubs - 1].head < 0, "subtransaction %u ended holding rows of tier 1", (unsigned) sub);
		nsubs--;
	}
}

void
sift_touched_reset(void)
{
	touched = NULL;
	touched_ix = NULL;
	subs = NULL;
	ntouched = captouched = ixcap = nsubs = capsubs = 0;
}
