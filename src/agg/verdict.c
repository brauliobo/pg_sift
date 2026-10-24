/*
 * agg/verdict.c - the choice between a shared CTE and its inline plan, kept per backend (cte.c plan_cte).
 *
 * Making the choice takes PostgreSQL's planner four to eight runs of the statement (the shared plan, the inline plan, the larger sizes of
 * sift.agg_scale_horizon, the flattened form), where a backend that serves one application sees the same few statements again and again with
 * other constants. The verdict is kept under the statement without its constants (the query jumble), the size class of each table it reads, and
 * the settings that move a plan; a hit plans the chosen variant only. Every eighth hit of an entry plans in full again and compares, so a verdict
 * that went stale (the constants of a statement select other rows) is replaced, and sift.stats() counts the ones that changed.
 *
 * A stale verdict is a plan of the other variant, never other rows: both variants are plans of the same query. An entry is dropped when a table it
 * names is invalidated (DDL, an index created or dropped, ANALYZE, VACUUM), and the least recently used one goes when the cache holds VERDICT_MAX.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_class.h"
#include "common/hashfn.h"
#include "lib/ilist.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "utils/guc_tables.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/plan_int.h"
#include "agg/stats.h"

/* about 200 bytes an entry: under 1 MB */
#define VERDICT_MAX		4096

/* hits of an entry between two plans in full */
#define VERDICT_RECHECK	8

typedef struct VerdictEntry
{
	uint64		key;
	dlist_node	lru;
	bool		won;
	uint32		age;
	double		scans,
				shared;
	int			nrels;
	Oid			rels[VERDICT_RELS];
} VerdictEntry;

static HTAB *cache;
static dlist_head recent = DLIST_STATIC_INIT(recent);

/* the relations of all the entries, one bit each: an invalidation of another relation costs a test, not a walk of the cache */
static uint64 rel_bloom;

#define REL_BIT(relid) (UINT64CONST(1) << ((relid) % 64))

static void
forget_entry(VerdictEntry *e)
{
	dlist_delete(&e->lru);
	hash_search(cache, &e->key, HASH_REMOVE, NULL);
}

static bool
names_rel(const VerdictEntry *e, Oid relid)
{
	for (int i = 0; i < e->nrels; i++)
		if (e->rels[i] == relid)
			return true;
	return false;
}

/* relid: a table whose relcache entry was invalidated (InvalidOid: all of them) */
static void
verdict_inval(Datum arg, Oid relid)
{
	dlist_mutable_iter it;

	if (cache == NULL || (relid != InvalidOid && !(rel_bloom & REL_BIT(relid))))
		return;
	dlist_foreach_modify(it, &recent)
	{
		VerdictEntry *e = dlist_container(VerdictEntry, lru, it.cur);

		if (relid == InvalidOid || names_rel(e, relid))
			forget_entry(e);
	}
	if (dlist_is_empty(&recent))
		rel_bloom = 0;
}

/* the settings a plan depends on: the planner's (methods, costs), the memory a plan may use and the extension's */
static uint64
settings_hash(void)
{
	int			n;
	struct config_generic **vars = get_guc_variables(&n);
	uint64		h = 0;

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
			rel = relation_open(rte->relid, AccessShareLock);
			s->h = hash_combine64(s->h, size_class(RelationGetNumberOfBlocks(rel)));
			relation_close(rel, AccessShareLock);
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
	CacheRegisterRelcacheCallback(verdict_inval, (Datum) 0);
}

/* q is jumbled in place (its queryId is put back) and is not otherwise touched */
void
verdict_lookup(Query *q, CteVerdict *vd)
{
	uint64		queryid = q->queryId;
	int			compute;
	TableSizes	sizes = {0};
	VerdictEntry *e;

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
	vd->key = hash_combine64(hash_combine64(q->queryId, settings_hash()), sizes.h);
	q->queryId = queryid;
	vd->nrels = sizes.n;
	vd->cacheable = true;
	if (cache == NULL)
		cache_create();
	e = hash_search(cache, &vd->key, HASH_FIND, NULL);
	if (e != NULL && e->age < VERDICT_RECHECK - 1)
	{
		e->age++;
		dlist_move_head(&recent, &e->lru);
		vd->known = true;
		vd->won = e->won;
		vd->scans = e->scans;
		vd->shared = e->shared;
		sift_stat_add(SIFT_STAT_VERDICT_HITS, 1);
	}
	else
		sift_stat_add(SIFT_STAT_VERDICT_MISSES, 1);
}

void
verdict_remember(const CteVerdict *vd, bool won, double scans, double shared)
{
	bool		found;
	VerdictEntry *e;

	if (!vd->cacheable)
		return;
	if (hash_get_num_entries(cache) >= VERDICT_MAX && hash_search(cache, &vd->key, HASH_FIND, NULL) == NULL)
		forget_entry(dlist_container(VerdictEntry, lru, dlist_tail_node(&recent)));
	e = hash_search(cache, &vd->key, HASH_ENTER, &found);
	if (found)
	{
		if (e->won != won)
			sift_stat_add(SIFT_STAT_VERDICT_FLIPS, 1);
		dlist_move_head(&recent, &e->lru);
	}
	else
		dlist_push_head(&recent, &e->lru);
	e->won = won;
	e->age = 0;
	e->scans = scans;
	e->shared = shared;
	e->nrels = vd->nrels;
	memcpy(e->rels, vd->rels, sizeof(Oid) * vd->nrels);
	for (int i = 0; i < vd->nrels; i++)
		rel_bloom |= REL_BIT(vd->rels[i]);
}
