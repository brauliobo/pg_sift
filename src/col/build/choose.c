/*
 * col/build/choose.c - where the rows of a CREATE INDEX or REINDEX of a sift_col index come from
 * There are three producers of rows for one writer of sorted runs: the heap scan (with the key ranges' files when the index is key_cut), the heap read in the order of a
 * btree on the key (btree.c: no files, and the heap blocks again when the heap is not in the order of the key), and the rows of the index a REINDEX replaces (from_index.c:
 * no heap read, no files). Each is priced from what it reads and writes and what a row costs in it, and the build takes the cheapest; the NOTICE says which and what the others
 * would have taken. sift.col_build_source = heap takes the heap, as before 5.1.
 * A scoped index builds the rows of its entities through the path chosen, but the second pass of its scopes sorts and files its own rows (col/build/scope.c); one with a scope
 * has no old index that can give them (from_index.c), and its btree path is serial.
 */
#include "postgres.h"

#include <float.h>
#include "access/tableam.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "col/build/build_int.h"
#include "col/scope.h"
#include "core/membudget.h"

#define SIFT_BUILD_SOURCE_AUTO	0
#define SIFT_BUILD_SOURCE_HEAP	1

int			sift_col_build_source = SIFT_BUILD_SOURCE_AUTO;

#ifdef SIFT_TESTING
int			sift_test_col_build_source_used = 0;
int			sift_test_col_choose_mutant = 0;
#endif

static const struct config_enum_entry build_source_options[] = {
	{"auto", SIFT_BUILD_SOURCE_AUTO, false},
	{"heap", SIFT_BUILD_SOURCE_HEAP, false},
	{NULL, 0, false}
};

void
sift_col_build_define_gucs(void)
{
	DefineCustomEnumVariable("sift.col_build_source",
							 "Where CREATE INDEX and REINDEX of a sift_col index take their rows from: auto (the build chooses the cheapest of the heap, the heap read through a btree on the key and the index a REINDEX replaces) or heap (always the heap).",
							 "auto prices the paths from the blocks they read, the temporary files they write and the CPU of a row, at the cost of a read measured on the disk of the table, and says its choice in a NOTICE. "
							 "heap scans the heap, with the files of the key ranges for a key_cut index, as releases before 5.1 did.",
							 &sift_col_build_source, SIFT_BUILD_SOURCE_AUTO, build_source_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);
}

/* What each path that could serve costs, for the choice and the NOTICE */
typedef struct Paths
{
	BlockNumber heap_blocks;
	uint64		rows;
	double		heap_us,		/* the heap scan, with the key ranges' files when the index is key_cut */
				heap_mb;
	bool		heap_priced;	/* the time and the files of the heap scan are known (a btree was walked); else only its blocks */
	bool		ranges;			/* the heap scan writes the files of the key ranges */
	BtPlan	   *btree;
	const char *btrees;			/* the candidates and what each reads */
	ColOld	   *old;
	double		old_us,
				old_seq_us;
	bool		no_room;
	double		room_mb;
} Paths;

static bool
old_serves(const ColOld *old)
{
	return old != NULL && colb_old_refusal(old) == NULL;
}

static void
say_heap(StringInfo out, const Paths *p)
{
	appendStringInfo(out, "%s: %u heap blocks", p->ranges ? "key ranges" : "heap scan", p->heap_blocks);
	if (p->ranges && p->heap_priced)
		appendStringInfo(out, ", %.0f MB of temporary files", p->heap_mb);
	else if (p->ranges)
		appendStringInfoString(out, ", temporary files not priced");
	else
		appendStringInfoString(out, ", no temporary files");
	if (p->heap_priced)
		appendStringInfo(out, ", %.2f s", p->heap_us / 1e6);
	appendStringInfoString(out, ". ");
}

static void
say_btree(StringInfo out, const Paths *p, int participants)
{
	const BtPlan *b = p->btree;

	appendStringInfo(out, "btree \"%s\": %.0f heap blocks, %.0f at random, at %.1f us (%s), no temporary files, %.2f s with %d process%s",
					 b->name, b->reads, b->random_reads, b->cost.random_us, b->cost.measured ? "measured" : "set", b->btree_us / 1e6, participants, participants == 1 ? "" : "es");
	if (p->heap_priced && b->break_even_us < 1e6)
		appendStringInfo(out, ", as long as the key ranges at %.1f us", b->break_even_us);
	appendStringInfoString(out, ". ");
}

static void
say_old(StringInfo out, const Paths *p)
{
	if (old_serves(p->old))
		appendStringInfo(out, "index it replaces: %llu entries in %u blocks, no heap block, no temporary files, %.2f s. ",
						 (unsigned long long) colb_old_rows(p->old), colb_old_blocks(p->old), p->old_us / 1e6);
	else if (p->old != NULL)
		appendStringInfo(out, "the index it replaces cannot give the rows: %s. ", colb_old_refusal(p->old));
}

static char *
paths_detail(const Paths *p, int participants)
{
	StringInfoData out;

	initStringInfo(&out);
	if (p->old != NULL)
		say_old(&out, p);
	if (p->btree != NULL)
		say_btree(&out, p, participants);
	say_heap(&out, p);
	if (p->btrees != NULL && p->btrees[0] != '\0')
		appendStringInfo(&out, "Btrees: %s.", p->btrees);
	return out.data;
}

static void
say_choice(Relation index, const BuildPlan *plan, const Paths *p)
{
	char	   *detail = paths_detail(p, plan->participants);
	const char *name = sift_col_final_name(index);

	if (plan->source == COLB_OLD)
		ereport(NOTICE, (errmsg("sift_col index \"%s\": rows taken from the index it replaces, no heap read and no temporary files", name),
						 errdetail("%s", detail),
						 errhint("SET sift.col_build_source = heap reads the heap, as releases before 5.1 did.")));
	else if (plan->source == COLB_BTREE && p->no_room)
		ereport(NOTICE, (errmsg("sift_col index \"%s\": no room for the key ranges' files, heap read through the btree \"%s\"", name, p->btree->name),
						 errdetail("The key ranges need about %.0f MB of temporary files, %.0f MB are allowed. %s", p->heap_mb, p->room_mb, detail),
						 errhint("SET sift.col_build_source = heap reads the heap, as releases before 5.1 did.")));
	else if (plan->source == COLB_BTREE)
		ereport(NOTICE, (errmsg("sift_col index \"%s\": heap read through the btree \"%s\" in key order, no temporary files", name, p->btree->name),
						 errdetail("%s", detail),
						 errhint("SET sift.col_build_source = heap reads the heap, as releases before 5.1 did.")));
	else
		ereport(NOTICE, (errmsg("sift_col index \"%s\": the heap is read, which costs less than the other sources of rows", name),
						 errdetail("%s", detail),
						 errhint("SET sift.col_build_source = heap reads the heap, as releases before 5.1 did.")));
}

#define MUTANT_DEARER		1	/* the dearest path is taken */
#define MUTANT_ROOM_IGNORED	2	/* the key ranges are taken whatever room the temporary files have */

static bool
mutant(int which)
{
#ifdef SIFT_TESTING
	return sift_test_col_choose_mutant == which;
#else
	return false;
#endif
}

static bool
btree_forced(void)
{
#ifdef SIFT_TESTING
	return sift_test_col_build_btree > 0;
#else
	return false;
#endif
}

static bool
btree_banned(void)
{
#ifdef SIFT_TESTING
	return sift_test_col_build_btree < 0;
#else
	return false;
#endif
}

/* The btree that reads the heap for the least, priced against the key ranges (NULL: there is none that serves, or the table is less than a run) */
static BtPlan *
price_btree(Relation heap, Relation index, IndexInfo *info, uint64 tuples, int participants, MemoryContext plan_cx, Paths *p, BtPricing *price)
{
	StringInfoData names;
	BtPlan	   *best;
	double		allowed;

	if (!sift_col_key_cut(sift_col_rel(index)) || btree_banned() ||
		colb_spill_leaf_of(sift_col_rel(index), RelationGetNumberOfBlocks(heap), tuples, sift_colb_budget(1)) < 0)
		return NULL;
	initStringInfo(&names);
	best = colb_btree_best(heap, index, info, colb_btree_candidates(index, heap, info), tuples, participants, plan_cx, &names, price);
	if (best == NULL)
		return NULL;
	p->btrees = names.data;
	p->ranges = true;
	p->heap_priced = price->files_priced;
	if (price->files_priced)
	{
		p->heap_us = best->spool_us;
		p->heap_mb = best->spool_mb;
		p->no_room = !colb_spill_fits(best->spool_mb * 1048576.0, &allowed);
		p->room_mb = allowed / 1048576.0;
	}
	return best;
}

/* The old index, priced: its blocks are read in order and its entries merged */
static void
price_old(Paths *p)
{
	p->old_seq_us = colb_seq_us(colb_old_view(p->old));
	p->old_us = colb_old_blocks(p->old) * p->old_seq_us + colb_old_rows(p->old) * COLB_OLD_ROW_US;
}

/* The cheapest of the paths that serve: the forced btree of a test, else by time (the old index before the btree before the heap at a tie) */
static void
choose_by_price(Relation index, Paths *p, int btree_participants, BuildPlan *plan)
{
	if (!p->heap_priced && old_serves(p->old))
	{
		p->heap_us = p->heap_blocks * p->old_seq_us + p->rows * COLB_HEAP_ROW_US;
		p->ranges = sift_col_key_cut(sift_col_rel(index));
	}
	if (p->btree != NULL && btree_forced())
		plan->source = COLB_BTREE;
	else
	{
		double		heap_us = p->no_room && !mutant(MUTANT_ROOM_IGNORED) ? DBL_MAX : p->heap_us,
					btree_us = p->btree != NULL ? p->btree->btree_us : DBL_MAX,
					old_us = old_serves(p->old) ? p->old_us : DBL_MAX;

		if (mutant(MUTANT_DEARER))
		{
			heap_us = heap_us == DBL_MAX ? DBL_MAX : -heap_us;
			btree_us = btree_us == DBL_MAX ? DBL_MAX : -btree_us;
			old_us = old_us == DBL_MAX ? DBL_MAX : -old_us;
		}
		plan->source = old_us <= btree_us && old_us <= heap_us ? COLB_OLD : btree_us <= heap_us ? COLB_BTREE : COLB_HEAP;
	}
	plan->btree = plan->source == COLB_BTREE ? p->btree : NULL;
	plan->participants = plan->source == COLB_BTREE ? btree_participants : 1;
}

/*
 * The path the rows of the build come from. The plan's memory (the old index's segments, the texts of the NOTICE) is one context, plan.cx, that colb_choose_end deletes after the
 * build. A read of a btree costs a participant's share of the time of one when a build through it is parallel; the old index is read by one. heap_only: the heap holds rows the old
 * index has not (this transaction's own buffered inserts).
 */
BuildPlan
colb_choose(Relation heap, Relation index, IndexInfo *info, bool heap_only)
{
	MemoryContext caller = CurrentMemoryContext;
	BuildPlan	plan = {.source = COLB_HEAP, .participants = 1,.cx = AllocSetContextCreate(caller, "sift_col build choose", ALLOCSET_SMALL_SIZES)};
	bool		par = info->ii_ParallelWorkers > 0 && sift_scope_specs(index) == NULL;
	int			btree_participants = par ? 1 + Min(info->ii_ParallelWorkers, SIFT_COL_BTREE_WORKERS) : 1;
	Paths		p = {0};
	BtPricing	price = {0};
	BlockNumber pages;
	double		tuples,
				allvisfrac;

	SIFT_MEM_SCOPE(col_build_choose, plan.cx);
#ifdef SIFT_TESTING
	sift_test_col_from_index_used = sift_test_col_build_source_used = 0;
	sift_test_col_btree_entries = sift_test_col_btree_visible = sift_test_col_btree_blocks = 0;
	sift_test_col_pick_entries = sift_test_col_pick_blocks = sift_test_col_pick_reads = sift_test_col_pick_random = sift_test_col_pick_spool_bytes = 0;
#endif
	if (sift_col_build_source == SIFT_BUILD_SOURCE_HEAP || sift_col_journal_empty)
		return plan;
	table_relation_estimate_size(heap, NULL, &pages, &tuples, &allvisfrac);
	p.heap_blocks = RelationGetNumberOfBlocks(heap);
	p.rows = (uint64) tuples;
	MemoryContextSwitchTo(plan.cx);
	if (!heap_only)
		p.old = colb_old_open(heap, index, info);
	plan.old = p.old;
	price.old_us = price.heap_floor_us = DBL_MAX;
	if (old_serves(p.old))
	{
		price_old(&p);
		price.old_us = p.old_us;
		price.heap_floor_us = p.heap_blocks * p.old_seq_us + p.rows * COLB_HEAP_ROW_US;
	}
	else if (p.old != NULL)
		elog(DEBUG1, "sift_col index \"%s\" is built from the heap: %s", RelationGetRelationName(index), colb_old_refusal(p.old));
	p.btree = price_btree(heap, index, info, p.rows, btree_participants, caller, &p, &price);
	if (p.btree != NULL || old_serves(p.old))
	{
		choose_by_price(index, &p, btree_participants, &plan);
		say_choice(index, &plan, &p);
	}
	MemoryContextSwitchTo(caller);
#ifdef SIFT_TESTING
	sift_test_col_build_source_used = plan.source;
#endif
	SIFT_MEM_CHECKPOINT();
	return plan;
}

void
colb_choose_end(BuildPlan *plan)
{
	if (plan->old != NULL)
		colb_old_close(plan->old);
	MemoryContextDelete(plan->cx);
}
