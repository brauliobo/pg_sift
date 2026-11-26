/*
 * core/quarantine/flags.c - the flag of the metapage of an index that a backend has read, kept until the relcache entry of the index goes (core/quarantine/flags.h).
 */
#include "postgres.h"

#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/cachebudget.h"
#include "core/meta_peek.h"
#include "core/page.h"
#include "core/quarantine/flags.h"
#include "core/sift.h"

typedef struct FlagMemo
{
	Oid			index;
	bool		flagged;
	uint64		used;
} FlagMemo;

/* In the memory of the relcache entry of the index, where the relcache frees it when it invalidates the entry: the flag of the metapage is forgotten then */
typedef struct FlagForget
{
	MemoryContextCallback cb;
	Oid			index;
} FlagForget;

static HTAB *flag_memo;
static MemoryContext flag_cx;

/*
 * The flag of a metapage changes with a commit that invalidates the relcache entry of its index (the worker that writes it, a write that skips the index, a REINDEX), so the backend reads it
 * once in between and not at every plan. No relcache callback for it: a backend has ten of those slots for all its libraries.
 */
static void
flag_forget(void *arg)
{
	Oid			index = ((FlagForget *) arg)->index;

	if (flag_memo != NULL)
		hash_search(flag_memo, &index, HASH_REMOVE, NULL);
}

static FlagMemo *
oldest_entry(void)
{
	HASH_SEQ_STATUS it;
	FlagMemo   *m,
			   *oldest = NULL;

	if (flag_memo == NULL)
		return NULL;
	hash_seq_init(&it, flag_memo);
	while ((m = hash_seq_search(&it)) != NULL)
		if (oldest == NULL || m->used < oldest->used)
			oldest = m;
	return oldest;
}

static uint64
flag_oldest(void)
{
	FlagMemo   *m = oldest_entry();

	return m ? m->used : UINT64_MAX;
}

static bool
flag_evict_one(void)
{
	FlagMemo   *m = oldest_entry();
	Oid			index;

	if (m == NULL)
		return false;
	index = m->index;
	hash_search(flag_memo, &index, HASH_REMOVE, NULL);
	return true;
}

static void
flag_evict_all(void)
{
	if (flag_memo == NULL)
		return;
	hash_destroy(flag_memo);
	flag_memo = NULL;
}

static const SiftCacheKind flag_kind = {"core_quarantine_flags", flag_oldest, flag_evict_one, flag_evict_all};

/* An entry that the budget dropped is made again for the same entry of the relcache: its context has the callback already */
static bool
forget_registered(MemoryContext cx)
{
	for (MemoryContextCallback *cb = cx->reset_cbs; cb != NULL; cb = cb->next)
		if (cb->func == flag_forget)
			return true;
	return false;
}

bool
sift_quar_meta_flagged(Relation index)
{
	Oid			relid = RelationGetRelid(index);
	FlagMemo   *m;
	FlagForget *f;
	bool		flagged;

	if (flag_cx == NULL)
	{
		flag_cx = sift_cache_create_context("sift cache quarantine flags");
		sift_cache_register(&flag_kind);
	}
	if (flag_memo == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(FlagMemo),.hcxt = flag_cx};

		/* MEM[MEM-IDLE-20]: cachebudget quarantine flag_memo HTAB (16 init; registered evictable) */
		flag_memo = hash_create("sift quarantine flags", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if ((m = hash_search(flag_memo, &relid, HASH_FIND, NULL)) != NULL)
	{
		m->used = sift_cache_tick();
		return m->flagged;
	}
	flagged = (sift_meta_peek_format(index).flags & SIFT_META_QUARANTINED) != 0;
	m = hash_search(flag_memo, &relid, HASH_ENTER, NULL);
	m->flagged = flagged;
	m->used = sift_cache_tick();
	if (!forget_registered(index->rd_indexcxt))
	{
		f = MemoryContextAlloc(index->rd_indexcxt, sizeof(FlagForget));
		f->cb.func = flag_forget;
		f->cb.arg = f;
		f->index = relid;
		MemoryContextRegisterResetCallback(index->rd_indexcxt, &f->cb);
	}
	return flagged;
}
