/*
 * core/dirpart.c - the partitions of a directory update: lineages that name a partition across the steps that rewrite it, and the layout of the entries by partition
 *
 * An update that edits a partitioned directory (a key-cut index cut into partitions of the key) works on the entries as a list; the partition each entry is in is kept by generation
 * for the update, so that a split, a merge or a rewrite gives the partitions it writes the lineages the rules ask for, and the entries are laid out partition by partition at its end.
 */
#include "core/host.h"

#include "core/store_int.h"
#include "core/dirpart.h"

/* The partitions of the update in hand, for sift_dir_part_replace: the mutators run inside it and are not passed it */
static SiftDirParts *cur = NULL;

static void
part_add(SiftDirParts *e, uint64 gen, uint64 lineage, const uint8 *lo)
{
	if (e->n == e->max)
	{
		e->max = Max(16, 2 * e->max);
		e->of = e->of ? repalloc(e->of, e->max * sizeof(SiftPartOf)) : palloc(e->max * sizeof(SiftPartOf));
	}
	e->of[e->n] = (SiftPartOf) {.gen = gen,.lineage = lineage};
	if (lo != NULL)
		memcpy(e->of[e->n].lo, lo, SIFT_BOUND_BYTES);
	e->n++;
}

static int
cmp_partof(const void *a, const void *b)
{
	return pg_cmp_u64(((const SiftPartOf *) a)->gen, ((const SiftPartOf *) b)->gen);
}

/* qsort and bsearch take a base that is never NULL: the list has none until its first partition */
static void
part_sort(SiftDirParts *e)
{
	if (e->n > 1)
		qsort(e->of, e->n, sizeof(SiftPartOf), cmp_partof);
}

static const SiftPartOf *
part_of(const SiftDirParts *e, uint64 gen)
{
	SiftPartOf		key = {.gen = gen};

	return e->n > 0 ? bsearch(&key, e->of, e->n, sizeof(SiftPartOf), cmp_partof) : NULL;
}

/*
 * The entries [first, first + n) of the old directory are in the partitions its chain lists: their lineages are the ones the update starts from, and an entry the update
 * leaves in the directory stays in its partition.
 */
void
sift_dirpart_open(SiftDirParts *e, const SiftDirChain *chain, const SiftSegEntry *old)
{
	int			at = 0;

	for (uint32 k = 0; k < chain->nleaves; at += chain->leaf[k].n, k++)
		if (chain->leaf[k].lineage != 0)
			for (uint32 i = 0; i < chain->leaf[k].n; i++)
				part_add(e, old[at + i].gen, chain->leaf[k].lineage, chain->leaf[k].lo);
	part_sort(e);
}

/*
 * The entries neu[0..nneu), in the order of the key, take the place of the entries old[0..nold) as partitions of their own, one each, from the partitions of the old ones:
 * a partition is cut in several (a split): the one that holds the tail of its range keeps its lineage, the others are new; several are made one (a merge): the lowest lineage
 * survives; as many are made as there were (a rewrite): each keeps its lineage. A lineage that no entry of the new directory has is gone. A new lineage is the generation of
 * the first entry of its partition, which no other has had. lo: the lower bound of each partition (NULL: unknown, all zero). Called by a mutator, whose entries are in the working copy of the directory.
 */
void
sift_dir_part_replace(const uint64 *old, int nold, const SiftSegEntry *neu, int nneu, const uint8 (*lo)[SIFT_BOUND_BYTES])
{
	SiftDirParts *e = cur;
	uint64	   *lin = palloc(Max(nold, 1) * sizeof(uint64));
	int			m = 0;

	Assert(e != NULL);
	for (int i = 0; i < nold; i++)
	{
		const SiftPartOf *p = part_of(e, old[i]);

		if (p != NULL)
		{
			bool		seen = false;

			for (int k = 0; k < m && !seen; k++)
				seen = lin[k] == p->lineage;
			if (!seen)
				lin[m++] = p->lineage;
		}
	}
	for (int i = 0; i < nold; i++)
	{
		SiftPartOf *gone = (SiftPartOf *) part_of(e, old[i]);

		if (gone != NULL)
			gone->gen = PG_UINT64_MAX;
	}
	part_sort(e);
	while (e->n > 0 && e->of[e->n - 1].gen == PG_UINT64_MAX)
		e->n--;
	for (int j = 0; j < nneu; j++)
	{
		uint64		id = neu[j].gen;

		if (m > 0 && nneu == 1)
		{
			id = lin[0];
			for (int k = 1; k < m; k++)
				id = Min(id, lin[k]);
		}
		else if (m > 0 && nneu < m)
		{
			id = lin[(int) ((int64) j * m / nneu)];
			for (int k = (int) ((int64) j * m / nneu); k < (int) ((int64) (j + 1) * m / nneu); k++)
				id = Min(id, lin[k]);
		}
		else if (m > 0 && nneu - 1 - j < m)
			id = lin[m - 1 - (nneu - 1 - j)];
		part_add(e, neu[j].gen, id, lo ? lo[j] : NULL);
	}
	pfree(lin);
	part_sort(e);
}

/* The entries neu[0..nneu) take the place of old[0..nold) inside the one partition those were in: they are in it too. Entries of several partitions, or of none, leave them in no partition */
void
sift_dir_part_inherit(const uint64 *old, int nold, const SiftSegEntry *neu, int nneu)
{
	SiftDirParts *e = cur;
	const SiftPartOf *found = NULL;
	SiftPartOf	home;

	Assert(e != NULL);
	for (int i = 0; i < nold; i++)
	{
		const SiftPartOf *p = part_of(e, old[i]);

		if (p == NULL || (found != NULL && found->lineage != p->lineage))
			return;
		found = p;
	}
	if (found == NULL)
		return;
	home = *found;
	for (int j = 0; j < nneu; j++)
		part_add(e, neu[j].gen, home.lineage, home.lo);
	part_sort(e);
}

typedef struct PartGroup
{
	uint64		lineage;
	const uint8 *lo;
	int			first;			/* the position of its first entry, which orders the partitions of one bound */
	int			n;
} PartGroup;

static int
cmp_group(const void *a, const void *b)
{
	const PartGroup *x = a,
			   *y = b;
	int			c = memcmp(x->lo, y->lo, SIFT_BOUND_BYTES);

	return c != 0 ? c : (x->first > y->first) - (x->first < y->first);
}

/*
 * The entries of no partition first, in the order they have, then each partition's entries in the order they have, the partitions by bound: the directory of a partitioned index
 * as the chain keeps it (the entries of a leaf are the entries of one partition). Returns the partitions, in directory order after the move.
 */
int
sift_dirpart_arrange(Relation index, const SiftDirParts *e, SiftSegEntry *segs, int n, SiftDirPart **parts)
{
	const SiftPartOf **of = palloc(Max(n, 1) * sizeof(SiftPartOf *));
	PartGroup  *g = palloc(Max(n, 1) * sizeof(PartGroup));
	SiftSegEntry *out = palloc(Max(n, 1) * sizeof(SiftSegEntry));
	int			ng = 0,
				at = 0;

	for (int i = 0; i < n; i++)
		of[i] = part_of(e, segs[i].gen);
	for (int i = 0; i < n; i++)
	{
		int			k = 0;

		if (of[i] == NULL)
			continue;
		while (k < ng && g[k].lineage != of[i]->lineage)
			k++;
		if (k == ng)
			g[ng++] = (PartGroup) {of[i]->lineage, of[i]->lo, i, 0};
		g[k].n++;
	}
	for (int k = 0; k < ng; k++)
		if ((uint32) g[k].n > sift_dirchain_leaf_cap())
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" has a partition of %d entries, a leaf of its directory holds %u", RelationGetRelationName(index), g[k].n,
							sift_dirchain_leaf_cap()),
					 errhint("Merge the segments of the partition first (sift.merge()).")));
	qsort(g, ng, sizeof(PartGroup), cmp_group);
	for (int i = 0; i < n; i++)
		if (of[i] == NULL)
			out[at++] = segs[i];
	*parts = palloc(Max(ng, 1) * sizeof(SiftDirPart));
	for (int k = 0; k < ng; k++)
	{
		(*parts)[k] = (SiftDirPart) {.lineage = g[k].lineage,.first = at,.n = g[k].n};
		memcpy((*parts)[k].lo, g[k].lo, SIFT_BOUND_BYTES);
		for (int i = 0; i < n; i++)
			if (of[i] != NULL && of[i]->lineage == g[k].lineage)
				out[at++] = segs[i];
	}
	memcpy(segs, out, n * sizeof(SiftSegEntry));
	pfree(of);
	pfree(g);
	pfree(out);
	return ng;
}

void
sift_dirpart_enter(SiftDirParts *e)
{
	cur = e;
}

void
sift_dirpart_leave(SiftDirParts *e)
{
	cur = NULL;
	if (e->of != NULL)
		pfree(e->of);
}

int
sift_dir_parts_read(Relation index, const SiftDirSnapshot *snap, SiftSegEntry **segs, SiftDirPart **parts)
{
	SiftDirChain chain;

	*parts = NULL;
	if (snap->nchain == 0)
	{
		*segs = snap->segs;
		return 0;
	}
	(void) sift_dirchain_read(index, snap->chain[0], snap->version, 0, segs, &chain);
	return sift_dirchain_parts(&chain, parts);
}
