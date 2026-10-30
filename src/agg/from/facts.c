/*
 * agg/from/facts.c - what is read of a column index and kept between plans
 * Statistics, zones and pre-aggregates of an index and of the partitions of a table, and the
 * invalidation that empties them.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/from/int.h"

/*
 * The layout, statistics and pre-aggregates of a partition's index are read once for a plan, however many arms and CTEs scan the table: the
 * planner hook opens a memo (sift_agg_memo_begin) that its plans share and deletes it when the last of them is made. They are kept for the
 * backend as well, in the context "sift cache partition facts", and read again only when the index's stamp moved (its directory, the pages of its
 * table and those the visibility map says are all-visible) or the catalog entry of the index or of its table was invalidated. The memo gives a plan an
 * index once, so an entry read again is never one that plan holds; the cache is emptied whole, by the next outermost plan, once it has grown past
 * FACTS_CACHE (with the entries read again, whose old facts stay until then).
 */
typedef struct PartMemo
{
	Oid			indexoid;
	PartCand   *pc;
} PartMemo;

typedef struct FactsWatch
{
	uint32		hash;			/* the syscache hash of the pg_class row of an index or a table the facts depend on */
	uint32		gen;			/* bumped by every change of that row */
} FactsWatch;

typedef struct PartFacts
{
	Oid			indexoid;
	SiftSrcStamp stamp;
	FactsWatch *index,
			   *table;
	uint32		index_gen,
				table_gen,
				all_gen;
	PartCand	pc;				/* pc.lay NULL: not read (yet) */
} PartFacts;

/* what is read of the column index of a table scanned in one piece: its statistics and pre-aggregates, as of a stamp, with the zones copied into facts_cx */
typedef struct SrcFacts
{
	Oid			indexoid;
	SiftSrcStamp stamp;
	FactsWatch *index,
			   *table;
	uint32		index_gen,
				table_gen,
				all_gen;
	bool		read;
	SiftSrcStats stats;			/* as the index gave them: sift.plan_scale is applied to the copy a plan takes */
	SiftSrcPreagg pa;
} SrcFacts;

#define FACTS_CACHE		SIFT_AGG_MAX_PARTS	/* the indexes of the largest scan */

static MemoryContext part_memo_cx;
static HTAB *part_memo;
static MemoryContext facts_cx;
static HTAB *facts;
static HTAB *src_facts;
static HTAB *facts_watch;
static uint32 facts_all;		/* bumped by an invalidation of every row */
static int	facts_replaced;		/* entries read again since the cache was emptied: their old facts stay until it is */

/* a syscache callback, not a relcache one: a backend has ten relcache callback slots for all its libraries, and sixty-four for the syscaches */
static void
facts_inval(Datum arg, int cacheid, uint32 hashvalue)
{
	FactsWatch *w;

	if (hashvalue == 0)
		facts_all++;
	else if (facts_watch != NULL && (w = hash_search(facts_watch, &hashvalue, HASH_FIND, NULL)) != NULL)
		w->gen++;
}

static void
facts_empty(void)
{
	if (facts_cx != NULL)
		MemoryContextReset(facts_cx);
	facts = src_facts = facts_watch = NULL;
	facts_replaced = 0;
}

static FactsWatch *
facts_watch_of(Oid relid)
{
	bool		found;
	uint32		hash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(relid));
	FactsWatch *w = hash_search(facts_watch, &hash, HASH_ENTER, &found);

	if (!found)
		w->gen = 0;
	return w;
}

MemoryContext
sift_agg_memo_begin(void)
{
	if (part_memo_cx != NULL)
		return NULL;
	if (facts != NULL && hash_get_num_entries(facts) + (src_facts ? hash_get_num_entries(src_facts) : 0) + facts_replaced >= FACTS_CACHE)
		facts_empty();
	part_memo_cx = AllocSetContextCreate(CurrentMemoryContext, "sift plan memo", ALLOCSET_DEFAULT_SIZES);
	return part_memo_cx;
}

void
sift_agg_memo_end(MemoryContext cx)
{
	if (cx == NULL)
		return;
	part_memo_cx = NULL;
	part_memo = NULL;
	MemoryContextDelete(cx);
}

/* sift.plan_scale: the source has that many times the rows, units and bytes */
void
stats_scaled(Relation rel, SiftSrcStats *st)
{
	sift_colsrc_stats(rel, st);
	st->nrows *= sift_plan_scale;
	st->nunits *= sift_plan_scale;
	st->bytes *= sift_plan_scale;
}

/* what is read of a partition's index: its layout in the terms of the partition, and the statistics and pre-aggregates, packed, in cx */
static void
read_facts(Relation rel, MemoryContext cx, PartCand *pc)
{
	SiftSrcStats *full = palloc(sizeof(SiftSrcStats));
	SiftSrcPreagg *pa;
	MemoryContext old;

	stats_scaled(rel, full);
	old = MemoryContextSwitchTo(cx);
	pc->indexoid = rel->rd_rel->relkind == RELKIND_INDEX ? RelationGetRelid(rel) : InvalidOid;
	pc->lay = sift_colsrc_layout(rel);
	pc->stats = sift_aggpart_pack_stats(full, pc->lay->nleaves);
	pc->pa = pa = palloc(sizeof(SiftSrcPreagg));
	sift_colsrc_preagg(rel, pa);
	MemoryContextSwitchTo(old);
	pfree(full);
}

static void
facts_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(PartFacts)};

	if (facts != NULL)
		return;
	if (facts_cx == NULL)
	{
		facts_cx = AllocSetContextCreate(CacheMemoryContext, "sift cache partition facts", ALLOCSET_DEFAULT_SIZES);
		CacheRegisterSyscacheCallback(RELOID, facts_inval, (Datum) 0);
	}
	ctl.hcxt = facts_cx;
	facts = hash_create("sift partition facts", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ctl.entrysize = sizeof(SrcFacts);
	src_facts = hash_create("sift index facts", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ctl.entrysize = sizeof(FactsWatch);
	facts_watch = hash_create("sift partition facts watch", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static const SiftSrcZones *
zones_copy(const SiftSrcZones *z, MemoryContext cx)
{
	SiftSrcZones *c;

	if (z == NULL)
		return NULL;
	c = MemoryContextAllocZero(cx, sizeof(SiftSrcZones));
	*c = *z;
	for (int i = 0; i < SIFT_COLSRC_MAX_LEAVES; i++)
		if (z->lo[i] != NULL)
		{
			c->lo[i] = memcpy(MemoryContextAlloc(cx, z->n * sizeof(int64)), z->lo[i], z->n * sizeof(int64));
			c->hi[i] = memcpy(MemoryContextAlloc(cx, z->n * sizeof(int64)), z->hi[i], z->n * sizeof(int64));
		}
	return c;
}

/* The statistics of the first n leaves: the struct is 30 KB for the 128 leaves an index can have, and an index has a few (the rest of dst is zero) */
static void
stats_copy(SiftSrcStats *dst, const SiftSrcStats *src, int n)
{
	size_t		d = n * sizeof(double),
				b = n * sizeof(bool);

	dst->nrows = src->nrows;
	dst->nunits = src->nunits;
	dst->bytes = src->bytes;
	dst->allvisible_frac = src->allvisible_frac;
	dst->exact_frac = src->exact_frac;
	dst->rows_miss = src->rows_miss;
	dst->zones = src->zones;
	memcpy(dst->dim_miss, src->dim_miss, d);
	memcpy(dst->mm_miss, src->mm_miss, d);
	memcpy(dst->roll_miss, src->roll_miss, d);
	memcpy(dst->ndistinct, src->ndistinct, d);
	memcpy(dst->min, src->min, d);
	memcpy(dst->max, src->max, d);
	memcpy(dst->point_cover, src->point_cover, d);
	memcpy(dst->set_cover, src->set_cover, d);
	memcpy(dst->set_keys, src->set_keys, d);
	memcpy(dst->zone_span, src->zone_span, d);
	memcpy(dst->head_cover, src->head_cover, d);
	memcpy(dst->gord_cover, src->gord_cover, d);
	memcpy(dst->gord_group, src->gord_group, n * sizeof(int));
	memcpy(dst->gord_coll, src->gord_coll, n * sizeof(uint32));
	memcpy(dst->elems_per_row, src->elems_per_row, d);
	memcpy(dst->ovf_units, src->ovf_units, d);
	memcpy(dst->ovf_frac, src->ovf_frac, d);
	memcpy(dst->ovf, src->ovf, b);
	memcpy(dst->jnull, src->jnull, b);
	memcpy(dst->nokinds, src->nokinds, b);
}

/*
 * The statistics and pre-aggregates of the column index of a table scanned in one piece (sift.plan_scale applied), as of the stamp of the index: read when
 * the index has changed since a plan of this backend took them, not by every plan: they cost more than the plan of a statement that the normal plan answers.
 */
void
index_facts(Relation rel, int nleaves, SiftSrcStats *st, SiftSrcPreagg *pa)
{
	Oid			indexoid = RelationGetRelid(rel);
	SiftSrcStamp stamp;
	SrcFacts   *e;
	bool		found;

	facts_init();
	sift_colsrc_stamp(rel, &stamp);
	e = hash_search(src_facts, &indexoid, HASH_ENTER, &found);
	if (!found || !e->read || memcmp(&e->stamp, &stamp, sizeof(stamp)) != 0 || e->index->gen != e->index_gen || e->table->gen != e->table_gen || e->all_gen != facts_all)
	{
		SiftSrcStats fresh;

		/* the generations as of before the facts are read: an invalidation while they are is seen at the next plan */
		e->index = facts_watch_of(indexoid);
		e->table = facts_watch_of(rel->rd_index->indrelid);
		e->index_gen = e->index->gen;
		e->table_gen = e->table->gen;
		e->all_gen = facts_all;
		e->read = false;
		sift_colsrc_stats(rel, &fresh);
		fresh.zones = zones_copy(fresh.zones, facts_cx);
		e->stats = fresh;
		sift_colsrc_preagg(rel, &e->pa);
		e->stamp = stamp;
		e->read = true;
	}
	/* the context of a plan is zero until its first source (a second source of the same context, by another index, is the exception) */
	if (st->nrows != 0 || st->nunits != 0 || st->bytes != 0 || st->zones != NULL)
		memset(st, 0, sizeof(*st));
	stats_copy(st, &e->stats, Min(nleaves, SIFT_COLSRC_MAX_LEAVES));
	*pa = e->pa;
	st->nrows *= sift_plan_scale;
	st->nunits *= sift_plan_scale;
	st->bytes *= sift_plan_scale;
}

static PartCand *
cached_facts(Relation rel)
{
	Oid			indexoid = RelationGetRelid(rel);
	SiftSrcStamp stamp;
	PartFacts  *e;
	PartCand	fresh;
	bool		found;

	facts_init();
	sift_colsrc_stamp(rel, &stamp);
	e = hash_search(facts, &indexoid, HASH_ENTER, &found);
	if (found && e->pc.lay != NULL && memcmp(&e->stamp, &stamp, sizeof(stamp)) == 0 && e->index->gen == e->index_gen &&
		e->table->gen == e->table_gen && e->all_gen == facts_all)
		return &e->pc;
	if (found)
		facts_replaced++;
	/* the generations as of before the facts are read: an invalidation while they are is seen at the next plan */
	e->index = facts_watch_of(indexoid);
	e->table = facts_watch_of(rel->rd_index->indrelid);
	e->index_gen = e->index->gen;
	e->table_gen = e->table->gen;
	e->all_gen = facts_all;
	e->pc.lay = NULL;
	read_facts(rel, facts_cx, &fresh);
	e->pc = fresh;
	e->stamp = stamp;
	return &e->pc;
}

/* what is read of a partition's index or, for a sift_table, of the partition; in the memo when there is one. Its layout's expressions are those of the partition and nothing of it is to be changed */
const PartCand *
part_facts(Oid relid, bool table)
{
	PartMemo   *m = NULL;
	PartCand   *pc;
	Relation	rel;

	if (part_memo_cx != NULL)
	{
		bool		found;

		if (part_memo == NULL)
		{
			HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(PartMemo), .hcxt = part_memo_cx};

			part_memo = hash_create("sift plan memo", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		}
		m = hash_search(part_memo, &relid, HASH_ENTER, &found);
		if (found)
			return m->pc;
	}
	rel = table ? table_open(relid, AccessShareLock) : index_open(relid, AccessShareLock);
	/* the facts of a table have no stamp to tell when they are stale: they are read for each plan */
	if (!table && part_memo_cx != NULL && (facts == NULL || hash_get_num_entries(facts) < FACTS_CACHE))
		pc = cached_facts(rel);
	else
	{
		MemoryContext cx = part_memo_cx ? part_memo_cx : CurrentMemoryContext;

		pc = MemoryContextAllocZero(cx, sizeof(PartCand));
		read_facts(rel, cx, pc);
	}
	relation_close(rel, AccessShareLock);
	if (m != NULL)
		m->pc = pc;
	return pc;
}
