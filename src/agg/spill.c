/*
 * agg/spill.c - the group table past hash memory: partial states in partitions on disk
 *
 * The group table of a scan is in memory. When, at the end of an item, it holds more than hash memory (work_mem times
 * hash_mem_multiplier), its groups go to a set of partitions as blobs (agg/blob/, the partial tables of the workers): a group goes to the
 * partition of the hash of its key values, so every table that ever held the group wrote it to the same one. The table is reset and the
 * scan goes on. When the scan ends, the last table goes to the partitions too, and each partition is merged into a table of its own, by
 * value, and returned as the groups of the result before the next is read. A partition that does not fit hash memory when merged goes to a
 * set of partitions one level deeper (another hash of the same values), as a hash aggregate's spill does; each level is one more write and
 * read of the partial states, never a second scan of the table.
 *
 * A scan that gives its groups in an order (an ORDER BY whose first key is an integer, a date or a timestamp: sift_aggspec_ord_range) has partitions that are
 * ranges of that key instead, merged in the order of the key, each sorted by itself (agg/gorder.c), so the groups come out in order with the memory of one range.
 * When the key is the value of a leaf (or a step of it) the zones of the superblocks give its range and the tables go to equal widths of it at once, so a range is
 * final after one read; otherwise the tables go to one stream, and a sample of the key of the groups written (SPILL_SAMPLE values, each group as likely to be in
 * it as another) gives the points that cut the stream into ranges of as many groups. A range that does not fit is cut again by the points of the sample inside it.
 * A range of one value that does not fit is merged as it is. The range of a group depends on its key alone, so the groups of one value are never apart.
 *
 * Only the specs agg/spec.c names (sift_aggspec_spillable) take this path: a table is reset between two items, which the lanes of a parallel
 * scan do not allow, and the states must be blobs. The float sums of a group are added in the order of its partial states: the lane order
 * of the scan inside a table, the order of the spills between tables.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/tuplestore.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/agg.h"
#include "agg/back.h"
#include "agg/colsrc.h"
#include "agg/spec.h"
#include "core/limits.h"
#include "agg/agg_int.h"

#define SPILL_MIN_PARTS		8
#define SPILL_MAX_PARTS		128		/* one BufFile (8 kB) each */
#define SPILL_MAX_DEPTH		8
#define SPILL_SAMPLE		8192	/* values of the first order key of the groups written, for the points that cut the ranges */
#define SPILL_MIN_GROUPS	128		/* a table with fewer groups is not spilled: its memory is something else (the key ranges) */

typedef struct SpillSet
{
	int			depth;
	int			n;				/* partitions: for ranges, those of the values and one more for NULL */
	int64	   *bound;			/* ranges: n - 2 points, ascending; partition p < n - 1 holds the values from bound[p - 1] up to bound[p] (those of its parent at the ends) */
	BufFile    *f[SPILL_MAX_PARTS + 1];
	uint32		nblobs[SPILL_MAX_PARTS + 1];
} SpillSet;

typedef struct SpillWork
{
	BufFile    *f;
	uint32		nblobs;
	int			depth;
	int64		lo;				/* ranges: the values the partition holds, lo to hi */
	int64		hi;
	bool		nulls;			/* ranges: the partition of the NULL keys */
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
	bool		replayed;		/* the node was scanned again: the store is read as well as written (the groups made since are stepped over) */
	bool		done;
	MemoryContext tmp;			/* the temporaries of one blob, dumped or merged (agg/blob/ keeps them for the life of a process) */
	SpillSet   *set;			/* the level 0 partitions the scan writes */
	List	   *work;			/* partitions to merge, as SpillWork */
	double	   *amax;			/* avg(float8): the largest absolute input of the tables written */
	bool		ranged;			/* the partitions are ranges of the first order key, in the order of the result */
	bool		desc;
	bool		nulls_first;
	pg_prng_state prng;
	int64	   *sample;			/* ranged: values of the first order key of the groups written to the stream */
	uint32		nsample;
	uint64		nseen;
	uint64		nstream;		/* ranged: groups written to the stream */
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
	if (ex->spec->nord > 0)
	{
		bool		reverse = false;

		sp->ranged = true;
		sp->nulls_first = ex->spec->ord[0].nulls_first;
		sp->sample = MemoryContextAlloc(ex->cxt, SPILL_SAMPLE * sizeof(int64));
		pg_prng_seed(&sp->prng, 42);
		get_equality_op_for_ordering_op(ex->spec->ord[0].sortop, &reverse);
		sp->desc = reverse;
	}
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
 * Would the table be spilled now, had the spill been armed? A streaming count(DISTINCT) scan (agg/dist/) holds the spill back, since a partial
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
set_new(int depth, int n)
{
	SpillSet   *set = palloc0(sizeof(SpillSet));

	set->depth = depth;
	set->n = n;
	for (int p = 0; p < n; p++)
		set->f[p] = BufFileCreateTemp(false);
	return set;
}

/* the first order key of group g, as the integer that sorts as it does; false for NULL */
static bool
lead_value(AggExec *ex, uint32 g, int64 *v)
{
	int			j = ex->spec->ord[0].key;
	bool		isnull;
	Datum		d = key_id_datum(ex, j, key_id(ex, j, g), &isnull);

	if (isnull)
		return false;
	switch (ex->spec->key[j].typid)
	{
		case INT2OID:
			*v = DatumGetInt16(d);
			break;
		case INT4OID:
		case DATEOID:
			*v = DatumGetInt32(d);
			break;
		default:
			*v = DatumGetInt64(d);
	}
	return true;
}

/*
 * The range of the first order key in the zones of the superblocks the scan reads, when the key is the value of a leaf of its type or a step of it (so that the
 * zones of the leaf are in the domain of the key): false when there is none
 */
static bool
lead_domain(AggExec *ex, int64 *lo, int64 *hi)
{
	const SiftAggSpec *s = ex->spec;
	const SiftAggKey *k = &s->key[s->ord[0].key];
	bool		any = false;
	uint32		nsb;

	if ((k->kind != SAK_LEAF && k->kind != SAK_LUT) || k->leaf < 0 || k->leaf >= ex->nbase || ex->src == NULL || !sift_colsrc_has_zone_range(ex->src) ||
		ex->lay->leaf[k->leaf].typid != k->typid || (ex->lay->leaf[k->leaf].vtype != SCV_I32 && ex->lay->leaf[k->leaf].vtype != SCV_I64))
		return false;
	nsb = sift_colsrc_nsb(ex->src);
	for (uint32 sb = ex->tid_sb_lo; sb < Min(nsb, ex->tid_sb_hi); sb++)
	{
		SiftSrcZone zone;

		if (sift_colsrc_zone_range(ex->src, sb, sb + 1, k->leaf, &zone) == 0 || zone.inexact || zone.nnull >= zone.nvals)
			continue;
		*lo = any ? Min(*lo, zone.min) : zone.min;
		*hi = any ? Max(*hi, zone.max) : zone.max;
		any = true;
	}
	return any && *hi > *lo;
}

/* the first set of the spill of a ranged scan: equal widths of the key's range when the zones give it, else one stream */
static SpillSet *
first_set(AggExec *ex)
{
	int64		lo,
				hi;
	int			nv = SPILL_MAX_PARTS - 1;
	SpillSet   *set;

	if (!lead_domain(ex, &lo, &hi))
		return set_new(0, 1);
	set = set_new(0, nv + 1);
	set->bound = MemoryContextAlloc(ex->cxt, (nv - 1) * sizeof(int64));
	for (int i = 1; i < nv; i++)
		set->bound[i - 1] = lo + (int64) ((double) ((uint64) hi - (uint64) lo) * i / nv);
	return set;
}

static void
sample_add(AggSpill *sp, int64 v)
{
	sp->nseen++;
	if (sp->nsample < SPILL_SAMPLE)
		sp->sample[sp->nsample++] = v;
	else
	{
		uint64		i = pg_prng_uint64_range(&sp->prng, 0, sp->nseen - 1);

		if (i < SPILL_SAMPLE)
			sp->sample[i] = v;
	}
}

/* the partition of a value: the number of points at or below it */
static int
range_part(const SpillSet *set, int64 v)
{
	int			lo = 0,
				hi = set->n - 2;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (set->bound[mid] <= v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
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
			   *start = palloc0((set->n + 1) * sizeof(uint32)),
			   *order,
			   *fill = palloc0(set->n * sizeof(uint32));

	/* the float sums of the lanes done, and of the lane being scanned */
	if (ex->lane_serial)
		for (int i = 0; i < ex->spec->naggs; i++)
			if (ex->acc[i].dt)
				for (uint32 g = 0; g < n; g++)
					ex->acc[i].d[g] += ex->acc[i].dt[g];
	for (uint32 g = 0; g < n; g++)
		if (ex->seen[g] > 0)
		{
			gl[ng] = g;
			if (sp->ranged)
			{
				int64		v;

				if (!lead_value(ex, g, &v))
					part[ng] = set->n - 1;
				else
				{
					if (set->depth == 0)
						sample_add(sp, v);
					part[ng] = set->bound ? range_part(set, v) : 0;
				}
				sp->nstream += set->depth == 0;
			}
			else
			{
				/* another hash of the same values at each level, so that a partition splits */
				uint32		h = hash_bytes_uint32(blob_group_hash(ex, g) ^ ((uint32) set->depth * 0x9E3779B9u));

				part[ng] = h & (set->n - 1);
			}
			start[part[ng] + 1]++;
			ng++;
		}
	for (int p = 0; p < set->n; p++)
		start[p + 1] += start[p];
	order = palloc(Max(ng, 1) * sizeof(uint32));
	for (uint32 r = 0; r < ng; r++)
		order[start[part[r]] + fill[part[r]]++] = gl[r];
	if (blob_splittable(ex))
		blob_dump_parts(ex, set->n, order, start, fill, sp->tmp, part_put, set);
	else
		for (int p = 0; p < set->n; p++)
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

	if (sp->set == NULL && sp->ranged)
		sp->set = first_set(ex);
	else if (sp->set == NULL)
	{
		double		groups = Max(ex->spec->est_groups, 1.0),
					held = 0;
		int			want = SPILL_MIN_PARTS,
					cap = Max((int) sift_limit(group_spill_parts, SPILL_MAX_PARTS), SPILL_MIN_PARTS);

		/* the groups this much memory held (a direct table counts the groups the scan made, not the range it has room for) */
		for (uint32 g = 0; g < (ex->hash ? ex->ngroups : ex->gcap); g++)
			held += ex->seen[g] > 0;
		held = Max(held, 1.0);
		/* partitions that each fit hash memory: the groups the planner expects over the groups this much memory held, twice over */
		while (want < cap && want < 2.0 * groups / held)
			want *= 2;
		sp->nparts = want;
		sp->set = set_new(0, want);
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

/*
 * The partitions of a set that have groups become work: after the others for a hash, and for ranges before them, in the order of the result (the NULL keys first or
 * last as the ORDER BY says, the values ascending or descending); parent is the work the set was made from
 */
static void
work_add(AggSpill *sp, SpillSet *set, const SpillWork *parent)
{
	List	   *kids = NIL;
	int			nv = set->bound ? set->n - 1 : 0;

	for (int i = 0; i < set->n; i++)
	{
		int			p = i;
		SpillWork  *w;

		if (sp->ranged && set->bound)
		{
			/* the null partition is the last of the set; the order of the result puts it at an end */
			if (i == 0 && sp->nulls_first)
				p = nv;
			else if (i == set->n - 1 && !sp->nulls_first)
				p = nv;
			else
			{
				int			k = i - (sp->nulls_first ? 1 : 0);

				p = sp->desc ? nv - 1 - k : k;
			}
		}
		if (set->nblobs[p] == 0)
		{
			BufFileClose(set->f[p]);
			continue;
		}
		w = palloc0(sizeof(SpillWork));
		w->f = set->f[p];
		w->nblobs = set->nblobs[p];
		w->depth = set->depth;
		if (sp->ranged)
		{
			w->lo = parent->lo;
			w->hi = parent->hi;
			if (set->bound)
			{
				w->nulls = p == nv;
				if (p > 0 && p < nv)
					w->lo = set->bound[p - 1];
				if (p < nv - 1)
					w->hi = set->bound[p] - 1;
			}
		}
		if (sp->ranged)
			kids = lappend(kids, w);
		else
			sp->work = lappend(sp->work, w);
	}
	sp->work = list_concat(kids, sp->work);
	pfree(set);
}

static int
int64_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

/*
 * The ranges a partition of values is cut into: as many groups each as the table held when it was full, from the points of the sample inside its range; NULL
 * when it cannot be cut (one value, or the sample has no two apart), and then it is merged as it is
 */
static SpillSet *
range_set(AggExec *ex, const SpillWork *w)
{
	AggSpill   *sp = ex->spill;
	int64	   *v = palloc(Max(sp->nsample, 1) * sizeof(int64)),
			   *pts = palloc(SPILL_MAX_PARTS * sizeof(int64));
	uint32		n = 0;
	int			k = SPILL_MIN_PARTS,
				m = 0;
	SpillSet   *set;

	if (w->nulls || w->depth >= SPILL_MAX_DEPTH)
		return NULL;
	for (uint32 i = 0; i < sp->nsample; i++)
		if (sp->sample[i] >= w->lo && sp->sample[i] <= w->hi)
			v[n++] = sp->sample[i];
	if (n < 2)
		return NULL;
	qsort(v, n, sizeof(int64), int64_cmp);
	if (w->depth == 0)
		while (k < SPILL_MAX_PARTS && k < 2.0 * sp->nstream / Max(ex->ngroups, 1))
			k *= 2;
	for (int i = 1; i < k; i++)
	{
		int64		x = v[(uint64) i * n / k];

		if (x > w->lo && (m == 0 || x > pts[m - 1]))
			pts[m++] = x;
	}
	pfree(v);
	if (m == 0)
		return NULL;
	set = set_new(w->depth + 1, m + 2);
	set->bound = MemoryContextAlloc(ex->cxt, m * sizeof(int64));
	memcpy(set->bound, pts, m * sizeof(int64));
	pfree(pts);
	return set;
}

/* the scan is over: if the table was ever spilled, the rest of it goes to the partitions, which are the result from now on */
void
spill_finish(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	SpillWork	root = {.lo = PG_INT64_MIN,.hi = PG_INT64_MAX};
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	table_write(ex, sp->set);
	sp->keep = 0;
	table_reset(ex);
	/* the check of avg(float8) (final_checks) is that of the scan, not of the table that is left */
	for (int i = 0; i < ex->spec->naggs; i++)
		ex->amax[i] = sp->amax[i];
	work_add(sp, sp->set, &root);
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
		bool		whole = false;	/* ranges: this one cannot be cut */

		sp->work = list_delete_first(sp->work);
		table_reset(ex);
		SIFT_INVARIANT(BufFileSeek(w->f, 0, 0, SEEK_SET) == 0, "sift: could not rewind a partition of the aggregate");
		for (uint32 i = 0; i < w->nblobs; i++)
		{
			MemoryContext tmp = MemoryContextSwitchTo(sp->tmp);

			blob_merge(ex, blob_read(w->f));
			MemoryContextSwitchTo(tmp);
			MemoryContextReset(sp->tmp);
			if (whole || !over_budget(ex))
				continue;
			if (sp->ranged)
			{
				if (child == NULL && (child = range_set(ex, w)) == NULL)
				{
					whole = true;
					continue;
				}
			}
			else
			{
				if (w->depth + 1 >= (int) sift_limit(group_spill_depth, SPILL_MAX_DEPTH))
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: a partition of the aggregate does not fit hash memory after %d levels of partitions", w->depth + 1),
							 errhint("Raise work_mem or hash_mem_multiplier, or set sift.enable_agg to off.")));
				if (child == NULL)
					child = set_new(w->depth + 1, sp->nparts);
			}
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
		work_add(sp, child, w);
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
	if (ex->spec->gtopn_k > 0)
		groups_topn(ex);
	if (ex->spec->nord > 0)
		groups_order(ex);
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

	if (sp->out == NULL && ex->rescannable)
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
	if (sp->out)
	{
		/* until the node is scanned again the store is only appended to: a read after every write is a system call each */
		tuplestore_puttupleslot(sp->out, slot);
		if (sp->replayed)
			tuplestore_advance(sp->out, true);
		sp->nwritten++;
		sp->nread++;
	}
	return slot;
}

/* the node is scanned again: the groups returned so far are replayed, the others made when they are asked for (a node that cannot be scanned again keeps none) */
bool
spill_rescan(AggExec *ex)
{
	if (ex->spill == NULL || ex->spill->out == NULL)
		return false;
	tuplestore_rescan(ex->spill->out);
	ex->spill->nread = 0;
	ex->spill->replayed = true;
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
	for (int p = 0; sp->set && p < sp->set->n; p++)
		BufFileClose(sp->set->f[p]);
	foreach_ptr(SpillWork, w, sp->work)
		BufFileClose(w->f);
}
