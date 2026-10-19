/*
 * sift_topn.c - ORDER BY <columns> LIMIT n from the column units (sift_topn.h).
 */
#include "postgres.h"

#include <math.h>

#include "access/relation.h"
#include "access/tableam.h"
#include "executor/executor.h"
#include "catalog/pg_type.h"
#include "port/pg_bitutils.h"
#include "utils/rel.h"

#include "sift_topn.h"

/* Superblocks per window of the visit order at most: the zones are looked at again, and the match set is asked for, once per window */
#define TOPN_WINDOW		8
/* Superblocks whose zones tell whether the sort column follows the table */
#define TOPN_PROBES		6

/* ---------- planner ---------- */

bool
sift_topn_key_ok(const SiftSrcLeaf *lf)
{
	if (lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->lossy)
		return false;
	switch (lf->typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return lf->vtype == SCV_I32 || lf->vtype == SCV_I64;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case NUMERICOID:
			return lf->vtype == SCV_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return lf->vtype == SCV_F64;
		default:
			return false;
	}
}

Node *
sift_topn_spec(int limit, int nkey, const int *leaf, const bool *desc, const bool *nulls_first, int nout, const int *attno)
{
	List	   *l = list_make2_int(limit, nkey);

	for (int j = 0; j < nkey; j++)
		l = lappend_int(lappend_int(l, leaf[j]), (desc[j] ? 1 : 0) | (nulls_first[j] ? 2 : 0));
	l = lappend_int(l, nout);
	for (int c = 0; c < nout; c++)
		l = lappend_int(l, attno[c]);
	return (Node *) l;
}

int
sift_topn_limit(Node *spec)
{
	return linitial_int((List *) spec);
}

/* ---------- executor ---------- */

typedef struct TopKey
{
	int			leaf;
	bool		desc;
	bool		nulls_first;
	SiftSrcVecType vtype;
} TopKey;

typedef struct TopEnt
{
	int64		k[SIFT_TOPN_MAX_KEYS];
	uint64		tie;			/* the TID's key in the result heap, the row in a unit's */
	uint32		row;
	uint8		nul;			/* bit j: key j is NULL */
	ItemPointerData tid;
} TopEnt;

struct SiftTopN
{
	int			limit;
	int			nkey;
	TopKey		key[SIFT_TOPN_MAX_KEYS];
	int			nout;
	int		   *attno;
	TopEnt	   *heap;			/* the best rows so far, the worst at heap[0] */
	int			n;
	TopEnt	   *lh;				/* the best rows of the unit */
	int			ln;
	uint32	   *crow;			/* the rows of the unit that pass, their first sort key and whether it is NULL */
	int64	   *ck;
	uint8	   *cnul;
	int			ccap;
	TopEnt	   *ce;				/* the rows whose first key is not worse than the n-th of the unit's and of the heap */
	uint32	   *cand;			/* the rows of ce */
	uint32	   *rows;
	ItemPointerData *tids;
	TopEnt	   *res;			/* after sift_topn_finish: the heap in order */
	/* the visit order */
	uint32		nsb;
	bool		reverse;			/* the superblocks are visited from the last */
	bool		follows;			/* the first sort column follows the superblocks (as far as the probes say): the visit starts at its best end */
	uint32		left;				/* superblocks not yet in a window */
	uint32		size;				/* superblocks of the next window */
	uint32		nprobe;
	struct
	{
		uint32		sb;
		int64		best;
	}			probe[TOPN_PROBES];
	Relation	rel;
	Snapshot	snap;
	TupleTableSlot *fslot;
	int64		skipped;
	int64		fetched;
};

SiftTopN *
sift_topn_open(Node *spec, const SiftSrcLayout *lay, Relation heap, Snapshot snapshot, MemoryContext cxt)
{
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftTopN   *t = palloc0(sizeof(SiftTopN));
	List	   *l = (List *) spec;
	int			i = 0;

	t->limit = list_nth_int(l, i++);
	t->nkey = list_nth_int(l, i++);
	for (int j = 0; j < t->nkey; j++)
	{
		int			leaf = list_nth_int(l, i++),
					fl = list_nth_int(l, i++);

		t->key[j] = (TopKey) {leaf, fl & 1, (fl & 2) != 0, lay->leaf[leaf].vtype};
	}
	t->nout = list_nth_int(l, i++);
	t->attno = palloc(Max(t->nout, 1) * sizeof(int));
	for (int c = 0; c < t->nout; c++)
		t->attno[c] = list_nth_int(l, i++);
	t->heap = palloc(t->limit * sizeof(TopEnt));
	t->lh = palloc(t->limit * sizeof(TopEnt));
	t->rows = palloc(t->limit * sizeof(uint32));
	t->tids = palloc(t->limit * sizeof(ItemPointerData));
	t->rel = heap;
	t->snap = snapshot;
	t->fslot = table_slot_create(heap, NULL);
	MemoryContextSwitchTo(old);
	return t;
}

void
sift_topn_close(SiftTopN *t)
{
	if (t->fslot)
		ExecDropSingleTupleTableSlot(t->fslot);
}

int
sift_topn_nkey(const SiftTopN *t)
{
	return t->nkey;
}

int
sift_topn_leaf(const SiftTopN *t, int j)
{
	return t->key[j].leaf;
}

int64
sift_topn_skipped(const SiftTopN *t)
{
	return t->skipped;
}

int64
sift_topn_fetched(const SiftTopN *t)
{
	return t->fetched;
}

/* < 0: a sorts before b by the first nk keys */
static int
key_cmp_n(const SiftTopN *t, const TopEnt *a, const TopEnt *b, int nk)
{
	for (int j = 0; j < nk; j++)
	{
		bool		an = (a->nul >> j) & 1,
					bn = (b->nul >> j) & 1;

		if (an != bn)
			return an == t->key[j].nulls_first ? -1 : 1;
		if (an || a->k[j] == b->k[j])
			continue;
		return (a->k[j] < b->k[j]) != t->key[j].desc ? -1 : 1;
	}
	return 0;
}

static int
key_cmp(const SiftTopN *t, const TopEnt *a, const TopEnt *b)
{
	return key_cmp_n(t, a, b, t->nkey);
}

static int
ent_cmp(const SiftTopN *t, const TopEnt *a, const TopEnt *b)
{
	int			c = key_cmp(t, a, b);

	return c != 0 ? c : (a->tie < b->tie ? -1 : a->tie > b->tie);
}

static int
ent_cmp_arg(const void *a, const void *b, void *arg)
{
	return ent_cmp(arg, a, b);
}

/* the heap keeps its worst entry at h[0]: a parent sorts after its children */
static void
heap_down(const SiftTopN *t, TopEnt *h, int n, int i)
{
	TopEnt		e = h[i];

	for (;;)
	{
		int			c = 2 * i + 1;

		if (c >= n)
			break;
		if (c + 1 < n && ent_cmp(t, &h[c + 1], &h[c]) > 0)
			c++;
		if (ent_cmp(t, &h[c], &e) <= 0)
			break;
		h[i] = h[c];
		i = c;
	}
	h[i] = e;
}

static void
heap_push(const SiftTopN *t, TopEnt *h, int *n, const TopEnt *e)
{
	int			i;

	if (*n == t->limit)
	{
		if (ent_cmp(t, e, &h[0]) >= 0)
			return;
		h[0] = *e;
		heap_down(t, h, *n, 0);
		return;
	}
	i = (*n)++;
	while (i > 0 && ent_cmp(t, e, &h[(i - 1) / 2]) > 0)
	{
		h[i] = h[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	h[i] = *e;
}

/* the key of row i; floats map to integers of the same order (NaN last); a numeric NaN is PG_INT64_MAX in the vector (SiftSrcVec.ovf) */
static inline bool
row_key(const TopKey *k, const SiftSrcVec *v, int i, int64 *out)
{
	if (v->isnull && v->isnull[i])
		return false;
	switch (k->vtype)
	{
		case SCV_I32:
			*out = ((const int32 *) v->data)[i];
			break;
		case SCV_I64:
			*out = ((const int64 *) v->data)[i];
			break;
		default:
			{
				double		d = ((const double *) v->data)[i];
				int64		b;

				if (isnan(d))
					*out = PG_INT64_MAX;
				else
				{
					d = d == 0 ? 0.0 : d;
					memcpy(&b, &d, sizeof(b));
					*out = b < 0 ? b ^ PG_INT64_MAX : b;
				}
			}
	}
	return true;
}

static int
row_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

static uint64
tid_key(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid);
}

static void *
grow(SiftTopN *t, void *p, Size sz)
{
	return p ? repalloc(p, sz) : MemoryContextAlloc(GetMemoryChunkContext(t), sz);
}

static void
grow_unit_buffers(SiftTopN *t, int nrow)
{
	if (t->ccap >= nrow)
		return;
	t->ccap = Max(nrow, 2 * t->ccap);
	t->crow = grow(t, t->crow, t->ccap * sizeof(uint32));
	t->ck = grow(t, t->ck, t->ccap * sizeof(int64));
	t->cnul = grow(t, t->cnul, t->ccap);
	t->cand = grow(t, t->cand, t->ccap * sizeof(uint32));
	t->ce = grow(t, t->ce, t->ccap * sizeof(TopEnt));
}

/* The rows cand[0..nc) (ascending) with their first key in ce: the other keys by position, the best n by all of them to the heap with their TIDs */
static void
finish_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int nc)
{
	t->ln = 0;
	for (int j = 1; j < t->nkey; j++)
	{
		SiftSrcVec	v;

		sift_colsrc_read_rows(src, unit, t->key[j].leaf, t->cand, nc, &v);
		for (int x = 0; x < nc; x++)
			if (!row_key(&t->key[j], &v, x, &t->ce[x].k[j]))
				t->ce[x].nul |= 1 << j;
	}
	for (int x = 0; x < nc; x++)
		heap_push(t, t->lh, &t->ln, &t->ce[x]);
	if (t->ln == 0)
		return;
	for (int i = 0; i < t->ln; i++)
		t->rows[i] = t->lh[i].row;
	qsort(t->rows, t->ln, sizeof(uint32), row_cmp);
	sift_colsrc_tids(src, unit, t->rows, t->ln, t->tids);
	for (int i = 0; i < t->ln; i++)
	{
		TopEnt	   *e = &t->lh[i];
		uint32	   *at = bsearch(&e->row, t->rows, t->ln, sizeof(uint32), row_cmp);

		e->tid = t->tids[at - t->rows];
		e->tie = tid_key(&e->tid);
		heap_push(t, t->heap, &t->n, e);
	}
}

/* The first key as a position in the sort order: NULLs in front of or behind the values, the values by the direction */
typedef struct Rank
{
	int			cls;
	uint64		ord;
} Rank;

static inline Rank
rank_of(const TopKey *k, bool isnull, int64 v)
{
	Rank		r = {isnull ? (k->nulls_first ? 0 : 2) : 1, 0};

	if (!isnull)
		r.ord = k->desc ? ~((uint64) v ^ UINT64CONST(0x8000000000000000)) : (uint64) v ^ UINT64CONST(0x8000000000000000);
	return r;
}

static inline bool
rank_after(Rank a, Rank b)
{
	return a.cls != b.cls ? a.cls > b.cls : a.ord > b.ord;
}

static inline Rank
ent_rank(const SiftTopN *t, const TopEnt *e)
{
	return rank_of(&t->key[0], e->nul & 1, e->k[0]);
}

/*
 * The rows of the unit are cut in two steps, so that the leaves of the other sort keys are read for a handful of rows and not for all of
 * them. First the first key of every row that passes: a row strictly worse on it than the n-th of the heap, or than the n-th of the unit
 * so far, cannot be in the result whatever its other keys are. Then the others, for the rows that remain (the n best and those tied with
 * the n-th).
 */
void
sift_topn_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow)
{
	const TopKey *k0 = &t->key[0];
	Rank		bar = {0, 0},
				mine;
	bool		barred = false;
	int			nw = (nrow + 63) / 64,
				m = 0,
				nc = 0;

	grow_unit_buffers(t, nrow);
	t->ln = 0;
	if (t->n == t->limit)
	{
		bar = ent_rank(t, &t->heap[0]);
		barred = true;
	}
	for (int w = 0; w < nw; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			int64		v = 0;
			bool		isnull = !row_key(k0, key0, i, &v);
			TopEnt		e;

			if (barred && rank_after(rank_of(k0, isnull, v), bar))
				continue;
			memset(&e, 0, sizeof(e));
			e.k[0] = v;
			e.nul = isnull;
			e.row = e.tie = i;
			t->crow[m] = i;
			t->ck[m] = v;
			t->cnul[m++] = isnull;
			heap_push(t, t->lh, &t->ln, &e);	/* the other keys are 0 in all of them: the order is the first key's, then the row */
			if (t->ln == t->limit)
			{
				mine = ent_rank(t, &t->lh[0]);
				if (!barred || rank_after(bar, mine))
				{
					bar = mine;
					barred = true;
				}
			}
		}
	for (int q = 0; q < m; q++)
	{
		TopEnt	   *e = &t->ce[nc];

		if (barred && rank_after(rank_of(k0, t->cnul[q], t->ck[q]), bar))
			continue;
		memset(e, 0, sizeof(TopEnt));
		e->k[0] = t->ck[q];
		e->nul = t->cnul[q];
		e->row = t->cand[nc] = t->crow[q];
		e->tie = e->row;
		nc++;
	}
	if (nc > 0)
		finish_unit(t, src, unit, nc);
}

/* ---------- the visit order ---------- */

bool
sift_topn_wants_zones(const SiftTopN *t)
{
	return t->key[0].vtype == SCV_I32 || t->key[0].vtype == SCV_I64;
}

/*
 * Can a row of a unit with this zone of the first sort column sort before the n-th best so far (or tie with it)? Nulls sort as the query
 * says, the rest between min and max. Rows tied on the first key may win on the others, so a bound equal to the n-th's is kept. The zone
 * counts the dead rows too: it only ever says "no" for a unit that has nothing better.
 */
bool
sift_topn_zone_viable(const SiftTopN *t, const SiftSrcZone *z)
{
	const TopKey *k = &t->key[0];
	const TopEnt *w;
	bool		wnull;
	int64		best;

	if (t->n < t->limit || z->inexact)
		return true;
	w = &t->heap[0];
	wnull = w->nul & 1;
	if (z->nnull > 0 && (k->nulls_first || wnull))
		return true;
	if (z->nnull == z->nvals)
		return wnull;
	if (wnull)
		return !k->nulls_first;
	best = k->desc ? z->max : z->min;
	return k->desc ? best >= w->k[0] : best <= w->k[0];
}

/* The zone of the unit of superblock sb, probed before the scan; it may be passed in any order */
void
sift_topn_probe(SiftTopN *t, uint32 sb, const SiftSrcZone *z)
{
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

/* ---------- the result ---------- */

int
sift_topn_finish(SiftTopN *t)
{
	t->res = t->heap;
	qsort_arg(t->res, t->n, sizeof(TopEnt), ent_cmp_arg, t);
	return t->n;
}

bool
sift_topn_fetch(SiftTopN *t, int i, TupleTableSlot *slot)
{
	ItemPointerData tid = t->res[i].tid;

	if (!table_tuple_fetch_row_version(t->rel, &tid, t->snap, t->fslot))
		elog(ERROR, "sift: top-N row (%u,%u) is not in the heap", ItemPointerGetBlockNumber(&tid), ItemPointerGetOffsetNumber(&tid));
	t->fetched++;
	ExecClearTuple(slot);
	for (int c = 0; c < t->nout; c++)
		slot->tts_values[c] = slot_getattr(t->fslot, t->attno[c], &slot->tts_isnull[c]);
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);
	return true;
}
