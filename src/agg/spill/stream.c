/*
 * agg/spill/stream.c - the sweep: a scan gives its groups in the order of the ORDER BY as the superblocks it reads are done
 * The superblocks in the order of their zones, the groups below the zone of the next one as a batch of the result, and the rest of the table kept for the next
 * (agg/spill.c has the partitions it goes on with when no group is final).
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/memutils.h"

#include "agg/agg.h"
#include "agg/par/int.h"
#include "agg/spill/int.h"

/* the sweep of gstream_begin: a place in the order of the result is the key (negated for a descending order), NULL at the end the ORDER BY puts it */
typedef __int128 Pos __attribute__((aligned(8)));	/* palloc gives 8 bytes */

#define POS_LOW		(-((Pos) 1 << 100))
#define POS_HIGH	((Pos) 1 << 100)

typedef struct GsSb
{
	uint32		sb;
	uint32		rows;			/* with a value for the key */
	Pos			lo;				/* the places the values of the superblock can have */
	Pos			hi;
	Pos			elo;			/* ... and where its rows are taken to be, for the estimate of what the sweep holds (an overflow value or a write buffer: anywhere in the others' range) */
	Pos			ehi;
	bool		inexact;
} GsSb;

typedef struct GStream
{
	uint32	   *sb;				/* the superblocks, in the order of the sweep */
	Pos		   *lo;				/* ... and the lowest place of each */
	uint32		n;
	uint32		next;			/* the superblock the sweep reads next */
	bool		desc;
	bool		nulls_first;
	bool		shown;			/* a batch is being returned: the table holds it, and `kept` the groups that are not final */
	bool		spilling;		/* no batch was to be had: the groups go to the partitions from here */
	bool		over;			/* the sweep is done */
	uint32		grew;			/* the most groups the table took from one superblock */
	StringInfoData kept;
	MemoryContext tmp;
} GStream;


/*
 * The sweep. The superblocks that hold rows are read in the order of the lowest place in the result that a group of each can have: a place is the key (negated for a
 * descending order), NULL at the end the ORDER BY puts it. A group whose place is below that of the next superblock cannot get a row from any superblock still to read, nor
 * from one read: it is final.
 */
static Pos
pos_of(const GStream *gs, int64 v)
{
	return gs->desc ? -(Pos) v : (Pos) v;
}

static Pos
null_pos(const GStream *gs)
{
	return gs->nulls_first ? POS_LOW : POS_HIGH;
}

/* the key for a value of its leaf: the value, or its step */
static int64
step_value(AggExec *ex, int64 z)
{
	int			ki = ex->spec->ord[0].key;
	const SiftAggKey *k = &ex->spec->key[ki];
	bool		isnull;

	if (k->kind == SAK_LEAF)
		return z;
	return sift_int_key(k->typid, key_input_datum(ex, ki, spill_int_datum(k->typid, z), &isnull));
}

static int
gssb_by_lo(const void *a, const void *b)
{
	const GsSb *x = a,
			   *y = b;

	if (x->lo != y->lo)
		return x->lo < y->lo ? -1 : 1;
	return x->sb < y->sb ? -1 : x->sb > y->sb;
}

/* the places of the values of a superblock, from its zone: no bound when it has an overflow value, and NULLs first are before every value */
static void
gssb_zone(AggExec *ex, const GStream *gs, const SiftSrcZone *zone, GsSb *e)
{
	e->lo = e->elo = POS_LOW;
	e->hi = e->ehi = POS_HIGH;
	e->rows = zone->nvals;
	e->inexact = zone->inexact;
	if (zone->inexact)
		return;
	e->rows = zone->nvals - zone->nnull;
	if (zone->nnull >= zone->nvals)
		e->lo = e->hi = null_pos(gs);
	else
	{
		Pos			a = pos_of(gs, step_value(ex, zone->min)),
					b = pos_of(gs, step_value(ex, zone->max));

		e->lo = e->elo = Min(a, b);
		e->hi = e->ehi = Max(a, b);
		/* the groups of NULL keys are held to the end, a few: they do not keep the others */
		if (zone->nnull > 0 && gs->nulls_first)
			e->lo = POS_LOW;
	}
}

/* a superblock with a value out of its zone, or rows no zone describes, is taken to have its rows anywhere in the range of the others */
static void
gs_spread(GsSb *z, uint32 n)
{
	Pos			lo = POS_HIGH,
				hi = POS_LOW;

	for (uint32 i = 0; i < n; i++)
		if (!z[i].inexact && z[i].rows > 0)
		{
			lo = Min(lo, z[i].lo);
			hi = Max(hi, z[i].hi);
		}
	for (uint32 i = 0; i < n; i++)
		if (z[i].inexact && lo <= hi)
		{
			z[i].elo = lo;
			z[i].ehi = hi;
		}
}

#define GS_ACTIVE	64

/*
 * The rows the sweep holds at most, at the end of a superblock and before its batch: the rows of the one just read and, of those before it that the zone of the next
 * one does not leave behind, the share above the place of the next (the rows are taken to be spread over the zone). -1 when the zones overlap so much that more than
 * GS_ACTIVE of them are in play at once.
 */
static double
gs_peak(const GsSb *z, uint32 n)
{
	uint32		act[GS_ACTIVE];
	uint32		na = 0;
	double		held = 0,
				peak = 0;

	for (uint32 i = 0; i < n; i++)
	{
		Pos			front = i + 1 < n ? z[i + 1].lo : POS_HIGH;
		uint32		k = 0;

		peak = Max(peak, held + z[i].rows);
		if (na == GS_ACTIVE)
			return -1;
		act[na++] = i;
		held = 0;
		for (uint32 a = 0; a < na; a++)
		{
			const GsSb *e = &z[act[a]];

			if (e->ehi < front)
				continue;
			held += e->rows * (front <= e->elo ? 1.0 : (double) (e->ehi - front) / (double) (e->ehi - e->elo));
			act[k++] = act[a];
		}
		na = k;
	}
	return peak;
}

/*
 * Does the scan give its groups as the superblocks are done? Only a scan that would spill, over superblocks whose zones leave groups final before the end: the rows
 * the sweep holds, at most (gs_peak), are less than half the rows of the scan. The write buffers are read in by the first begin; until then a range says it may hold anything.
 */
bool
gstream_begin(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	const SiftAggKey *k;
	SkipMap    *m;
	GStream    *gs;
	GsSb	   *z;
	double		total = 0,
				peak;
	uint32		nsb,
				n = 0;

	if (sp == NULL || !sp->ranged || !sift_aggspec_gstream(ex->spec) || !spill_lead_zoned(ex) || ex->spec->est_groups * 256.0 <= (double) sp->budget)
		return false;
	k = &ex->spec->key[ex->spec->ord[0].key];
	gs = MemoryContextAllocZero(ex->cxt, sizeof(GStream));
	gs->desc = sp->desc;
	gs->nulls_first = sp->nulls_first;
	nsb = sift_colsrc_nsb(ex->src);
	m = skip_map(ex);
	sift_colsrc_begin(ex->src, 0, 1, ex->prune, ex->nzone);
	z = palloc(Max(nsb, 1) * sizeof(GsSb));
	for (uint32 sb = ex->tid_sb_lo; sb < Min(nsb, ex->tid_sb_hi); sb++)
	{
		SiftSrcZone zone;

		if ((m->ok && !((m->kept[sb / 64] >> (sb % 64)) & 1)) || sift_colsrc_zone_range(ex->src, sb, sb + 1, k->leaf, &zone) == 0)
			continue;
		z[n].sb = sb;
		gssb_zone(ex, gs, &zone, &z[n]);
		total += z[n].rows;
		n++;
	}
	if (n < 2)
	{
		pfree(z);
		pfree(gs);
		return false;
	}
	qsort(z, n, sizeof(GsSb), gssb_by_lo);
	gs_spread(z, n);
	peak = gs_peak(z, n);
	if (peak < 0 || peak * 2.0 > total)
	{
		pfree(z);
		pfree(gs);
		return false;
	}
	gs->n = n;
	gs->sb = MemoryContextAlloc(ex->cxt, n * sizeof(uint32));
	gs->lo = MemoryContextAlloc(ex->cxt, (n + 1) * sizeof(Pos));
	for (uint32 i = 0; i < n; i++)
	{
		gs->sb[i] = z[i].sb;
		gs->lo[i] = z[i].lo;
	}
	gs->lo[n] = POS_HIGH;
	pfree(z);
	gs->tmp = AllocSetContextCreate(ex->cxt, "sift agg group stream", ALLOCSET_DEFAULT_SIZES);
	ex->gs = gs;
	/* the table starts again as a spilled scan's does: a hash table */
	table_reset(ex);
	return true;
}

/*
 * The groups of the table below the place of the next superblock are the batch, if they are a worthwhile share of it (an eighth: the others are dumped and loaded
 * again for each batch); the others are dumped for the table to start with them once the batch is returned
 */
static bool
gs_retire(AggExec *ex)
{
	GStream    *gs = ex->gs;
	AggSpill   *sp = ex->spill;
	Pos			front = gs->lo[gs->next];
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				nf = 0,
				nk = 0;
	uint32	   *fin = palloc(Max(n, 1) * sizeof(uint32)),
			   *rest = palloc(Max(n, 1) * sizeof(uint32));
	MemoryContext old;

	for (uint32 g = 0; g < n; g++)
	{
		int64		v;

		if (ex->seen[g] == 0)
			continue;
		if ((spill_lead_value(ex, g, &v) ? pos_of(gs, v) : null_pos(gs)) < front)
			fin[nf++] = g;
		else
			rest[nk++] = g;
	}
	if (nf == 0 || nf < (nf + nk) / 8)
	{
		pfree(fin);
		pfree(rest);
		return false;
	}
	old = MemoryContextSwitchTo(gs->tmp);
	initStringInfo(&gs->kept);
	MemoryContextSwitchTo(sp->tmp);
	if (nk > 0)
		blob_dump_list(ex, -1, rest, nk, &gs->kept);
	else
		resetStringInfo(&gs->kept);
	MemoryContextSwitchTo(old);
	MemoryContextReset(sp->tmp);
	pfree(rest);
	ex->glist = MemoryContextAlloc(ex->cxt, nf * sizeof(uint32));
	memcpy(ex->glist, fin, nf * sizeof(uint32));
	pfree(fin);
	ex->nglist = nf;
	sp->keep = ex->hash ? ex->ngroups : 0;
	gs->shown = true;
	return true;
}

/* the next batch of the sweep, ordered: false when the sweep is over (the partitions, if the table spilled, are what is left) */
bool
gstream_next(AggExec *ex)
{
	GStream    *gs = ex->gs;
	AggSpill   *sp = ex->spill;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	bool		got = false;

	if (gs->over)
		return false;
	if (ex->glist)
		pfree(ex->glist);
	ex->glist = NULL;
	if (gs->shown)
	{
		table_reset(ex);
		if (gs->kept.len > 0)
		{
			MemoryContextSwitchTo(sp->tmp);
			blob_merge(ex, gs->kept.data);
			MemoryContextSwitchTo(ex->cxt);
			MemoryContextReset(sp->tmp);
		}
		MemoryContextReset(gs->tmp);
		gs->shown = false;
	}
	while (!got && gs->next < gs->n)
	{
		uint32		sb = gs->sb[gs->next++];
		uint32		before = ex->ngroups,
					n;

		scan_units(ex, sb, sb + 1);
		n = ex->ngroups;
		gs->grew = Max(gs->grew, n > before ? n - before : 0);
		if (gs->spilling || gs->next >= gs->n || !spill_over(ex))
			continue;
		got = gs_retire(ex);
		/* nothing is final and the table is far past what the groups in hash memory, and a few superblocks, make: the zones overlap, and the rest of the scan is the spill's */
		if (!got && n > (uint64) n * sp->budget / Max(MemoryContextMemAllocated(ex->tcxt, true) - sp->floor, 1) + 3 * (uint64) gs->grew)
			gs->spilling = true;
	}
	if (!got)
	{
		gs->over = true;
		scan_tail(ex);
		if (!sp->spilled)
		{
			finish_groups(ex);
			got = ex->nglist > 0;
		}
	}
	MemoryContextSwitchTo(old);
	if (got)
	{
		if (ex->spec->gtopn_k > 0)
			groups_topn(ex);
		groups_order(ex);
		ex->n_gs++;
	}
	return got;
}


/* the table is looked at after each superblock, not after each unit: the spill (agg/spill.c) is for when no batch is to be had */
bool
gstream_holds(const AggExec *ex)
{
	return ex->gs != NULL && !ex->gs->spilling;
}
