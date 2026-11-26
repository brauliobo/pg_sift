/*
 * col/f2/cache.c - the per-backend snapshot of the partition directory and the part lists of an index of format major 2 (f2_cache.h).
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "col/f2/meta.h"
#include "col/f2/cache.h"
#include "core/hash.h"
#include "core/memtrack.h"
#include "core/cachebudget.h"
#include "core/shm/manifest.h"

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

/* The snapshot of a relation file whose generations start again: one a statement still pins cannot go, it only stops matching any manifest */
void
sift_plan_cache_forget(const RelFileLocator *locator)
{
	SiftPlanSnapshot *s = cache != NULL ? hash_search(cache, locator, HASH_FIND, NULL) : NULL;

	if (s == NULL)
		return;
	if (s->pins == 0)
		drop(s);
	else
		s->manifest_version = s->scope_dir_gen = UINT64_MAX;
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

#define PART_ID_HT_MIN_PARTITIONS	16

static uint32
part_id_home(uint64 part_id, uint32 mask)
{
	return (uint32) (sift_fmix64(part_id) & mask);
}

/* The table of the places of entries[0 .. n) by part_id: a slot holds the place plus one, 0 is empty */
static uint32 *
part_id_table(const SiftPartitionEntry *entries, int n, uint32 *mask)
{
	uint32		size = pg_nextpower2_32(Max(2 * (uint32) n, 32));
	uint32	   *ht = palloc0(size * sizeof(uint32));

	*mask = size - 1;
	for (int i = 0; i < n; i++)
	{
		uint32		h = part_id_home(entries[i].part_id, *mask);

		while (ht[h] != 0)
			h = (h + 1) & *mask;
		ht[h] = i + 1;
	}
	return ht;
}

static int
part_id_place(const SiftPartitionEntry *entries, const uint32 *ht, uint32 mask, uint64 part_id)
{
	for (uint32 h = part_id_home(part_id, mask); ht[h] != 0; h = (h + 1) & mask)
		if (entries[ht[h] - 1].part_id == part_id)
			return ht[h] - 1;
	return -1;
}

/* The lists of s that are those of the partition entries, found by part_id in a table of the old ones: a partition a split added is told from the others in one probe, however many of them there are */
static void
reuse_lists(const SiftPlanSnapshot *s, const SiftManifestRead *mr, SiftPlanPartList *out, bool *reused)
{
	uint32		mask;
	uint32	   *ht;

	if (s->npartitions == 0)
		return;
	ht = s->part_id_ht != NULL ? s->part_id_ht : part_id_table(s->partitions, s->npartitions, &mask);
	mask = s->part_id_ht != NULL ? s->part_id_mask : mask;
	for (int i = 0; i < mr->n; i++)
	{
		int			k = part_id_place(s->partitions, ht, mask, mr->entries[i].part_id);

		if (k >= 0 && s->partlists[k].manifest_gen == mr->entries[i].manifest_gen)
		{
			out[i] = s->partlists[k];
			reused[k] = true;
		}
	}
	if (ht != s->part_id_ht)
		pfree(ht);
}

/* The part list of one partition in the shared snapshot: the parts first, then the generation that says they are all there */
typedef struct ShmPartList
{
	uint64		manifest_gen;
	uint32		nparts;
	uint32		pad;
	SiftPartDesc parts[FLEXIBLE_ARRAY_MEMBER];
} ShmPartList;

typedef struct ShmListGet
{
	int			i;
	const SiftPartitionEntry *entry;
	SiftPlanPartList *out;
	bool		found;
} ShmListGet;

static void
shm_list_get_in(const SiftShmManifestSnap *shm, void *arg)
{
	ShmListGet *g = arg;
	const ShmPartList *l = sift_shm_snap_part_list(shm, g->i);

	if (l == NULL || g->entry->nparts == 0 || l->manifest_gen != g->entry->manifest_gen)
		return;
	g->out->nparts = l->nparts;
	g->out->parts = palloc(l->nparts * sizeof(SiftPartDesc));
	memcpy(g->out->parts, l->parts, l->nparts * sizeof(SiftPartDesc));
	g->found = true;
}

static bool
shm_list_get(const RelFileLocator *loc, uint64 version, int i, const SiftPartitionEntry *e, SiftPlanPartList *out)
{
	ShmListGet	g = {.i = i,.entry = e,.out = out};

	return sift_shm_snap_with_epoch(loc, version, shm_list_get_in, &g) && g.found;
}

typedef struct ShmListPut
{
	int			i;
	const void *bytes;
	Size		size;
} ShmListPut;

static void
shm_list_put_in(const SiftShmManifestSnap *shm, void *arg)
{
	ShmListPut *p = arg;

	(void) sift_shm_snap_part_list_install(shm, p->i, p->bytes, p->size);
}

static void
shm_list_put(const RelFileLocator *loc, uint64 version, int i, const SiftPlanPartList *list)
{
	Size		bytes = offsetof(ShmPartList, parts) + list->nparts * sizeof(SiftPartDesc);
	ShmPartList *l = palloc0(bytes);
	ShmListPut	p = {.i = i,.bytes = l,.size = bytes};

	l->manifest_gen = list->manifest_gen;
	l->nparts = list->nparts;
	memcpy(l->parts, list->parts, list->nparts * sizeof(SiftPartDesc));
	(void) sift_shm_snap_with_epoch(loc, version, shm_list_put_in, &p);
	pfree(l);
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

typedef struct ShmDirectory
{
	SiftManifestRead mr;
	SiftManifestScopeRef *scopes;
	int			nscopes;
} ShmDirectory;

static void
shm_directory_copy(const SiftShmManifestSnap *shm, void *arg)
{
	ShmDirectory *d = arg;

	d->mr = (SiftManifestRead) {.n = shm->npartitions,.version = shm->manifest_version};
	d->mr.entries = palloc(Max(d->mr.n, 1) * sizeof(SiftPartitionEntry));
	memcpy(d->mr.entries, sift_shm_snap_part_entries(shm), d->mr.n * sizeof(SiftPartitionEntry));
	d->nscopes = shm->nscopes;
	d->scopes = palloc(Max(d->nscopes, 1) * sizeof(SiftManifestScopeRef));
	if (d->nscopes > 0)
		memcpy(d->scopes, sift_shm_snap_scopes(shm), d->nscopes * sizeof(SiftManifestScopeRef));
}

/* The entries of the partitions and the scope refs of the version the registry holds a snapshot of, else those the pages hold (and the registry gets them for the next backend); *shared: the registry's snapshot of that version serves the part lists */
static SiftManifestRead
read_directory(Relation rel, const SiftShmIndexMeta *meta, bool *shared, SiftManifestScopeRef **scopes, int *nscopes)
{
	ShmDirectory d;
	SiftManifestRead mr;

	if (sift_shm_snap_with_epoch(&rel->rd_locator, meta->manifest_version, shm_directory_copy, &d))
	{
		*shared = true;
		*scopes = d.scopes;
		*nscopes = d.nscopes;
		PLAN_STAT(shm_loads, 1);
		return d.mr;
	}
	mr = sift_manifest_read_range(rel, NULL, NULL);
	*scopes = sift_manifest_read_scope_dir(rel, meta->scope_dir_blk, meta->nscopes, meta->scope_dir_gen, nscopes);
	*shared = false;
	if ((meta->state_flags & SIFT_SHM_SLOT_VALID) && !(meta->state_flags & SIFT_SHM_SLOT_HAS_SNAP) && mr.version == meta->manifest_version)
	{
		SiftShmSnapSpec spec = {.manifest_version = meta->manifest_version,.scope_dir_gen = meta->scope_dir_gen,.npartitions = mr.n,.nscopes = *nscopes,.part_entries = mr.entries,
		.part_entry_size = sizeof(SiftPartitionEntry),.scopes = *scopes,.scope_entry_size = sizeof(SiftManifestScopeRef)};

		if (sift_shm_manifest_publish(meta, &spec))
		{
			*shared = true;
			PLAN_STAT(shm_publishes, 1);
		}
	}
	return mr;
}

/* The directory of the version *meta names. A commit writes the pages of its version before the registry names it, so a read of the pages may find a later version than the registry's: meta then becomes the one the pages hold, as often as writers keep moving it (a cancel ends the chase) */
static SiftManifestRead
read_directory_of(Relation rel, SiftShmIndexMeta *meta, bool *shared, SiftManifestScopeRef **scopes, int *nscopes)
{
	SiftManifestRead mr = read_directory(rel, meta, shared, scopes, nscopes);

	while (mr.version != meta->manifest_version)
	{
		SiftColMetaPageData page;

		CHECK_FOR_INTERRUPTS();
		page = sift_col_meta_read(rel);

		*meta = sift_col_meta_shm(rel, &page);
		mr = read_directory(rel, meta, shared, scopes, nscopes);
	}
	return mr;
}

/* Reads the directory and the part lists that moved since s was loaded into s->cx; s is left as it was when a read fails */
static void
load(SiftPlanSnapshot *s, Relation rel, const SiftShmIndexMeta *registry)
{
	SiftShmIndexMeta at = *registry;
	const SiftShmIndexMeta *meta = &at;
	bool		shared;
	SiftManifestScopeRef *scopes;
	int			nscopes;
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift plan snapshot read");
	MemoryContext caller = MemoryContextSwitchTo(scratch);
	SiftManifestRead mr = read_directory_of(rel, &at, &shared, &scopes, &nscopes);
	/* MEM[MEM-IDLE-07]: cachebudget plan snapshot ctx (1 KB init + partitions + part lists + card view; evictable LRU) */
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
		if (shared && shm_list_get(&rel->rd_locator, mr.version, i, &mr.entries[i], &lists[i]))
			continue;
		lists[i].parts = read_parts_exact(s->cx, prev, rel, &mr.entries[i], &lists[i].nparts);
		PLAN_STAT(partlists_read, 1);
		if (shared && lists[i].nparts > 0)
			shm_list_put(&rel->rd_locator, mr.version, i, &lists[i]);
	}
	for (int k = 0; k < s->npartitions; k++)
		if (!reused[k])
			pfree(s->partlists[k].parts);
	pfree(reused);
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
	if (s->part_id_ht != NULL)
		pfree(s->part_id_ht);
	s->part_id_ht = mr.n > PART_ID_HT_MIN_PARTITIONS ? part_id_table(entries, mr.n, &s->part_id_mask) : NULL;
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
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(scratch);
	s->size = MemoryContextMemAllocated(s->cx, true);
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
	{
		SiftShmIndexMeta peeked;

		if (sift_shm_manifest_peek(&rel->rd_locator, &peeked))
			meta = peeked;
	}
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
	/* MEM[MEM-IDLE-07]: the card view joins the plan snapshot (plan->cx), so the cache budget is enforced once it is added */
	(void) sift_cache_commit();
}

const SiftPartitionEntry *
sift_plan_entry(const SiftPlanSnapshot *snap, uint64 part_id)
{
	int			i = -1;

	if (snap->part_id_ht != NULL)
		i = part_id_place(snap->partitions, snap->part_id_ht, snap->part_id_mask, part_id);
	for (int k = 0; snap->part_id_ht == NULL && i < 0 && k < snap->npartitions; k++)
		i = snap->partitions[k].part_id == part_id ? k : i;
	if (i >= 0)
		return &snap->partitions[i];
	ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
}
