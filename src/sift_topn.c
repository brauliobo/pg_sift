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

/* Superblocks per window of the visit order: the zones are looked at again, and the match set is asked for, once per window */
#define TOPN_WINDOW		8

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

typedef struct TopZone
{
	int64		lo;
	int64		hi;
	uint32		nvals;
	uint32		nnull;
	uint8		state;			/* 0: no unit, 1: described, 2: no bound */
} TopZone;

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
	uint32	   *rows;
	ItemPointerData *tids;
	TopEnt	   *res;			/* after sift_topn_finish: the heap in order */
	/* the visit order */
	TopZone    *zone;
	uint32		nsb;
	bool		reverse;
	uint32		win;			/* windows left after the current one, counting in visit order */
	uint32		wlo,
				wend,
				p;
	bool		started;
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

/* < 0: a sorts before b by the keys */
static int
key_cmp(const SiftTopN *t, const TopEnt *a, const TopEnt *b)
{
	for (int j = 0; j < t->nkey; j++)
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

/* the key of row i; floats map to integers of the same order (NaN last), as a numeric NaN does */
static inline bool
row_key(const TopKey *k, const SiftSrcVec *v, int i, int64 *out)
{
	if (v->isnull && v->isnull[i])
	{
		if (v->ovf && ((v->ovf[i >> 6] >> (i & 63)) & 1))
		{
			*out = PG_INT64_MAX;
			return true;
		}
		return false;
	}
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

void
sift_topn_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *const *keys, const uint64 *rmask, int nrow)
{
	const TopEnt *worst = t->n == t->limit ? &t->heap[0] : NULL;
	int			nw = (nrow + 63) / 64;

	t->ln = 0;
	for (int w = 0; w < nw; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			TopEnt		e = {.row = i, .tie = i};

			for (int j = 0; j < t->nkey; j++)
				if (!row_key(&t->key[j], keys[j], i, &e.k[j]))
					e.nul |= 1 << j;
			if (worst && key_cmp(t, &e, worst) > 0)
				continue;
			heap_push(t, t->lh, &t->ln, &e);
		}
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

/* ---------- the visit order ---------- */

bool
sift_topn_wants_zones(const SiftTopN *t)
{
	return t->key[0].vtype == SCV_I32 || t->key[0].vtype == SCV_I64;
}

void
sift_topn_zones_begin(SiftTopN *t, uint32 nsb)
{
	t->nsb = nsb;
	t->zone = MemoryContextAllocZero(GetMemoryChunkContext(t), Max(nsb, 1) * sizeof(TopZone));
}

void
sift_topn_zone(SiftTopN *t, uint32 sb, const SiftSrcZone *z)
{
	TopZone    *tz;

	if (sb >= t->nsb)
		return;
	tz = &t->zone[sb];
	if (z->inexact)
		tz->state = 2;
	if (tz->state == 2)
		return;
	if (z->nnull < z->nvals)
	{
		tz->lo = tz->nvals > tz->nnull ? Min(tz->lo, z->min) : z->min;
		tz->hi = tz->nvals > tz->nnull ? Max(tz->hi, z->max) : z->max;
	}
	tz->nvals += z->nvals;
	tz->nnull += z->nnull;
	tz->state = 1;
}

/*
 * Can a row of the superblock sort before the n-th best so far (or tie with it)? Its first sort column is within the zone: nulls sort
 * as the query says, the rest between lo and hi. Rows tied on the first key may win on the others, so a bound equal to the worst's is kept.
 */
static bool
sb_viable(const SiftTopN *t, uint32 sb)
{
	const TopZone *z = t->zone ? &t->zone[sb] : NULL;
	const TopKey *k = &t->key[0];
	const TopEnt *w;
	bool		wnull;
	int64		best;

	if (t->n < t->limit || z == NULL || z->state != 1)
		return true;
	w = &t->heap[0];
	wnull = w->nul & 1;
	if (z->nnull > 0 && (k->nulls_first || wnull))
		return true;
	if (z->nnull == z->nvals)
		return wnull;
	if (wnull)
		return !k->nulls_first;
	best = k->desc ? z->hi : z->lo;
	return k->desc ? best >= w->k[0] : best <= w->k[0];
}

void
sift_topn_start(SiftTopN *t, uint32 nsb)
{
	uint32		ups = 0,
				downs = 0;
	int64		prev = 0;
	bool		have = false;

	t->started = true;
	t->nsb = nsb;
	t->reverse = false;
	for (uint32 sb = 0; t->zone && sb < nsb; sb++)
	{
		const TopZone *z = &t->zone[sb];
		int64		b;

		if (z->state != 1 || z->nnull == z->nvals)
			continue;
		b = t->key[0].desc ? z->hi : z->lo;
		if (have)
			ups += b > prev, downs += b < prev;
		prev = b;
		have = true;
	}
	/* a column that grows (or shrinks) with the superblocks is visited from the end its best values are at */
	if (ups + downs >= 4)
	{
		double		up = (double) ups / (ups + downs);

		t->reverse = t->key[0].desc ? up >= 0.75 : up <= 0.25;
	}
	t->win = (nsb + TOPN_WINDOW - 1) / TOPN_WINDOW;
	t->wend = t->p = 0;
}

static bool
next_window(SiftTopN *t)
{
	uint32		w;

	if (t->win == 0)
		return false;
	t->win--;
	w = t->reverse ? t->win : (t->nsb + TOPN_WINDOW - 1) / TOPN_WINDOW - 1 - t->win;
	t->wlo = w * TOPN_WINDOW;
	t->p = t->wlo;
	t->wend = Min(t->wlo + TOPN_WINDOW, t->nsb);
	return true;
}

bool
sift_topn_next_range(SiftTopN *t, uint32 *lo, uint32 *hi)
{
	for (;;)
	{
		if (t->p >= t->wend && !next_window(t))
			return false;
		while (t->p < t->wend && !sb_viable(t, t->p))
		{
			t->skipped++;
			t->p++;
		}
		if (t->p >= t->wend)
			continue;
		*lo = t->p;
		while (t->p < t->wend && sb_viable(t, t->p))
			t->p++;
		*hi = t->p;
		return true;
	}
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
