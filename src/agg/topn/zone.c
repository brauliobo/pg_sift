/*
 * agg/topn/zone.c - the zones, the probes and the visit order
 * Which superblocks the zones cut, the probes that tell whether a key follows the table,
 * and the order and the cache of the walk over the superblocks.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"

#include "agg/topn/int.h"

/* The superblocks by the best row they can hold, key by key: a unit whose bound on the keys so far ties with another is ordered by the next key */
typedef struct WalkSort
{
	const Rank *bound;
	int			wkeys;
} WalkSort;

static MemoryContext walk_cx;

static WalkBase *walk_kept[WALK_CACHED];

static uint64 walk_clock;

bool
sift_topn_wants_zones(const SiftTopN *t)
{
	return !t->all && !t->totals && t->key[0].leaf >= 0 && t->key[0].nenum == 0 && (t->key[0].vtype == SCV_I32 || t->key[0].vtype == SCV_I64);
}

/*
 * Can a row of a unit with this zone of the first sort column sort before the n-th best so far (or tie with it)? Nulls sort as the query
 * says, the rest between min and max. Rows tied on the first key may win on the others, so a bound equal to the n-th's is kept; with one key
 * a tie wins nothing (the query does not say which of the tied rows comes), so only a better bound is. The zone counts the dead rows too: it
 * only ever says "no" for a unit that has nothing better.
 */
bool
sift_topn_zone_viable(const SiftTopN *t, const SiftSrcZone *zone)
{
	const TopKey *k = &t->key[0];
	SiftSrcZone zk = key_zone(k, zone);
	const SiftSrcZone *z = &zk;
	Rank		bar,
				best;

	if (t->all || z->inexact || !bar_rank(t, &bar))
		return true;
	if (z->nnull > 0 && k->nulls_first)
		best = (Rank) {0, 0};
	else if (z->nnull == z->nvals)
		best = (Rank) {k->nulls_first ? 0 : 2, 0};
	else
		best = rank_of(k, false, k->desc ? z->max : z->min);
	return t->nkey == 1 ? rank_after(bar, best) : !rank_after(best, bar);
}

/* The zone of the unit of superblock sb, probed before the scan; it may be passed in any order */
void
sift_topn_probe(SiftTopN *t, uint32 sb, const SiftSrcZone *zone)
{
	SiftSrcZone zk = key_zone(&t->key[0], zone);
	const SiftSrcZone *z = &zk;

	if (t->nprobe < TOPN_PROBES && !z->inexact && z->nnull < z->nvals)
	{
		t->probe[t->nprobe].sb = sb;
		t->probe[t->nprobe++].best = t->key[0].desc ? z->max : z->min;
	}
}

/* The superblocks to probe: spread over the table */
uint32
sift_topn_probe_sb(const SiftTopN *t, int i, uint32 nsb)
{
	return (uint32) (((uint64) i * (nsb - 1)) / (TOPN_PROBES - 1));
}

int
sift_topn_nprobes(void)
{
	return TOPN_PROBES;
}

void
sift_topn_start(SiftTopN *t, uint32 nsb)
{
	int			ups = 0,
				downs = 0;

	t->nsb = nsb;
	t->reverse = t->follows = false;
	for (uint32 i = 1; i < t->nprobe; i++)
	{
		ups += t->probe[i].best > t->probe[i - 1].best;
		downs += t->probe[i].best < t->probe[i - 1].best;
	}
	/* a column that grows (or shrinks) with the superblocks: probes in ascending sb order all go the same way */
	if (t->nprobe >= 4 && (ups == (int) t->nprobe - 1 || downs == (int) t->nprobe - 1))
	{
		t->follows = true;
		/* best values at the high end of the table: desc and growing, or asc and shrinking */
		t->reverse = (ups > 0) == t->key[0].desc;
	}
	t->left = nsb;
	/* a column that follows the table starts with one superblock, the one the best values are in, and grows to TOPN_WINDOW */
	t->size = t->follows ? 1 : TOPN_WINDOW;
}

bool
sift_topn_reverse(const SiftTopN *t)
{
	return t->reverse;
}

/* The next window of superblocks in the visit order, [*lo, *hi); *best: its superblock nearest to the best values */
bool
sift_topn_next_window(SiftTopN *t, uint32 *lo, uint32 *hi, uint32 *best)
{
	uint32		n = Min(t->size, t->left);

	if (n == 0)
		return false;
	*lo = t->reverse ? t->left - n : t->nsb - t->left;
	*hi = *lo + n;
	*best = t->reverse ? *hi - 1 : *lo;
	t->left -= n;
	t->size = Min(t->size * 2, TOPN_WINDOW);
	return true;
}

void
sift_topn_skip(SiftTopN *t)
{
	t->skipped++;
}

/*
 * The walk: for a first sort column that does not follow the table (the probes disagree) the superblocks are visited by the best value of
 * their zone, not in table order. Every superblock's zone is read first (sift_topn_walk_setup, headers only), the superblocks are sorted by the
 * best row they can hold, and sift_topn_walk_next hands them out until the zone of the next cannot beat the n-th best:
 * the zones after it are no better, so the walk ends there. The result does not depend on the order: a superblock is left out only when no
 * row in it can sort before the n-th (a bound equal to it is kept: ties go on to the other keys and the TID).
 */
bool
sift_topn_follows(const SiftTopN *t)
{
	return t->follows;
}

int
sift_topn_walk_keys(const SiftTopN *t)
{
	return t->wkeys;
}

/* The best rank key j can have in a unit with this zone: before every rank when the zone is not exact, a NULL first under NULLS FIRST, else the bound of the values (NULLs last when there are no values) */
static Rank
zone_best(const TopKey *k, const SiftSrcZone *zone)
{
	SiftSrcZone zk = key_zone(k, zone);
	const SiftSrcZone *z = &zk;

	if (z->inexact)
		return (Rank) {-1, 0};
	if (z->nnull == z->nvals || (z->nnull > 0 && k->nulls_first))
		return rank_of(k, true, 0);
	return rank_of(k, false, k->desc ? z->max : z->min);
}

static int
rank_cmp(Rank a, Rank b)
{
	return rank_after(a, b) ? 1 : rank_after(b, a) ? -1 : 0;
}

static int
walk_cmp(const void *pa, const void *pb, void *arg)
{
	const WalkSort *w = arg;
	uint32		a = *(const uint32 *) pa,
				b = *(const uint32 *) pb;

	for (int j = 0; j < w->wkeys; j++)
	{
		int			c = rank_cmp(w->bound[(Size) a * w->wkeys + j], w->bound[(Size) b * w->wkeys + j]);

		if (c != 0)
			return c;
	}
	return SIFT_CMP3(a, b);
}

/*
 * Can a row of a superblock with these bounds on the leading keys sort before the n-th best so far, or tie with it? A bound on each key is a
 * lower bound of its rows' keys, so a row's keys are not before the bounds in key order: the superblock is out when its bound is behind the
 * n-th on a key that the bounds before it tie on, and in when it is ahead of it (or a bound is none).
 */
static bool
bounds_viable(const SiftTopN *t, const Rank *bound)
{
	const TopEnt *w;

	if (t->all || t->n < t->limit)
		return true;
	w = &t->heap[0];
	for (int j = 0; j < t->wkeys; j++)
	{
		int			c = rank_cmp(bound[j], rank_of(&t->key[j], (w->nul >> j) & 1, w->k[j]));

		if (c != 0)
			return c < 0;
	}
	return t->nkey > 1;
}

/*
 * The walk of a parallel scan, where a participant has no table of the bounds (the leader's order is all it gets): the superblock it takes
 * next, by the zones z of its leading keys. The first key is tested against the bound of every participant, all of them against the n-th row
 * of this participant's heap, which is no better than the n-th of the result.
 */
bool
sift_topn_walk_zone_viable(const SiftTopN *t, const SiftSrcZone *z)
{
	Rank		bound[SIFT_TOPN_MAX_KEYS];

	if (!sift_topn_zone_viable(t, &z[0]))
		return false;
	for (int j = 0; j < t->wkeys; j++)
		bound[j] = zone_best(&t->key[j], &z[j]);
	return bounds_viable(t, bound);
}

static bool
walk_viable(const SiftTopN *t, uint32 sb)
{
	return bounds_viable(t, &t->wb->bound[(Size) sb * t->wkeys]);
}

static WalkBase *
walk_find(const WalkKey *key)
{
	for (int i = 0; i < WALK_CACHED; i++)
		if (walk_kept[i] != NULL && memcmp(&walk_kept[i]->key, key, sizeof(WalkKey)) == 0)
		{
			walk_kept[i]->used = ++walk_clock;
			return walk_kept[i];
		}
	return NULL;
}

/* Keeps wb, in a context of its own; the kept orders of the same file of an older version, and the least used when all places are taken, go */
static void
walk_keep(WalkBase *wb)
{
	int			slot = -1;

	for (int i = 0; i < WALK_CACHED; i++)
	{
		WalkBase   *o = walk_kept[i];

		if (o != NULL && o->key.st.index == wb->key.st.index && o->key.st.file == wb->key.st.file && o->key.st.version != wb->key.st.version)
		{
			MemoryContextDelete(o->cx);
			walk_kept[i] = NULL;
		}
		if (walk_kept[i] == NULL)
			slot = i;
	}
	if (slot < 0)
	{
		slot = 0;
		for (int i = 1; i < WALK_CACHED; i++)
			if (walk_kept[i]->used < walk_kept[slot]->used)
				slot = i;
		MemoryContextDelete(walk_kept[slot]->cx);
	}
	wb->used = ++walk_clock;
	walk_kept[slot] = wb;
}

static WalkBase *
walk_build(SiftTopN *t, MemoryContext cxt, const WalkKey *key, bool segs)
{
	uint32		nsb = t->nsb;
	int			wkeys = t->wkeys;
	MemoryContext cx = key != NULL ? AllocSetContextCreate(cxt, "sift cache topn walk", ALLOCSET_DEFAULT_SIZES) : AllocSetContextCreate(cxt, "sift topn walk", ALLOCSET_DEFAULT_SIZES);
	WalkBase   *wb = MemoryContextAllocZero(cx, sizeof(WalkBase));
	WalkSort	ws;

	wb->cx = cx;
	wb->nsb = nsb;
	if (key != NULL)
		wb->key = *key;
	wb->bound = MemoryContextAllocZero(cx, (Size) nsb * wkeys * sizeof(Rank));
	wb->units = MemoryContextAllocZero(cx, nsb * sizeof(uint32));
	wb->ord = MemoryContextAlloc(cx, nsb * sizeof(uint32));
	wb->pos = MemoryContextAlloc(cx, nsb * sizeof(uint32));
	wb->tail = MemoryContextAllocZero(cx, ((Size) nsb + 1) * sizeof(uint32));
	for (uint32 sb = 0; sb < nsb; sb++)
	{
		for (int j = 0; j < wkeys; j++)
		{
			SiftSrcZone z;
			int			leaf = t->key[j].leaf;

			wb->units[sb] = segs ? sift_colsrc_zone_range_segs(t->src, sb, sb + 1, leaf, &z) : sift_colsrc_zone_range(t->src, sb, sb + 1, leaf, &z);
			wb->bound[(Size) sb * wkeys + j] = zone_best(&t->key[j], &z);
		}
		wb->ord[sb] = sb;
	}
	ws = (WalkSort) {wb->bound, wkeys};
	qsort_arg(wb->ord, nsb, sizeof(uint32), walk_cmp, &ws);
	for (uint32 i = nsb; i > 0; i--)
	{
		wb->pos[wb->ord[i - 1]] = i - 1;
		wb->tail[i - 1] = wb->tail[i] + wb->units[wb->ord[i - 1]];
	}
	return wb;
}

/* The walk of a first sort column that does not follow the table (t->nsb superblocks): its bounds and order, from the backend's kept ones when the source allows */
void
sift_topn_walk_setup(SiftTopN *t)
{
	WalkKey		key;
	SiftSrcWalkStamp st;
	uint32		mem[WALK_MEM_SBS];
	int			nmem = 0;
	bool		keep = sift_colsrc_walk_stamp(t->src, &st, mem, WALK_MEM_SBS, &nmem);
	WalkBase   *wb = NULL;

	t->wpos = 0;
	t->hpos = t->nhead = 0;
	if (keep)
	{
		memset(&key, 0, sizeof(key));
		key.st = st;
		key.nsb = t->nsb;
		key.wkeys = t->wkeys;
		for (int j = 0; j < t->wkeys; j++)
		{
			key.k[j].leaf = t->key[j].leaf;
			key.k[j].vtype = t->key[j].vtype;
			key.k[j].xk = t->key[j].xk;
			key.k[j].xc = t->key[j].xc;
			key.k[j].desc = t->key[j].desc;
			key.k[j].nulls_first = t->key[j].nulls_first;
		}
		wb = walk_find(&key);
		if (wb == NULL)
		{
			/* kept as long as the backend: "sift cache" contexts are not leaks to scripts/leakcheck.sh */
			if (walk_cx == NULL)
				walk_cx = AllocSetContextCreate(TopMemoryContext, "sift cache topn walks", ALLOCSET_DEFAULT_SIZES);
			if ((Size) t->nsb * (t->wkeys * sizeof(Rank) + 4 * sizeof(uint32)) <= WALK_KEEP_BYTES)
			{
				wb = walk_build(t, walk_cx, &key, true);
				walk_keep(wb);
			}
			else
				keep = false;
		}
	}
	if (!keep)
	{
		/* the zones with the write buffers in them: no superblock is left out of the order */
		wb = walk_build(t, GetMemoryChunkContext(t), NULL, false);
		nmem = 0;
	}
	t->wb = wb;
	t->wkeep = keep;
	if (nmem > 0)
	{
		t->whead = MemoryContextAlloc(GetMemoryChunkContext(t), nmem * sizeof(uint32));
		memcpy(t->whead, mem, nmem * sizeof(uint32));
		t->nhead = nmem;
	}
}

bool
walk_in_head(const SiftTopN *t, uint32 sb)
{
	int			lo = 0,
				hi = t->nhead;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (t->whead[mid] == sb)
			return true;
		if (t->whead[mid] < sb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

/* The units of the superblocks the walk has not reached: those the head has visited are not among them */
static int64
walk_rest(const SiftTopN *t)
{
	const WalkBase *wb = t->wb;
	int64		n = wb->tail[t->wpos];

	for (int i = 0; i < t->nhead; i++)
		if (t->whead[i] < wb->nsb && wb->pos[t->whead[i]] >= t->wpos)
			n -= wb->units[t->whead[i]];
	return n;
}

bool
sift_topn_walk_next(SiftTopN *t, uint32 *sb)
{
	const WalkBase *wb = t->wb;

	if (t->hpos < t->nhead)
	{
		*sb = t->whead[t->hpos++];
		return true;
	}
	for (;;)
	{
		if (t->wpos >= wb->nsb)
			return false;
		*sb = wb->ord[t->wpos];
		if (t->nhead > 0 && walk_in_head(t, *sb))
		{
			t->wpos++;
			continue;
		}
		if (!walk_viable(t, *sb))
		{
			t->skipped += walk_rest(t);
			t->wpos = wb->nsb;
			return false;
		}
		t->wpos++;
		return true;
	}
}

bool
sift_topn_incomplete(const SiftTopN *t)
{
	return t->incomplete;
}

int
sift_topn_candidates(const SiftTopN *t)
{
	return t->want > 0 && !t->all ? t->limit : 0;
}

/* The next row of the result: the TID of the sort, or of the heap in order, and the partition it is in */
bool
next_tid(SiftTopN *t, ItemPointerData *tid, uint16 *part)
{
	bool		isnull;
	int64		tie;

	if (t->mem)
	{
		if (t->pos >= t->nmem)
			return false;
		tie = mem_row(t, t->pos++)[t->stride - 1];
		tie_tid(tie, tid, part);
		return true;
	}
	if (!t->all)
	{
		if (t->pos >= t->n)
			return false;
		*tid = t->res[t->pos].tid;
		*part = t->res[t->pos++].part;
		return true;
	}
	if (!tuplesort_gettupleslot(t->sort, true, false, t->sout, NULL))
		return false;
	tie = DatumGetInt64(slot_getattr(t->sout, t->nkey + 1, &isnull));
	tie_tid(tie, tid, part);
	return true;
}
