/*
 * col/side_valid.c - which runs of a scope sidecar a read may take (col/side.h, col/scope.h)
 *
 * The runs of a segment hold the rows of the members that had a pair with their key when the sidecar was written, as of the stamp it holds. A read of the key under a snapshot may take a run
 * when no member within the run's span (the zones of its units) has a pair that changed since that stamp: the journal lists the members whose pairs of the key changed in (stamp, snapshot]
 * (sift_scope_changed), ended or inserted, and a member inside the span of a run is one whose rows the run holds or sits among. A member outside every run's span has no row in a run's units, so a
 * pair of it that changed does not touch the runs. A text, varchar or uuid member has no span: the journal names it by a 64-bit word of its bytes, and the run keeps a Bloom filter of the words of
 * every member its units hold, so that a member is taken to be inside the run when its word may be in the filter (a word that is in it by chance only makes the run give up). The rows of the units a run does not stand for are read by reference with the members the read lists under the snapshot, which are exact. When the journal cannot say (it does not vouch for the stamp,
 * or more members changed than SIDE_CHANGED_CAP) no run of the segment is taken. Whether the units of a run are clean and all-visible is the scan's to say (col/scan/side.c).
 */
/* check-memory: allow-allocs the members that changed for the keys of one read, at most SIDE_CHANGED_CAP, and one verdict for each segment of the index */
#include "postgres.h"

#include "col/side.h"

typedef struct SideVerdict
{
	uint64		gen;
	bool		ok;
	SiftConnKeys changed;
	const SiftSide *sd;
	const SideScope *sc;
} SideVerdict;

struct SideValid
{
	Relation	index;
	int			scope;
	Snapshot	snap;
	SiftKeyMap *km;
	const int64 *keys;
	uint32		nkeys;
	MemoryContext mcxt;
	SideVerdict *seg;
	uint32		nseg;
	const uint64 *words;		/* the words of the members the read lists, ascending as the int64 the journal keys are (a text, varchar or uuid member) */
	uint32		nwords;
};

SideValid *
sift_side_valid_open(Relation index, int scope, Snapshot snap, SiftKeyMap *km, const int64 *keys, uint32 nkeys, MemoryContext mcxt)
{
	SideValid  *v = MemoryContextAllocZero(mcxt, sizeof(SideValid));

	*v = (SideValid) {index, scope, snap, km, keys, nkeys, mcxt, NULL, 0, NULL, 0};
	return v;
}

static SideVerdict *
verdict_of(SideValid *v, SiftColSegment *seg)
{
	SideVerdict *sv;
	const SiftSide *sd;
	const SideScope *sc;
	MemoryContext old;

	for (uint32 i = 0; i < v->nseg; i++)
		if (v->seg[i].gen == seg->entry.gen)
			return &v->seg[i];
	old = MemoryContextSwitchTo(v->mcxt);
	v->seg = v->seg ? repalloc(v->seg, (v->nseg + 1) * sizeof(SideVerdict)) : palloc(sizeof(SideVerdict));
	sv = &v->seg[v->nseg++];
	*sv = (SideVerdict) {.gen = seg->entry.gen};
	sd = sift_cseg_side(seg);
	sc = sd != NULL ? sift_side_scope(sd, v->scope) : NULL;
	sv->sd = sd;
	sv->sc = sc;
	sv->ok = sc != NULL && sc->stamp[0].lineage != 0 &&
		sift_scope_changed(v->index, v->scope, sc->stamp, v->snap, SIDE_CHANGED_CAP, v->keys, v->nkeys, v->km, &sv->changed) == SIFT_CONN_OK;
	MemoryContextSwitchTo(old);
	return sv;
}

/* The first changed member that is not below lo */
static uint32
first_not_below(const SiftConnKeys *c, int64 lo)
{
	uint32		a = 0,
				b = c->n;

	while (a < b)
	{
		uint32		mid = a + (b - a) / 2;

		if (c->keys[mid] < lo)
			a = mid + 1;
		else
			b = mid;
	}
	return a;
}

bool
sift_side_run_untouched(SiftColSegment *seg, const SiftSide *sd, const SideScope *sc, const SiftConnKeys *changed, const SideRun *run)
{
	uint64		nbits;
	uint64	   *bits;
	bool		hit = false;

	if (sc->member_kind == SIDE_MEMBER_RANGE)
	{
		uint32		at = first_not_below(changed, run->lo);

		return at == changed->n || changed->keys[at] > run->hi;
	}
	if (changed->n == 0)
		return true;
	bits = sift_side_run_filter(seg, sd, run, &nbits);
	for (uint32 i = 0; i < changed->n && !hit; i++)
		hit = sift_side_filter_has(bits, nbits, (uint64) changed->keys[i]);
	pfree(bits);
	return !hit;
}

static int
cmp_word(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return (x > y) - (x < y);
}

void
sift_side_valid_members(SideValid *v, const uint64 *words, uint32 n)
{
	uint64	   *sorted = MemoryContextAlloc(v->mcxt, Max(n, 1) * sizeof(uint64));

	memcpy(sorted, words, n * sizeof(uint64));
	qsort(sorted, n, sizeof(uint64), cmp_word);
	v->words = sorted;
	v->nwords = n;
}

bool
sift_side_members_covered(void *arg, SiftColSegment *seg)
{
	SideValid  *v = arg;
	SideVerdict *sv = verdict_of(v, seg);
	uint32		a = 0,
				b = 0;

	if (!sv->ok)
		return false;
	while (a < v->nwords && b < sv->changed.n)
	{
		int64		w = (int64) v->words[a],
					c = sv->changed.keys[b];

		if (w == c)
			return false;
		if (w < c)
			a++;
		else
			b++;
	}
	return true;
}

bool
sift_side_run_valid(void *arg, SiftColSegment *seg, const SideRun *run)
{
	SideVerdict *sv = verdict_of(arg, seg);

	return sv->ok && sift_side_run_untouched(seg, sv->sd, sv->sc, &sv->changed, run);
}
