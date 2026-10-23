/*
 * agg/spill.c - the group table past hash memory: partial states in partitions on disk
 *
 * The group table of a scan is in memory. When, at the end of an item, it holds more than hash memory (work_mem times
 * hash_mem_multiplier), its groups go to a set of partitions as blobs (agg/blob.c, the partial tables of the workers): a group goes to the
 * partition of the hash of its key values, so every table that ever held the group wrote it to the same one. The table is reset and the
 * scan goes on. When the scan ends, the last table goes to the partitions too, and each partition is merged into a table of its own, by
 * value, and returned as the groups of the result before the next is read. A partition that does not fit hash memory when merged goes to a
 * set of partitions one level deeper (another hash of the same values), as a hash aggregate's spill does; each level is one more write and
 * read of the partial states, never a second scan of the table.
 *
 * Only the specs agg/spec.c names (sift_aggspec_spillable) take this path: a table is reset between two items, which the lanes of a parallel
 * scan do not allow, and the states must be blobs. The float sums of a group are added in the order of its partial states: the lane order
 * of the scan inside a table, the order of the spills between tables.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/tuplestore.h"
#include "utils/memutils.h"

#include "agg/agg.h"
#include "agg/back.h"
#include "agg/spec.h"
#include "core/limits.h"
#include "agg/agg_int.h"

#define SPILL_MIN_PARTS		8
#define SPILL_MAX_PARTS		128		/* one BufFile (8 kB) each */
#define SPILL_MAX_DEPTH		8
#define SPILL_MIN_GROUPS	128		/* a table with fewer groups is not spilled: its memory is something else (the key ranges) */

typedef struct SpillSet
{
	int			depth;
	BufFile    *f[SPILL_MAX_PARTS];
	uint32		nblobs[SPILL_MAX_PARTS];
} SpillSet;

typedef struct SpillWork
{
	BufFile    *f;
	uint32		nblobs;
	int			depth;
} SpillWork;

typedef struct AggSpill
{
	Size		budget;			/* hash memory */
	Size		floor;			/* what an empty table holds */
	uint32		keep;			/* groups of the table written last: the next table of the scan starts with room for them */
	int			nparts;
	bool		spilled;
	int64		groups;			/* of the partitions returned so far */
	Tuplestorestate *out;		/* the groups returned, in order: the table holds one partition, a rescan replays them */
	TupleTableSlot *mslot;
	int64		nread;
	int64		nwritten;
	bool		done;
	MemoryContext tmp;			/* the temporaries of one blob, dumped or merged (agg/blob.c keeps them for the life of a process) */
	SpillSet   *set;			/* the level 0 partitions the scan writes */
	List	   *work;			/* partitions to merge, as SpillWork */
	double	   *amax;			/* avg(float8): the largest absolute input of the tables written */
} AggSpill;

void
spill_begin(AggExec *ex)
{
	AggSpill   *sp;

	if (!sift_aggspec_spillable(ex->spec) || ex->par != NULL || part_flush_on(ex) || ex->be != &sift_agg_cpu_backend)
		return;
	sp = MemoryContextAllocZero(ex->cxt, sizeof(AggSpill));
	sp->budget = get_hash_memory_limit();
	sp->amax = MemoryContextAllocZero(ex->cxt, Max(ex->spec->naggs, 1) * sizeof(double));
	sp->tmp = AllocSetContextCreate(ex->cxt, "sift agg spill", ALLOCSET_DEFAULT_SIZES);
	ex->spill = sp;
}

/* what an empty table holds: the end of table_init */
void
spill_floor(AggExec *ex)
{
	if (ex->spill)
		ex->spill->floor = MemoryContextMemAllocated(ex->tcxt, true);
}

/* a table that starts with room for the groups the planner expects must not be one that cannot fit hash memory */
bool
spill_presize_ok(const AggExec *ex)
{
	return ex->spill == NULL || ex->spec->est_groups * 256.0 <= (double) ex->spill->budget;
}

/* the room a table of the scan starts with in hash mode: growing it again group by group costs as much as the groups themselves (0: none kept) */
uint32
spill_keep(const AggExec *ex)
{
	return ex->spill ? ex->spill->keep : 0;
}

/* a spilled scan keeps hash tables: the memory of a direct table follows the range of the keys, not the groups */
bool
spill_started(const AggExec *ex)
{
	return ex->spill != NULL && ex->spill->spilled;
}

static bool
over_budget(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	return n >= SPILL_MIN_GROUPS && MemoryContextMemAllocated(ex->tcxt, true) > sp->floor + sp->budget;
}

/*
 * Would the table be spilled now, had the spill been armed? A streaming count(DISTINCT) scan (agg/dist.c) holds the spill back, since a partial
 * state carries the set of a group and the pairs it dropped are not in it, and starts over as a plain scan when this says yes.
 */
bool
spill_would(AggExec *ex, AggSpill *sp)
{
	AggSpill   *armed = ex->spill;
	bool		over;

	ex->spill = sp;
	over = over_budget(ex);
	ex->spill = armed;
	return over;
}

static SpillSet *
set_new(AggSpill *sp, int depth)
{
	SpillSet   *set = palloc0(sizeof(SpillSet));

	set->depth = depth;
	for (int p = 0; p < sp->nparts; p++)
		set->f[p] = BufFileCreateTemp(false);
	return set;
}

static void
blob_write(SpillSet *set, int p, const StringInfoData *b)
{
	uint32		len = b->len;

	BufFileWrite(set->f[p], &len, sizeof(len));
	BufFileWrite(set->f[p], b->data, b->len);
	set->nblobs[p]++;
}

static char *
blob_read(BufFile *f)
{
	uint32		len;
	char	   *data;

	BufFileReadExact(f, &len, sizeof(len));
	data = palloc(len);
	BufFileReadExact(f, data, len);
	return data;
}

static void
part_put(void *arg, int p, StringInfo b)
{
	blob_write(arg, p, b);
}

/* the groups of the table to the partitions of the set, by the hash of their keys one level down; the table is left as it was */
static void
table_write(AggExec *ex, SpillSet *set)
{
	AggSpill   *sp = ex->spill;
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				ng = 0,
			   *gl = palloc(Max(n, 1) * sizeof(uint32)),
			   *part = palloc(Max(n, 1) * sizeof(uint32)),
			   *start = palloc0((sp->nparts + 1) * sizeof(uint32)),
			   *order,
			   *fill = palloc0(sp->nparts * sizeof(uint32));

	/* the float sums of the lanes done, and of the lane being scanned */
	if (ex->lane_serial)
		for (int i = 0; i < ex->spec->naggs; i++)
			if (ex->acc[i].dt)
				for (uint32 g = 0; g < n; g++)
					ex->acc[i].d[g] += ex->acc[i].dt[g];
	for (uint32 g = 0; g < n; g++)
		if (ex->seen[g] > 0)
		{
			uint32		h = blob_group_hash(ex, g);

			/* another hash of the same values at each level, so that a partition splits */
			h = hash_bytes_uint32(h ^ ((uint32) set->depth * 0x9E3779B9u));
			gl[ng] = g;
			part[ng] = h & (sp->nparts - 1);
			start[part[ng] + 1]++;
			ng++;
		}
	for (int p = 0; p < sp->nparts; p++)
		start[p + 1] += start[p];
	order = palloc(Max(ng, 1) * sizeof(uint32));
	for (uint32 r = 0; r < ng; r++)
		order[start[part[r]] + fill[part[r]]++] = gl[r];
	if (blob_splittable(ex))
		blob_dump_parts(ex, sp->nparts, order, start, fill, sp->tmp, part_put, set);
	else
		for (int p = 0; p < sp->nparts; p++)
		{
			StringInfoData b;

			MemoryContext old;

			if (fill[p] == 0)
				continue;
			old = MemoryContextSwitchTo(sp->tmp);
			initStringInfo(&b);
			blob_dump_list(ex, -1, order + start[p], fill[p], &b);
			blob_write(set, p, &b);
			MemoryContextSwitchTo(old);
			MemoryContextReset(sp->tmp);
		}
	for (int i = 0; i < ex->spec->naggs; i++)
		sp->amax[i] = Max(sp->amax[i], ex->amax[i]);
	pfree(gl);
	pfree(part);
	pfree(start);
	pfree(order);
	pfree(fill);
}

/* the table is full: its groups go to the partitions, and the scan starts a table again */
static void
spill_table(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	if (sp->set == NULL)
	{
		double		groups = Max(ex->spec->est_groups, 1.0),
					held = Max((double) (ex->hash ? ex->ngroups : ex->gcap), 1.0);
		int			want = SPILL_MIN_PARTS,
					cap = Max((int) sift_limit(group_spill_parts, SPILL_MAX_PARTS), SPILL_MIN_PARTS);

		/* partitions that each fit hash memory: the groups the planner expects over the groups this much memory held, twice over */
		while (want < cap && want < 2.0 * groups / held)
			want *= 2;
		sp->nparts = want;
		sp->set = set_new(sp, 0);
	}
	table_write(ex, sp->set);
	dist_note_spill(ex);
	sp->spilled = true;
	sp->keep = ex->hash ? ex->ngroups : 0;
	ex->n_spills++;
	table_reset(ex);
	MemoryContextSwitchTo(old);
}

/* after an item of the scan: a table past hash memory goes to disk */
void
spill_check(AggExec *ex)
{
	if (over_budget(ex))
		spill_table(ex);
}

static void
work_add(AggSpill *sp, SpillSet *set)
{
	for (int p = 0; p < sp->nparts; p++)
	{
		SpillWork  *w;

		if (set->nblobs[p] == 0)
		{
			BufFileClose(set->f[p]);
			continue;
		}
		w = palloc(sizeof(SpillWork));
		w->f = set->f[p];
		w->nblobs = set->nblobs[p];
		w->depth = set->depth;
		sp->work = lappend(sp->work, w);
	}
	pfree(set);
}

/* the scan is over: if the table was ever spilled, the rest of it goes to the partitions, which are the result from now on */
void
spill_finish(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	table_write(ex, sp->set);
	sp->keep = 0;
	table_reset(ex);
	/* the check of avg(float8) (final_checks) is that of the scan, not of the table that is left */
	for (int i = 0; i < ex->spec->naggs; i++)
		ex->amax[i] = sp->amax[i];
	work_add(sp, sp->set);
	sp->set = NULL;
	MemoryContextSwitchTo(old);
}

/* the next partition as the table, complete: false when there is none */
static bool
spill_next(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	while (sp->work != NIL)
	{
		SpillWork  *w = linitial(sp->work);
		SpillSet   *child = NULL;

		sp->work = list_delete_first(sp->work);
		table_reset(ex);
		SIFT_INVARIANT(BufFileSeek(w->f, 0, 0, SEEK_SET) == 0, "sift: could not rewind a partition of the aggregate");
		for (uint32 i = 0; i < w->nblobs; i++)
		{
			MemoryContext tmp = MemoryContextSwitchTo(sp->tmp);

			blob_merge(ex, blob_read(w->f));
			MemoryContextSwitchTo(tmp);
			MemoryContextReset(sp->tmp);
			if (!over_budget(ex))
				continue;
			if (w->depth + 1 >= (int) sift_limit(group_spill_depth, SPILL_MAX_DEPTH))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: a partition of the aggregate does not fit hash memory after %d levels of partitions", w->depth + 1),
						 errhint("Raise work_mem or hash_mem_multiplier, or set sift.enable_agg to off.")));
			if (child == NULL)
				child = set_new(sp, w->depth + 1);
			table_write(ex, child);
			table_reset(ex);
		}
		BufFileClose(w->f);
		if (child == NULL)
		{
			MemoryContextSwitchTo(old);
			return true;
		}
		table_write(ex, child);
		table_reset(ex);
		work_add(sp, child);
	}
	MemoryContextSwitchTo(old);
	return false;
}

/* the groups of the next partition are the ones to return */
bool
spill_more(AggExec *ex)
{
	if (!spill_started(ex) || !spill_next(ex))
		return false;
	if (ex->glist)
		pfree(ex->glist);
	/* the sets of a partition that went through the sort have their counts when it is done */
	dist_finish_all(ex);
	dist_end_all(ex);
	list_groups(ex);
	ex->gpos = 0;
	ex->spill->groups += ex->nglist;
	ex->n_groups = ex->spill->groups;
	return true;
}

/*
 * The groups of a spilled scan, one at a time: the table holds one partition, so a group that is returned is kept (in memory up to work_mem,
 * then on disk) for the rescan of the node, which replays them. next makes the next group of the table in slot, false when there is none.
 */
TupleTableSlot *
spill_output(AggExec *ex, TupleTableSlot *slot, bool (*next) (AggExec *, TupleTableSlot *))
{
	AggSpill   *sp = ex->spill;

	if (sp->out == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);

		sp->out = tuplestore_begin_heap(true, false, work_mem);
		sp->mslot = MakeSingleTupleTableSlot(slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
		MemoryContextSwitchTo(old);
	}
	if (sp->nread < sp->nwritten)
	{
		SIFT_INVARIANT(tuplestore_gettupleslot(sp->out, true, false, sp->mslot), "sift: a group of the aggregate scan is missing from its store");
		slot_getallattrs(sp->mslot);
		ExecClearTuple(slot);
		memcpy(slot->tts_values, sp->mslot->tts_values, slot->tts_tupleDescriptor->natts * sizeof(Datum));
		memcpy(slot->tts_isnull, sp->mslot->tts_isnull, slot->tts_tupleDescriptor->natts * sizeof(bool));
		sp->nread++;
		return ExecStoreVirtualTuple(slot);
	}
	if (sp->done || !next(ex, slot))
	{
		sp->done = true;
		return NULL;
	}
	tuplestore_puttupleslot(sp->out, slot);
	tuplestore_advance(sp->out, true);
	sp->nwritten++;
	sp->nread++;
	return slot;
}

/* the node is scanned again: the groups returned so far are replayed, the others made when they are asked for */
bool
spill_rescan(AggExec *ex)
{
	if (ex->spill == NULL || ex->spill->out == NULL)
		return false;
	tuplestore_rescan(ex->spill->out);
	ex->spill->nread = 0;
	return true;
}

void
spill_end(AggExec *ex)
{
	AggSpill   *sp = ex->spill;

	if (sp == NULL)
		return;
	if (sp->out)
		tuplestore_end(sp->out);
	if (sp->mslot)
		ExecDropSingleTupleTableSlot(sp->mslot);
	for (int p = 0; sp->set && p < sp->nparts; p++)
		BufFileClose(sp->set->f[p]);
	foreach_ptr(SpillWork, w, sp->work)
		BufFileClose(w->f);
}
