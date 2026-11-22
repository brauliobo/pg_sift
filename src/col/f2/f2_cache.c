/*
 * col/f2/f2_cache.c - the per-backend snapshot of the partition directory and the part lists of an index of format major 2 (f2_cache.h).
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "col/f2/col_meta.h"
#include "col/f2/f2_cache.h"
#include "core/memtrack.h"
#include "core/cachebudget.h"
#include "core/shm_manifest.h"

#ifdef SIFT_TESTING
SiftPlanCacheStats sift_plan_cache_stats;
#define PLAN_STAT(f, n)		(sift_plan_cache_stats.f += (n))
#else
#define PLAN_STAT(f, n)		((void) 0)
#endif

typedef struct Unpin
{
	MemoryContextCallback cb;
	SiftPlanSnapshot *snap;		/* stays where it is while it is pinned: only a snapshot nobody pins is dropped or reloaded */
} Unpin;

static HTAB *cache;
static MemoryContext cache_cx;
static dlist_head lru = DLIST_STATIC_INIT(lru);

static void
drop(SiftPlanSnapshot *s)
{
	dlist_delete(&s->node);
	MemoryContextDelete(s->cx);
	hash_search(cache, &s->locator, HASH_REMOVE, NULL);
}

static SiftPlanSnapshot *
victim(void)
{
	dlist_iter	it;

	dlist_foreach(it, &lru)
	{
		SiftPlanSnapshot *s = dlist_container(SiftPlanSnapshot, node, it.cur);

		if (s->pins == 0)
			return s;
	}
	return NULL;
}

static uint64
plan_oldest(void)
{
	SiftPlanSnapshot *s = victim();

	return s ? s->used : UINT64_MAX;
}

static bool
plan_evict_one(void)
{
	SiftPlanSnapshot *s = victim();

	if (s == NULL)
		return false;
	drop(s);
	return true;
}

void
sift_plan_cache_evict_all(void)
{
	while (plan_evict_one())
		;
}

static const SiftCacheKind plan_kind = {"col_f2_part_cache", plan_oldest, plan_evict_one, sift_plan_cache_evict_all};

static void
cache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(RelFileLocator),.entrysize = sizeof(SiftPlanSnapshot)};

	cache_cx = sift_cache_create_context("sift cache plan snapshots");
	ctl.hcxt = cache_cx;
	cache = hash_create("sift cache plan snapshots", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sift_cache_register(&plan_kind);
}

static void
unpin(void *arg)
{
	((Unpin *) arg)->snap->pins--;
}

static const SiftPlanSnapshot *
pin(SiftPlanSnapshot *s, MemoryContext mcxt)
{
	Unpin	   *u = MemoryContextAlloc(mcxt, sizeof(Unpin));

	s->pins++;
	s->used = sift_cache_tick();
	dlist_move_tail(&lru, &s->node);
	u->cb = (MemoryContextCallback) {unpin, u, NULL};
	u->snap = s;
	MemoryContextRegisterResetCallback(mcxt, &u->cb);
	return s;
}

/* The lists of s that are those of the partition entries, in the order of the directory: a partition keeps its place among the others, so the walk over the old lists only moves forward */
static void
reuse_lists(const SiftPlanSnapshot *s, const SiftManifestRead *mr, SiftPlanPartList *out, bool *reused)
{
	int			from = 0;

	for (int i = 0; i < mr->n; i++)
	{
		int			k = from;

		while (k < s->npartitions && s->partlists[k].part_id != mr->entries[i].part_id)
			k++;
		if (k == s->npartitions)
			continue;
		from = k + 1;
		if (s->partlists[k].manifest_gen == mr->entries[i].manifest_gen)
		{
			out[i] = s->partlists[k];
			reused[k] = true;
		}
	}
}

/* The part list of one partition in the shared snapshot: the parts first, then the generation that says they are all there */
typedef struct ShmPartList
{
	uint64		manifest_gen;
	uint32		nparts;
	uint32		pad;
	SiftPartDesc parts[FLEXIBLE_ARRAY_MEMBER];
} ShmPartList;

static bool
shm_list_get(const SiftShmManifestSnap *shm, int i, const SiftPartitionEntry *e, SiftPlanPartList *out)
{
	const ShmPartList *l = sift_shm_snap_part_list(shm, i);

	if (l == NULL || e->nparts == 0 || ((const volatile ShmPartList *) l)->manifest_gen != e->manifest_gen)
		return false;
	pg_read_barrier();
	out->nparts = l->nparts;
	out->parts = palloc(l->nparts * sizeof(SiftPartDesc));
	memcpy(out->parts, l->parts, l->nparts * sizeof(SiftPartDesc));
	return true;
}

static void
shm_list_put(const SiftShmManifestSnap *shm, int i, const SiftPlanPartList *list)
{
	ShmPartList *l = sift_shm_snap_part_list_install(shm, i, offsetof(ShmPartList, parts) + list->nparts * sizeof(SiftPartDesc));

	if (l == NULL || ((volatile ShmPartList *) l)->manifest_gen == list->manifest_gen)
		return;
	memcpy(l->parts, list->parts, list->nparts * sizeof(SiftPartDesc));
	l->nparts = list->nparts;
	pg_write_barrier();
	((volatile ShmPartList *) l)->manifest_gen = list->manifest_gen;
}

/* The part list of one partition in dst, exactly sized; the page copies of the read are the caller's scratch in src and go with it */
static SiftPartDesc *
read_parts_exact(MemoryContext dst, MemoryContext src, Relation rel, const SiftPartitionEntry *entry, int *nparts)
{
	MemoryContext cur = MemoryContextSwitchTo(src);
	SiftPartDesc *read = sift_manifest_read_parts(rel, entry, nparts);
	SiftPartDesc *kept = MemoryContextAlloc(dst, Max(*nparts, 1) * sizeof(SiftPartDesc));

	MemoryContextSwitchTo(cur);
	memcpy(kept, read, *nparts * sizeof(SiftPartDesc));
	pfree(read);
	return kept;
}

/* The entries of the partitions and the scope refs of the version the registry holds a snapshot of, else those the pages hold (and the registry gets them for the next backend) */
static SiftManifestRead
read_directory(Relation rel, const SiftShmIndexMeta *meta, const SiftShmManifestSnap **shm, SiftManifestScopeRef **scopes, int *nscopes)
{
	SiftManifestRead mr;

	*shm = sift_shm_snap_pin(&rel->rd_locator);
	if (*shm != NULL && (*shm)->manifest_version != meta->manifest_version)
	{
		sift_shm_snap_unpin(*shm);
		*shm = NULL;
	}
	if (*shm != NULL)
	{
		mr = (SiftManifestRead) {.n = (*shm)->npartitions,.version = (*shm)->manifest_version};
		mr.entries = palloc(Max(mr.n, 1) * sizeof(SiftPartitionEntry));
		memcpy(mr.entries, sift_shm_snap_part_entries(*shm), mr.n * sizeof(SiftPartitionEntry));
		*nscopes = (*shm)->nscopes;
		*scopes = palloc(Max(*nscopes, 1) * sizeof(SiftManifestScopeRef));
		if (*nscopes > 0)
			memcpy(*scopes, sift_shm_snap_scopes(*shm), *nscopes * sizeof(SiftManifestScopeRef));
		PLAN_STAT(shm_loads, 1);
		return mr;
	}
	mr = sift_manifest_read_range(rel, NULL, NULL);
	*scopes = sift_manifest_read_scope_dir(rel, meta->scope_dir_blk, meta->nscopes, meta->scope_dir_gen, nscopes);
	if ((meta->state_flags & SIFT_SHM_SLOT_VALID) && !(meta->state_flags & SIFT_SHM_SLOT_HAS_SNAP) && mr.version == meta->manifest_version)
	{
		SiftShmSnapSpec spec = {.manifest_version = meta->manifest_version,.scope_dir_gen = meta->scope_dir_gen,.npartitions = mr.n,.nscopes = *nscopes,.part_entries = mr.entries,
		.part_entry_size = sizeof(SiftPartitionEntry),.scopes = *scopes,.scope_entry_size = sizeof(SiftManifestScopeRef)};

		if (sift_shm_manifest_publish(meta, &spec))
		{
			*shm = sift_shm_snap_pin(&rel->rd_locator);
			PLAN_STAT(shm_publishes, 1);
		}
	}
	return mr;
}

/* Reads the directory and the part lists that moved since s was loaded into s->cx; s is left as it was when a read fails */
static void
load(SiftPlanSnapshot *s, Relation rel, const SiftShmIndexMeta *meta)
{
	const SiftShmManifestSnap *shm;
	SiftManifestScopeRef *scopes;
	int			nscopes;
	SiftManifestRead mr = read_directory(rel, meta, &shm, &scopes, &nscopes);
	MemoryContext prev = MemoryContextSwitchTo(s->cx);
	SiftPlanPartList *lists = palloc0(Max(mr.n, 1) * sizeof(SiftPlanPartList));
	bool	   *reused = palloc0(Max(s->npartitions, 1));
	SiftPartitionEntry *entries = palloc(Max(mr.n, 1) * sizeof(SiftPartitionEntry));
	SiftManifestScopeRef *kept = palloc(Max(nscopes, 1) * sizeof(SiftManifestScopeRef));

	reuse_lists(s, &mr, lists, reused);
	for (int i = 0; i < mr.n; i++)
	{
		if (lists[i].parts != NULL)
			continue;
		lists[i].part_id = mr.entries[i].part_id;
		lists[i].manifest_gen = mr.entries[i].manifest_gen;
		if (shm != NULL && shm_list_get(shm, i, &mr.entries[i], &lists[i]))
			continue;
		lists[i].parts = read_parts_exact(s->cx, prev, rel, &mr.entries[i], &lists[i].nparts);
		PLAN_STAT(partlists_read, 1);
		if (shm != NULL && lists[i].nparts > 0)
			shm_list_put(shm, i, &lists[i]);
	}
	for (int k = 0; k < s->npartitions; k++)
		if (!reused[k])
			pfree(s->partlists[k].parts);
	if (s->card_view != NULL)
		pfree(s->card_view);
	s->card_view = NULL;
	s->card_built = false;
	if (s->partlists != NULL)
	{
		pfree(s->partlists);
		pfree(s->partitions);
		pfree(s->scopes);
	}
	memcpy(entries, mr.entries, mr.n * sizeof(SiftPartitionEntry));
	if (nscopes > 0)
		memcpy(kept, scopes, nscopes * sizeof(SiftManifestScopeRef));
	s->partitions = entries;
	s->partlists = lists;
	s->npartitions = mr.n;
	s->scopes = kept;
	s->nscopes = nscopes;
	s->manifest_version = meta->manifest_version;
	s->scope_dir_gen = meta->scope_dir_gen;
	s->total_rows = meta->total_rows;
	s->total_dead = meta->total_dead;
	s->total_bytes = meta->total_bytes;
	MemoryContextSwitchTo(prev);
	s->size = MemoryContextMemAllocated(s->cx, true);
	if (shm != NULL)
		sift_shm_snap_unpin(shm);
}

static const SiftPlanSnapshot *
build(Relation rel, const SiftShmIndexMeta *meta, MemoryContext mcxt, bool keep)
{
	SiftPlanSnapshot fresh = {.locator = rel->rd_locator};
	SiftPlanSnapshot *s;

	fresh.cx = AllocSetContextCreateInternal(mcxt, "sift cache plan snapshot", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, SIFT_MEM_MAX_BLOCK);
	load(&fresh, rel, meta);
	if (!keep || !sift_cache_room(fresh.size))
	{
		s = MemoryContextAlloc(fresh.cx, sizeof(fresh));
		*s = fresh;
		PLAN_STAT(private_builds, 1);
		return s;
	}
	MemoryContextSetParent(fresh.cx, cache_cx);
	s = hash_search(cache, &fresh.locator, HASH_ENTER, NULL);
	*s = fresh;
	dlist_push_tail(&lru, &s->node);
	PLAN_STAT(builds, 1);
	return pin(s, mcxt);
}

/* The version of the manifest as the registry in shared memory has it, without a read of Block 0; from Block 0 when it does not know the index yet, which then tells it */
static SiftShmIndexMeta
manifest_meta(Relation rel)
{
	SiftShmIndexMeta meta;

	if (sift_shm_manifest_peek(&rel->rd_locator, &meta))
		return meta;
	{
		SiftColMetaPageData m = sift_col_meta_read(rel);

		meta = sift_col_meta_shm(rel, &m);
	}
	if (sift_shm_manifest_publish(&meta, NULL))
		sift_shm_manifest_peek(&rel->rd_locator, &meta);
	return meta;
}

const SiftPlanSnapshot *
sift_plan_cache_get(Relation rel, MemoryContext mcxt)
{
	SiftShmIndexMeta meta = manifest_meta(rel);
	SiftPlanSnapshot *s;

	if (cache == NULL)
		cache_init();
	s = hash_search(cache, &rel->rd_locator, HASH_FIND, NULL);
	if (s == NULL)
		return build(rel, &meta, mcxt, true);
	if (s->manifest_version == meta.manifest_version && s->scope_dir_gen == meta.scope_dir_gen)
	{
		PLAN_STAT(hits, 1);
		return pin(s, mcxt);
	}
	if (s->pins > 0)
		return build(rel, &meta, mcxt, false);
	load(s, rel, &meta);
	PLAN_STAT(refreshes, 1);
	return pin(s, mcxt);
}

bool
sift_plan_card_built(const SiftPlanSnapshot *snap)
{
	return snap->card_built;
}

const struct SiftCardStatsView *
sift_plan_card_get(const SiftPlanSnapshot *snap)
{
	return snap->card_view;
}

void
sift_plan_card_put(const SiftPlanSnapshot *snap, struct SiftCardStatsView *view)
{
	SiftPlanSnapshot *s = (SiftPlanSnapshot *) snap;

	s->card_view = view;
	s->card_built = true;
	s->size = MemoryContextMemAllocated(s->cx, true);
}

const SiftPartitionEntry *
sift_plan_entry(const SiftPlanSnapshot *snap, uint64 part_id)
{
	for (int i = 0; i < snap->npartitions; i++)
		if (snap->partitions[i].part_id == part_id)
			return &snap->partitions[i];
	ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
}
