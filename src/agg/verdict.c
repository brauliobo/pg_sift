/*
 * agg/verdict.c - the choice between a shared CTE and its inline plan, and the one between the flattened form of a statement and the form as written,
 * kept per backend (cte.c plan_cte, plan_flat).
 *
 * Making the choice takes PostgreSQL's planner four to eight runs of the statement (the shared plan, the inline plan, the larger sizes of
 * sift.agg_scale_horizon, the flattened form), where a backend that serves one application sees the same few statements again and again with
 * other constants. The verdict is kept under the statement without its constants (the query jumble), the size class of each table it reads, and
 * the settings that move a plan; a hit plans the chosen variant only. Every eighth hit of an entry plans in full again and compares, so a verdict
 * that went stale (the constants of a statement select other rows) is replaced, and sift.stats() counts the ones that changed. A plan in full costs 4 to 6
 * times a hit, so each one that confirms the entry doubles the hits to the next (8, 16, 32, 64: VERDICT_STREAK_MAX); one that changes it starts again at 8.
 *
 * A stale verdict is a plan of the other variant, never other rows: both variants are plans of the same query. An entry is dropped when a table it
 * names is invalidated (DDL, an index created or dropped, ANALYZE, VACUUM), and the least recently used one goes when the cache holds VERDICT_MAX.
 *
 * A new backend (a pool resets its sessions) would plan every statement in full once. The verdicts are therefore also kept in a table in shared
 * memory (sift.verdict_cache_kb, the DSM registry: nothing is allocated at postmaster start) that every backend of the database reads when its own
 * cache has no entry. The key of the shared entry carries what makes a verdict stale for the others without a message: the database, the size class,
 * the live tuples and the visible pages of each table, and the files of its column indexes, so that an ANALYZE, a rewrite or a REINDEX moves the
 * key and the old entry goes unread. The same check runs on a hit as on a local one: the plan of the variant that won must cost within a factor of
 * VERDICT_BAND of the cost it had when the verdict was made (cte.c), else the statement is planned in full, and every eighth hit of an entry plans in full.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/relation.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "common/hashfn.h"
#include "lib/ilist.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/dsm_registry.h"
#include "storage/lock.h"
#include "storage/spin.h"
#include "utils/guc_tables.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/agg_int.h"
#include "agg/plan_int.h"
#include "agg/stats.h"

/* about 200 bytes an entry: under 1 MB */
#define VERDICT_MAX		4096

/* hits of an entry between two plans in full; each plan in full that confirms the choice doubles it, up to VERDICT_STREAK_MAX times (8, 16, 32, 64) */
#define VERDICT_RECHECK	8
#define VERDICT_STREAK_MAX	3

/* a verdict is not taken when the plan it names costs more than this many times, or less than this many times, what it cost when the verdict was made */
#define VERDICT_BAND	1.4

typedef struct VerdictEntry
{
	uint64		key;
	dlist_node	lru;
	bool		won;
	uint8		streak;
	uint32		age;
	double		scans,
				shared,
				size,
				isize;
} VerdictEntry;

static HTAB *cache;
static dlist_head recent = DLIST_STATIC_INIT(recent);

int			sift_verdict_cache_kb = 256;

/* the table of all the backends: sets of VSHM_WAYS entries, the set of a key by its remainder; the least recently read of a full set goes */
#define VSHM_WAYS		4
#define VSHM_MAX_SETS	4600

typedef struct VerdictShmEnt
{
	uint64		key;			/* 0: free */
	double		scans,
				shared,
				size,
				isize;
	uint32		age,
				tick;
	bool		won;
	uint8		streak;
} VerdictShmEnt;

typedef struct VerdictShm
{
	slock_t		mutex;
	uint32		tick;
	int			nsets;
	VerdictShmEnt ents[VSHM_MAX_SETS][VSHM_WAYS];
} VerdictShm;

#if PG_VERSION_NUM >= 190000
#define VSHM_INIT_ARGS		void *ptr, void *arg
#define VSHM_GET(init, found)	GetNamedDSMSegment(VSHM_NAME, sizeof(VerdictShm), init, found, NULL)
#else
#define VSHM_INIT_ARGS		void *ptr
#define VSHM_GET(init, found)	GetNamedDSMSegment(VSHM_NAME, sizeof(VerdictShm), init, found)
#endif

/* the name carries the layout: a library of another one, loaded by new backends while old ones run, meets a segment it does not know as a size or a name that does not match */
#define VSHM_NAME		"sift_agg_verdicts_4"

static VerdictShm *vshm;

/* set when the table could not be had (no dynamic shared memory, no room, no more segments): the backend plans every statement in full, as with the setting at 0 */
static bool vshm_failed;

#ifdef SIFT_TESTING
bool		sift_test_verdict_fail = false;
bool		sift_test_verdict_gate = false;

/* Testing (sift.test_verdict_gate): a backend whose lookup found no verdict waits for the advisory lock (20984, 1) to be free, so that a test lets another backend plan and keep the same verdict first (test/t/verdict_shared.sh) */
static void
verdict_test_gate(void)
{
	LOCKTAG		tag;

	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F8, 1, 2);
	(void) LockAcquire(&tag, ShareLock, false, false);
	LockRelease(&tag, ShareLock, false);
}
#endif

static void
vshm_init(VSHM_INIT_ARGS)
{
	VerdictShm *s = ptr;

	memset(s, 0, sizeof(*s));
	SpinLockInit(&s->mutex);
	s->nsets = Max(1, Min(VSHM_MAX_SETS, (int) ((Size) sift_verdict_cache_kb * 1024 / (sizeof(VerdictShmEnt) * VSHM_WAYS))));
}

/*
 * The table of the registry, created by the first backend that asks. A cache must not turn into an error: whatever fails here (no room in /dev/shm, no
 * more segments, a segment of another size) is caught in a subtransaction, which also gives back the locks the registry
 * holds, and the backend goes without the table.
 */
static VerdictShm *
vshm_attach(void)
{
	MemoryContext cx = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	VerdictShm *volatile s = NULL;
	bool		found;

	/* a subtransaction cannot start where there is no transaction or a parallel operation; the next statement of the backend tries again */
	if (!IsTransactionState() || IsInParallelMode())
		return NULL;
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cx);
	PG_TRY();
	{
#ifdef SIFT_TESTING
		if (sift_test_verdict_fail)
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift: test: the table of the shared verdicts fails")));
#endif
		s = VSHM_GET(vshm_init, &found);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cx);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(cx);
		e = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cx);
		CurrentResourceOwner = owner;
		vshm_failed = true;
		s = NULL;
		ereport(LOG,
				(errmsg("sift: the choices of the planner are not shared with other backends: %s", e->message),
				 errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm), or set sift.verdict_cache_kb to 0.")));
		FreeErrorData(e);
	}
	PG_END_TRY();
	return s;
}

/* NULL where the table is off or not had */
static VerdictShm *
vshm_get(void)
{
	if (sift_verdict_cache_kb <= 0 || vshm_failed)
		return NULL;
	if (vshm == NULL)
		vshm = vshm_attach();
	return vshm;
}

/* nsets is set once, by the backend that creates the segment, before the registry lets another one attach (its lock is the barrier); every field of an entry is read and written under the spinlock, which is a full barrier on arm64 as on x86-64 */
/* an entry another backend kept under the key; every eighth read of it is a miss, so that a plan in full checks it */
static bool
vshm_read(uint64 key, CteVerdict *vd)
{
	VerdictShm *s = vshm_get();
	VerdictShmEnt *set;
	bool		found = false;

	if (s == NULL)
		return false;
	set = s->ents[key % s->nsets];
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < VSHM_WAYS; i++)
		if (set[i].key == key)
		{
			if (set[i].age < (VERDICT_RECHECK << set[i].streak) - 1)
			{
				set[i].age++;
				set[i].tick = ++s->tick;
				vd->won = set[i].won;
				vd->scans = set[i].scans;
				vd->shared = set[i].shared;
				vd->size = set[i].size;
				vd->isize = set[i].isize;
				vd->streak = set[i].streak;
				found = true;
			}
			else
			{
				vd->prior_seen = true;
				vd->prior_won = set[i].won;
				vd->streak = set[i].streak;
			}
			break;
		}
	SpinLockRelease(&s->mutex);
	return found;
}

static void
vshm_write(uint64 key, const CteVerdict *vd)
{
	VerdictShm *s = vshm_get();
	VerdictShmEnt *set,
			   *e;

	if (s == NULL)
		return;
	set = s->ents[key % s->nsets];
	SpinLockAcquire(&s->mutex);
	e = &set[0];
	for (int i = 0; i < VSHM_WAYS; i++)
	{
		if (set[i].key == key)
		{
			e = &set[i];
			break;
		}
		if (set[i].key == 0 || (e->key != 0 && s->tick - set[i].tick > s->tick - e->tick))
			e = &set[i];
	}
	e->key = key;
	e->won = vd->won;
	e->streak = vd->streak;
	e->age = 0;
	e->tick = ++s->tick;
	e->scans = vd->scans;
	e->shared = vd->shared;
	e->size = vd->size;
	e->isize = vd->isize;
	SpinLockRelease(&s->mutex);
}

static void
forget_entry(VerdictEntry *e)
{
	dlist_delete(&e->lru);
	hash_search(cache, &e->key, HASH_REMOVE, NULL);
}

/* the settings do not change while a statement is planned: one hash serves the lookups of its plans */
static bool settings_known;
static uint64 settings_memo;

/* the size class and the epoch of a table do not change while a statement is planned, and the lookups of its plans (the flattened form and the one as written, each with its CTEs shared and inlined) meet the same tables */
static struct
{
	Oid			relid;
	uint64		h;
}			rel_memo[64];
static int	n_memo;

void
verdict_new_planning(void)
{
	settings_known = false;
	n_memo = 0;
}

/* the settings a plan depends on: the planner's (methods, costs), the memory a plan may use and the extension's */
static uint64
settings_hash(void)
{
	int			n;
	struct config_generic **vars = get_guc_variables(&n);
	uint64		h = 0;

	if (settings_known)
		return settings_memo;

	for (int i = 0; i < n; i++)
	{
		const struct config_generic *g = vars[i];
		bool		planner;

		switch (g->group)
		{
			case QUERY_TUNING_METHOD:
			case QUERY_TUNING_COST:
			case QUERY_TUNING_GEQO:
			case QUERY_TUNING_OTHER:
				planner = true;
				break;
			default:
				planner = strncmp(g->name, "sift.", 5) == 0 || strcmp(g->name, "work_mem") == 0 || strcmp(g->name, "hash_mem_multiplier") == 0 ||
					strcmp(g->name, "max_parallel_workers_per_gather") == 0;
		}
		if (planner)
		{
			const char *v = GetConfigOption(g->name, false, false);

			h = hash_combine64(h, hash_bytes_extended((const unsigned char *) v, strlen(v), 0));
		}
	}
	settings_known = true;
	settings_memo = h;
	return h;
}

/* the tables of the statement and their size class: half an octave of the pages, which the planner reads from the file, not from pg_class */
typedef struct TableSizes
{
	uint64		h;
	int			n;
	Oid		   *rels;
	bool		overflow;
}			TableSizes;

static uint32
size_class(BlockNumber pages)
{
	uint32		pos;

	if (pages < 2)
		return pages;
	pos = pg_leftmost_one_pos32(pages);
	return 2 * pos + ((pages >> (pos - 1)) & 1);
}

/*
 * What an ANALYZE, a VACUUM or a rewrite of a table, or a REINDEX of its column index, changes in the relation cache of every backend, as the key part of
 * the verdicts the backends share (they are not told, as the relcache callback tells a backend of its own entries): the live tuples in half octaves, the
 * share of the pages that are all-visible in quarters (the heap check of the scans), and the oid and the file of each column index.
 */
static uint64
table_epoch(Relation rel)
{
	uint64		h = hash_combine64(0, size_class((BlockNumber) Min(Max(rel->rd_rel->reltuples, 0), (float) PG_UINT32_MAX)));

	h = hash_combine64(h, rel->rd_rel->relpages > 0 ? (uint32) ((uint64) rel->rd_rel->relallvisible * 4 / rel->rd_rel->relpages) : 0);
	h = hash_combine64(h, rel->rd_locator.relNumber);
	foreach_oid(ix, RelationGetIndexList(rel))
	{
		Relation	irel;

		if (!sift_colsrc_is_index(get_rel_relam(ix)))
			continue;
		irel = index_open(ix, AccessShareLock);
		h = hash_combine64(hash_combine64(h, ix), irel->rd_locator.relNumber);
		index_close(irel, AccessShareLock);
	}
	return h;
}

static bool
sizes_walker(Node *node, TableSizes *s)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, sizes_walker, s, QTW_EXAMINE_RTES_BEFORE);
	if (IsA(node, RangeTblEntry))
	{
		const RangeTblEntry *rte = (const RangeTblEntry *) node;
		Relation	rel;
		int			i;

		if (rte->rtekind != RTE_RELATION ||
			(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW && rte->relkind != RELKIND_PARTITIONED_TABLE))
			return false;
		for (i = 0; i < s->n && s->rels[i] != rte->relid; i++)
			;
		if (i < s->n)
			return false;
		if (s->n == VERDICT_RELS)
		{
			s->overflow = true;
			return true;
		}
		s->rels[s->n++] = rte->relid;
		/* a partitioned table has no pages of its own: the entry is dropped with it, and checked again every few hits as its partitions grow */
		if (rte->relkind != RELKIND_PARTITIONED_TABLE)
		{
			uint64		h = 0;
			int			k;

			for (k = 0; k < n_memo && rel_memo[k].relid != rte->relid; k++)
				;
			if (k < n_memo)
				h = rel_memo[k].h;
			else
			{
				rel = relation_open(rte->relid, AccessShareLock);
				h = hash_combine64(size_class(RelationGetNumberOfBlocks(rel)), table_epoch(rel));
				relation_close(rel, AccessShareLock);
				if (n_memo < lengthof(rel_memo))
				{
					rel_memo[n_memo].relid = rte->relid;
					rel_memo[n_memo++].h = h;
				}
			}
			s->h = hash_combine64(s->h, h);
		}
		return false;
	}
	return expression_tree_walker(node, sizes_walker, s);
}

static void
cache_create(void)
{
	HASHCTL		ctl = {.keysize = sizeof(uint64), .entrysize = sizeof(VerdictEntry), .hcxt = TopMemoryContext};

	cache = hash_create("sift CTE verdicts", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/* the entry of the cache of this backend under key, with the verdict of vd; flips: a verdict that replaces another of the other choice is counted */
static void
local_enter(uint64 key, const CteVerdict *vd, bool count_flips)
{
	bool		found;
	VerdictEntry *e;

	if (hash_get_num_entries(cache) >= VERDICT_MAX && hash_search(cache, &key, HASH_FIND, NULL) == NULL)
		forget_entry(dlist_container(VerdictEntry, lru, dlist_tail_node(&recent)));
	e = hash_search(cache, &key, HASH_ENTER, &found);
	if (found)
	{
		if (count_flips && e->won != vd->won)
			sift_stat_add(SIFT_STAT_VERDICT_FLIPS, 1);
		dlist_move_head(&recent, &e->lru);
	}
	else
		dlist_push_head(&recent, &e->lru);
	e->won = vd->won;
	e->streak = vd->streak;
	e->age = 0;
	e->scans = vd->scans;
	e->shared = vd->shared;
	e->size = vd->size;
	e->isize = vd->isize;
}

/* the verdict kept under key, in this backend or in the table of all of them (then it is kept here too): into vd; an entry is read VERDICT_RECHECK - 1 times, the next read is a miss */
static bool
entry_read(uint64 key, CteVerdict *vd)
{
	VerdictEntry *e = hash_search(cache, &key, HASH_FIND, NULL);

	if (e != NULL)
	{
		if (e->age >= (VERDICT_RECHECK << e->streak) - 1)
		{
			vd->prior_seen = true;
			vd->prior_won = e->won;
			vd->streak = e->streak;
			return false;
		}
		e->age++;
		dlist_move_head(&recent, &e->lru);
		vd->won = e->won;
		vd->scans = e->scans;
		vd->shared = e->shared;
		vd->size = e->size;
		vd->isize = e->isize;
		vd->streak = e->streak;
		vd->from_shared = false;
		return true;
	}
	if (!vshm_read(key, vd))
		return false;
	local_enter(key, vd, false);
	vd->from_shared = true;
	return true;
}

/* q is jumbled in place (its queryId is put back) and is not otherwise touched; kind: what the verdict is of (the CTE choice, the flattened form), so that the two of one statement do not meet */
void
verdict_lookup(Query *q, int kind, CteVerdict *vd)
{
	uint64		queryid = q->queryId;
	int			compute;
	TableSizes	sizes = {0};

	memset(vd, 0, sizeof(*vd));
	sizes.rels = vd->rels;
	sizes_walker((Node *) q, &sizes);
	if (sizes.overflow)
		return;
	/* PostgreSQL 19 asserts that the query id is enabled; compute_query_id = auto without a module that wants it is not */
	compute = compute_query_id;
	compute_query_id = COMPUTE_QUERY_ID_ON;
	JumbleQuery(q);
	compute_query_id = compute;
	vd->key = hash_combine64(hash_combine64(hash_combine64(hash_combine64(q->queryId, settings_hash()), sizes.h), kind), MyDatabaseId) | 1;
	q->queryId = queryid;
	vd->cacheable = true;
	if (cache == NULL)
		cache_create();
	vd->known = entry_read(vd->key, vd);
	if (!vd->known)
	{
		sift_stat_add(SIFT_STAT_VERDICT_MISSES, 1);
#ifdef SIFT_TESTING
		if (sift_test_verdict_gate)
			verdict_test_gate();
#endif
	}
}

/* the size in bands of VERDICT_BAND: the constants of a request move the rows of its CTEs over decades, and a verdict is made for the requests of one band; inl: the size of the inline plan, not of the plan with the shared CTEs */
static uint64
band_key(uint64 key, double size, bool inl)
{
	/* shifted, as the key is odd: the combine of two bands next to each other can differ in the lowest bit only */
	return (hash_combine64(key, (uint64) (log(Max(size, 1.0)) / log(VERDICT_BAND)) * 2 + inl) << 1) | 1;
}

/*
 * The plan the hit goes on to plan is of size rows: the largest scan of an indexed table of the plan with the shared CTEs, or the most rows the scans of the
 * inline plan read. The choice between the two plans turns on the size of the request (its scans read a share of the table, the CTEs they replace hold its
 * rows), so a verdict holds within VERDICT_BAND of the size of that plan it was made for: VERDICT_TAKE. Else the verdict made for requests of that size is looked up (remember keeps one
 * for each band): it names the variant to take (VERDICT_SWITCH where it is the other one, vd is then its verdict), or there is none and the statement
 * is planned in full (VERDICT_FULL).
 */
int
verdict_accept(CteVerdict *vd, double size)
{
	double		kept = vd->won ? vd->isize : vd->size;
	int			r = VERDICT_TAKE;

	if (kept > 0 && !(size <= kept * VERDICT_BAND && size * VERDICT_BAND >= kept))
	{
		bool		won = vd->won;

		r = VERDICT_FULL;
		if (entry_read(band_key(vd->key, size, won), vd))
			r = vd->won == won ? VERDICT_TAKE : VERDICT_SWITCH;
	}
	sift_stat_add(r == VERDICT_FULL ? SIFT_STAT_VERDICT_MISSES : SIFT_STAT_VERDICT_HITS, 1);
	if (r != VERDICT_FULL && vd->from_shared)
		sift_stat_add(SIFT_STAT_VERDICT_SHARED, 1);
	return r;
}

/* the verdict of a plan in full is kept under the statement, and under the band of the size of each of its plans, in this backend and in the table of all of them */
void
verdict_remember(CteVerdict *vd, bool won, double scans, double shared, double size, double isize)
{
	uint64		keys[3] = {vd->key};
	int			n = 1;

	if (!vd->cacheable)
		return;
	vd->streak = vd->prior_seen && vd->prior_won == won ? Min(vd->streak + 1, VERDICT_STREAK_MAX) : 0;
	vd->won = won;
	vd->scans = scans;
	vd->shared = shared;
	vd->size = size;
	vd->isize = isize;
	if (size > 0)
		keys[n++] = band_key(vd->key, size, false);
	if (isize > 0)
		keys[n++] = band_key(vd->key, isize, true);
	for (int i = 0; i < n; i++)
	{
		local_enter(keys[i], vd, i == 0);
		vshm_write(keys[i], vd);
	}
}
