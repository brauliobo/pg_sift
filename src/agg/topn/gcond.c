/*
 * agg/topn/gcond.c - the conditions on the groups of a top-N and the group order
 * The boxes the group conditions give the keys, the lazy filter, and the read of the
 * rows in the order of the groups.
 */
#include "postgres.h"

#include <math.h>

#include "access/visibilitymap.h"
#include "miscadmin.h"

#include "port/pg_bitutils.h"
#include "lib/stringinfo.h"
#include "utils/memutils.h"

#include "agg/gord.h"
#include "agg/topn/int.h"

/* The range of the strings of a text key that a condition keeps (the ends that it bounds); the order is the collation's, then the bytes */
typedef struct SBox
{
	bool		has_lo;
	bool		has_hi;
	bool		lo_excl;
	bool		hi_excl;
	const char *lo;
	const char *hi;
	uint32		lolen;
	uint32		hilen;
} SBox;

/*
 * More keys than the first: the array gives the rows in the order of the first key only, so the scan takes the whole groups of rows that share a value of it
 * (the heap row says what the others hold, and whether it passes the condition) until the groups make the n rows, and puts each group in order. A scan that
 * reads more entries than GORD_SCAN_MIN, or 64 for each row wanted, before the n are in has a first key that many rows share: the units read it better.
 */
#define GORD_SCAN_MIN	4096

/*
 * A condition on a column other than the key is tested on the heap row of every entry, which may be most of the array for a condition that few rows pass. The walk gives up, for the
 * units, when the pass rate of the rows it has fetched says that the n will take more fetches than the units cost: a quarter of the heap's blocks, and the blocks the units would vet
 * (those not all-visible that hold a row that passes), and never less than four for each row wanted (a filter that passes a quarter or more does what a b-tree's scan does). The
 * walk is judged with two passes more than it saw, so that a few rows give no verdict: one that passes none is given up after about twice the budget over the rows wanted.
 */
#define GORD_BUDGET_DIV	4
#define GORD_TRY_MIN	32

static GBox
gcond_box(const SiftTopGPred *p)
{
	GBox		r = {false, false, false, 0, 0};

	if (p->kind == STG_STR)
		return r;
	/* a value is not NULL: the entries of the values, whatever the key (those of the NULLs come after them) */
	if (p->kind == STG_ISNULL || p->kind == STG_BOOL)
		return r;
	if (p->kind == STG_NOTNULL)
	{
		r.has_lo = p->lead;
		r.lo = PG_INT64_MIN;
		return r;
	}
	if (p->kind == STG_CMP)
	{
		if (!p->lead)
			return r;
		switch (p->op)
		{
			case SCMP_EQ:
				r.has_lo = r.has_hi = true;
				r.lo = r.hi = p->val;
				break;
			case SCMP_LT:
				r.empty = p->val == PG_INT64_MIN;
				r.has_hi = true;
				r.hi = p->val - !r.empty;
				break;
			case SCMP_LE:
				r.has_hi = true;
				r.hi = p->val;
				break;
			case SCMP_GT:
				r.empty = p->val == PG_INT64_MAX;
				r.has_lo = true;
				r.lo = p->val + !r.empty;
				break;
			case SCMP_GE:
				r.has_lo = true;
				r.lo = p->val;
				break;
			default:
				break;
		}
		return r;
	}
	r = gcond_box(p->child[0]);
	for (int i = 1; i < p->nchild; i++)
	{
		GBox		c = gcond_box(p->child[i]);

		if (p->kind == STG_AND)
		{
			r.empty |= c.empty;
			if (c.has_lo && (!r.has_lo || c.lo > r.lo))
				r.has_lo = true, r.lo = c.lo;
			if (c.has_hi && (!r.has_hi || c.hi < r.hi))
				r.has_hi = true, r.hi = c.hi;
		}
		else if (c.empty)
			continue;
		else if (r.empty)
			r = c;
		else
		{
			r.has_lo = r.has_lo && c.has_lo;
			r.lo = Min(r.lo, c.lo);
			r.has_hi = r.has_hi && c.has_hi;
			r.hi = Max(r.hi, c.hi);
		}
	}
	return r;
}

static SBox
gcond_sbox(const SiftTopGPred *p, Oid collid)
{
	SBox		r = {0};

	if (p->kind == STG_STR)
	{
		switch (p->sop)
		{
			case SSTR_EQ:
				r.has_lo = r.has_hi = true;
				r.lo = r.hi = p->str;
				r.lolen = r.hilen = p->slen;
				break;
			case SSTR_LT:
			case SSTR_LE:
				r.has_hi = true;
				r.hi = p->str;
				r.hilen = p->slen;
				r.hi_excl = p->sop == SSTR_LT;
				break;
			case SSTR_GT:
			case SSTR_GE:
				r.has_lo = true;
				r.lo = p->str;
				r.lolen = p->slen;
				r.lo_excl = p->sop == SSTR_GT;
				break;
			case SSTR_PREFIX:
				r.has_lo = true;
				r.lo = p->str;
				r.lolen = p->slen;
				r.hi = sift_cgord_prefix_end(p->str, p->slen, &r.hilen);
				r.has_hi = r.hi != NULL;
				r.hi_excl = true;
				break;
		}
		return r;
	}
	if (p->kind == STG_CMP || p->kind == STG_NOTNULL || p->kind == STG_ISNULL || p->kind == STG_BOOL)
		return r;
	r = gcond_sbox(p->child[0], collid);
	for (int i = 1; i < p->nchild; i++)
	{
		SBox		c = gcond_sbox(p->child[i], collid);
		int			d;

		if (p->kind == STG_AND)
		{
			if (c.has_lo)
			{
				d = r.has_lo ? sift_cgord_str_order(c.lo, c.lolen, r.lo, r.lolen, collid) : 1;
				if (d > 0 || (d == 0 && c.lo_excl))
					r.has_lo = true, r.lo = c.lo, r.lolen = c.lolen, r.lo_excl = c.lo_excl;
			}
			if (c.has_hi)
			{
				d = r.has_hi ? sift_cgord_str_order(c.hi, c.hilen, r.hi, r.hilen, collid) : -1;
				if (d < 0 || (d == 0 && c.hi_excl))
					r.has_hi = true, r.hi = c.hi, r.hilen = c.hilen, r.hi_excl = c.hi_excl;
			}
		}
		else
		{
			if (r.has_lo && c.has_lo)
			{
				d = sift_cgord_str_order(c.lo, c.lolen, r.lo, r.lolen, collid);
				if (d < 0 || (d == 0 && r.lo_excl))
					r.lo = c.lo, r.lolen = c.lolen, r.lo_excl = c.lo_excl;
			}
			else
				r.has_lo = false;
			if (r.has_hi && c.has_hi)
			{
				d = sift_cgord_str_order(c.hi, c.hilen, r.hi, r.hilen, collid);
				if (d > 0 || (d == 0 && r.hi_excl))
					r.hi = c.hi, r.hilen = c.hilen, r.hi_excl = c.hi_excl;
			}
			else
				r.has_hi = false;
		}
	}
	return r;
}

/* is every condition on a text string one that compares under the collation the array ranks by */
static bool
gcond_coll_ok(const SiftTopGPred *p, Oid collid)
{
	if (p->kind == STG_STR)
		return p->collid == collid;
	for (int i = 0; i < p->nchild; i++)
		if (!gcond_coll_ok(p->child[i], collid))
			return false;
	return true;
}

/* does the heap string pass: the collation, then the bytes, as the b-tree of the column compares */
static bool
gcond_str_test(const SiftTopGPred *p, Datum d)
{
	struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
	const char *s = VARDATA_ANY(v);
	uint32		len = VARSIZE_ANY_EXHDR(v);
	int			c;

	if (p->sop == SSTR_PREFIX)
		return len >= p->slen && memcmp(s, p->str, p->slen) == 0;
	c = sift_cgord_str_order(s, len, p->str, p->slen, p->collid);
	switch (p->sop)
	{
		case SSTR_EQ:
			return c == 0;
		case SSTR_LT:
			return c < 0;
		case SSTR_LE:
			return c <= 0;
		case SSTR_GT:
			return c > 0;
		default:
			return c >= 0;
	}
}

/* The conditions of the scan on the first key as the range of it that they keep, which a sorted run is read within (only integer and time columns make a condition: sift_topn_leaf_attno) */
void
sift_topn_key_range(SiftTopN *t, const SiftTopGPred *cond)
{
	if (cond != NULL)
		t->kbox = gcond_box(cond);
}

/* does the heap row pass: a NULL column passes no comparison */
bool
gcond_test(const SiftTopGPred *p, TupleTableSlot *slot)
{
	if (p->kind == STG_BOOL)
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, p->attno, &isnull);

		return p->tab[isnull ? 2 : DatumGetBool(d)];
	}
	if (p->kind == STG_NOTNULL || p->kind == STG_ISNULL)
	{
		bool		isnull;

		(void) slot_getattr(slot, p->attno, &isnull);
		return p->kind == STG_NOTNULL ? !isnull : isnull;
	}
	if (p->kind == STG_CMP || p->kind == STG_STR)
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, p->attno, &isnull);

		if (isnull)
			return false;
		return p->kind == STG_STR ? gcond_str_test(p, d) : gcond_cmp(p->op, heap_key(p->typid, d), p->val);
	}
	for (int i = 0; i < p->nchild; i++)
		if (gcond_test(p->child[i], slot) == (p->kind == STG_OR))
			return p->kind == STG_OR;
	return p->kind == STG_AND;
}

/*
 * Does an entry of the global order pass the condition, from its key alone: 1 or 0, or -1 when a column other than the key decides. The entries of a group
 * block are all of the group gl that the top of the conjunction pins.
 */
int
gcond_entry(const SiftTopGPred *p, int gl, bool top, int64 key, bool isnull)
{
	int			r = p->kind == STG_AND ? 1 : 0;

	if (p->kind == STG_NOTNULL)
		return p->lead ? !isnull : -1;
	if (p->kind == STG_ISNULL)
		return p->lead ? isnull : -1;
	if (p->kind == STG_STR || p->kind == STG_BOOL)
		return -1;				/* the string is the heap row's: the entry holds a rank */
	if (p->kind == STG_CMP)
	{
		if (p->lead)
			return !isnull && gcond_cmp(p->op, key, p->val);
		return top && p->op == SCMP_EQ && p->leaf == gl ? 1 : -1;
	}
	for (int i = 0; i < p->nchild; i++)
	{
		int			c = gcond_entry(p->child[i], gl, top && p->kind == STG_AND, key, isnull);

		if (c == (p->kind == STG_OR))
			return c;
		if (c < 0)
			r = -1;
	}
	return r;
}

/* the keys after the first, then the address */
static int
grow_cmp(const void *a, const void *b, void *arg)
{
	const SiftTopN *t = arg;
	const GRow *x = a,
			   *y = b;

	for (int j = 1; j < t->nkey; j++)
	{
		const TopKey *k = &t->key[j];

		if (x->isnull[j] != y->isnull[j])
			return x->isnull[j] == k->nulls_first ? -1 : 1;
		if (!x->isnull[j] && x->key[j] != y->key[j])
			return (x->key[j] < y->key[j]) != k->desc ? -1 : 1;
	}
	return ItemPointerCompare(unconstify(ItemPointerData *, &x->tid), unconstify(ItemPointerData *, &y->tid));
}

/* the heap row's string is the entry's: a row an UPDATE or a vacuumed address replaced holds another */
static bool
text_is(Datum d, const char *str, uint32 len)
{
	struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

	return VARSIZE_ANY_EXHDR(v) == len && memcmp(VARDATA_ANY(v), str, len) == 0;
}

/* What is known of the entry (sift_gord_state), the condition counting: SIFT_GORD_EXACT when it passes the condition by its key alone and is live on an all-visible page (the heap does not confirm it). The condition is asked first: when another column decides it, the entry is never exact and the dead list of its unit is not read */
int
gord_entry_state(SiftTopN *t, ItemPointerData *tid, int64 key, bool isnull)
{
	if (t->gcond != NULL && gcond_entry(t->gcond, sift_gord_group(t->go), true, key, isnull) != 1)
		return SIFT_GORD_CHECK;
	return sift_gord_state(t->go, tid);
}

/*
 * The entries of one key of the array, in the order of the array. A group wholly before the OFFSET (even if every entry is live) is thrown away by the Limit above and the tie order
 * within it decides nothing: its entries that are exact (live and visible, the condition decided by the key) are counted and not fetched, the others are confirmed by the heap and
 * counted if it finds them. The group that holds the first row to keep is fetched whole and sorted by the keys after the first.
 */
typedef struct GGroup
{
	ItemPointerData *tid;
	bool	   *exact;			/* per entry: live and visible, and passes the condition by its key alone */
	bool	   *dead;			/* per entry: its segment lost the row and no other source repeats it: nothing stands for it, the heap is not asked */
	int			n;
	int			room;
	StringInfoData str;			/* the string of the group, for a text key (the key of the array is the rank in a segment) */
} GGroup;

static bool
gord_giveup(const SiftTopN *t)
{
	double		need = (double) t->limit - t->gbulk - t->gqn;

	if (t->gtried < GORD_TRY_MIN || need <= 0)
		return false;
	return t->gtried + need * t->gtried / (t->gpass + 2.0) > t->gbudget + t->gdirty * (1.0 - exp(-(double) t->gpass / t->gtried * t->grpp));
}

/* false: the walk gave up */
static bool
gord_group(SiftTopN *t, GGroup *g, int64 gkey, bool gnull)
{
	TopFetch   *f = &t->pf[0];
	int			gstart = t->gqn;
	bool		thrown = t->gbulk + t->gqn + g->n <= t->skip;

	for (int i = 0; i < g->n; i++)
	{
		GRow	   *r;
		ItemPointerData tid = g->tid[i],
					entry = tid;	/* the fetch leaves the address of the version it found in tid: the entry is the root of its chain, which a second fetch starts from */

		if (t->gqn == t->gqroom)
		{
			t->gqroom *= 2;
			t->gq = repalloc(t->gq, t->gqroom * sizeof(GRow));
		}
		r = &t->gq[t->gqn];
		if (g->dead[i])
			continue;
		memset(r, 0, sizeof(GRow));
		r->tid = entry;
		if (thrown)
		{
			r->exact = true;
			r->isnull[0] = gnull;
			r->key[0] = gkey;
		}
		if (!thrown || !g->exact[i])
		{
			bool		call_again = false,
						all_dead,
						hnull;
			Datum		d;

			if (t->gbudget > 0 && gord_giveup(t))
				return false;
			t->gtried++;
			if (ItemPointerGetBlockNumber(&tid) >= f->nblocks || !table_index_fetch_tuple(f->fetch, &tid, t->snap, f->slot, &call_again, &all_dead))
				continue;
			t->fetched++;
			d = slot_getattr(f->slot, t->gatt[0], &hnull);
			if (hnull != gnull || (!hnull && (t->gtext ? !text_is(d, g->str.data, g->str.len) : gkey_of(t, 0, d) != gkey)))
				continue;
			if (t->gcond != NULL && !gcond_test(t->gcond, f->slot))
				continue;
			t->gpass++;
			for (int j = 0; !thrown && j < t->nkey; j++)
			{
				d = slot_getattr(f->slot, t->gatt[j], &r->isnull[j]);
				r->key[j] = r->isnull[j] ? 0 : gkey_of(t, j, d);
			}
		}
		t->gqn++;
	}
	if (!thrown && t->gqn - gstart > 1)
		qsort_arg(t->gq + gstart, t->gqn - gstart, sizeof(GRow), grow_cmp, t);
	g->n = 0;
	return true;
}

static bool
gord_prep(SiftTopN *t)
{
	int64		cap = Max(GORD_SCAN_MIN, 64 * (int64) t->limit),
				scanned = 0,
				gkey = 0,
				key;
	bool		isnull,
				gnull = false,
				closed = false;
	GGroup		g = {.tid = palloc(64 * sizeof(ItemPointerData)), .exact = palloc(64 * sizeof(bool)), .dead = palloc(64 * sizeof(bool)), .room = 64};
	ItemPointerData tid;
	const char *str = NULL;
	uint32		slen = 0;
	int			st;

	initStringInfo(&g.str);

	t->gbulk = gord_skip(t);
	t->bulk = t->gbulk;
	t->gqroom = 64;
	t->gq = MemoryContextAlloc(GetMemoryChunkContext(t), t->gqroom * sizeof(GRow));
	while (sift_gord_next(t->go, &key, &isnull, &tid))
	{
		if (g.n > 0 && sift_gord_repeated(t->go))
			g.dead[g.n - 1] = false;
		if (++scanned > cap)
			return false;
		if (t->gtext && !isnull)
			str = sift_gord_str(t->go, &slen);
		if (g.n > 0 && (isnull != gnull || (t->gtext ? !isnull && (slen != g.str.len || memcmp(str, g.str.data, slen) != 0) : key != gkey)))
		{
			if (!gord_group(t, &g, gkey, gnull))
				return false;
			if (t->gbulk + t->gqn >= t->limit)
			{
				closed = true;
				break;
			}
		}
		if (g.n == 0 && t->gtext && !isnull)
		{
			resetStringInfo(&g.str);
			appendBinaryStringInfo(&g.str, str, slen);
		}
		gkey = key;
		gnull = isnull;
		if (g.n == g.room)
		{
			g.room *= 2;
			g.tid = repalloc(g.tid, g.room * sizeof(ItemPointerData));
			g.exact = repalloc(g.exact, g.room * sizeof(bool));
			g.dead = repalloc(g.dead, g.room * sizeof(bool));
		}
		st = gord_entry_state(t, &tid, key, isnull);
		g.exact[g.n] = st == SIFT_GORD_EXACT;
		g.dead[g.n] = st == SIFT_GORD_DEAD;
		g.tid[g.n++] = tid;
	}
	/* the end of the array closes the last group */
	if (g.n > 0 && sift_gord_repeated(t->go))
		g.dead[g.n - 1] = false;
	if (!closed && !gord_group(t, &g, gkey, gnull))
		return false;
	pfree(g.tid);
	pfree(g.exact);
	pfree(g.dead);
	pfree(g.str.data);
	return closed || !sift_gord_short(t->go) || t->gbulk + t->gqn >= t->limit;
}

/*
 * The scan of a segment that keeps the ends of its order only (column format minor 40) can run out of entries that are not its own: the rows of an end that the heap
 * finds dead, an end that is a few rows. That must be known before the first row goes out, so the scan reads the n it will give once, with the confirmation of the heap
 * (the pages stay in the buffers for the scan), and gives the rows to the units when the ends do not hold them.
 */
static bool
gord_probe(SiftTopN *t)
{
	TopFetch   *f = &t->pf[0];
	uint32		n = 0;
	bool		cut;
	int64		key;
	bool		isnull;
	ItemPointerData tid;

	while (n < t->limit && sift_gord_next(t->go, &key, &isnull, &tid))
	{
		bool		call_again = false,
					all_dead,
					hnull;
		Datum		d;

		if (n < t->skip && gord_entry_state(t, &tid, key, isnull) == SIFT_GORD_EXACT)
		{
			n++;
			continue;
		}
		if (ItemPointerGetBlockNumber(&tid) >= f->nblocks || !table_index_fetch_tuple(f->fetch, &tid, t->snap, f->slot, &call_again, &all_dead))
			continue;
		d = slot_getattr(f->slot, t->gatt[0], &hnull);
		if (hnull != isnull || (!hnull && gkey_of(t, 0, d) != key))
			continue;
		if (t->gcond != NULL && !gcond_test(t->gcond, f->slot))
			continue;
		n++;
	}
	cut = sift_gord_short(t->go);
	sift_gord_rewind(t->go);
	return n >= t->limit || !cut;
}

/* an OR of equalities of the group leaf gl with constants (its IN list): a global order grouped by gl reads the entries of each of the groups */
static bool
gcond_group_in(const SiftTopGPred *p, int gl)
{
	if (p->kind != STG_OR || gl < 0)
		return false;
	for (int i = 0; i < p->nchild; i++)
		if (p->child[i]->kind != STG_CMP || p->child[i]->op != SCMP_EQ || p->child[i]->lead || p->child[i]->leaf != gl)
			return false;
	return true;
}

/* the conditions on a column other than the first key that the array read does not narrow: those outside an equality of the group leaf gl at the top of the conjunction, with the first key not pinned */
static int
gcond_loose(const SiftTopGPred *p, int gl, bool top)
{
	int			n = 0;

	if (p->kind == STG_NOTNULL || p->kind == STG_ISNULL || p->kind == STG_BOOL)
		return p->lead || p->pinned ? 0 : 1;
	if (p->kind == STG_STR)
		return p->lead ? 0 : 1;
	if (p->kind == STG_CMP)
		return p->lead || p->pinned || (top && p->op == SCMP_EQ && p->leaf == gl) ? 0 : 1;
	if (top && gcond_group_in(p, gl))
		return 0;
	for (int i = 0; i < p->nchild; i++)
		n += gcond_loose(p->child[i], gl, top && p->kind == STG_AND);
	return n;
}

/* The condition is nothing but bounds on the first key (a conjunction of =, <, <=, >, >= and IS NOT NULL on it): every value entry between the bounds the array was opened with passes it */
static bool
gcond_bounds_only(const SiftTopGPred *p)
{
	if (p->kind == STG_ISNULL || p->kind == STG_BOOL)
		return false;
	if (p->kind == STG_NOTNULL)
		return p->lead;
	if (p->kind == STG_CMP)
		return p->lead && (p->op == SCMP_EQ || p->op == SCMP_LT || p->op == SCMP_LE || p->op == SCMP_GT || p->op == SCMP_GE);
	if (p->kind != STG_AND)
		return false;
	for (int i = 0; i < p->nchild; i++)
		if (!gcond_bounds_only(p->child[i]))
			return false;
	return true;
}

/* the leaves of a condition, into t->rleaf: false when there are more than it holds */
static bool
resid_leaves(SiftTopN *t, const SiftSrcLayout *lay, const SiftTopGPred *p)
{
	int			j = 0;

	if (p->kind == STG_BOOL)
		return false;
	if (p->kind != STG_AND && p->kind != STG_OR)
	{
		while (j < t->nresid && t->rleaf[j] != p->leaf)
			j++;
		if (j == t->nresid)
		{
			if (j == TOP_RESID)
				return false;
			t->rleaf[j] = p->leaf;
			t->rwide[j] = lay->leaf[p->leaf].vtype == SCV_I64;
			t->nresid++;
		}
		return true;
	}
	for (int i = 0; i < p->nchild; i++)
		if (!resid_leaves(t, lay, p->child[i]))
			return false;
	return true;
}

/*
 * The conditions of the scan are all of them on integer and time columns (cond), and the range of the first key that they keep is bounded: the units read through their
 * sorted run do not run the filters of the scan, they test cond on the rows the walk takes from the range (sift_topn_lazy_unit). A condition on another column that
 * does not narrow the walk (not pinned by an equality of the first key) is left to the filters: the walk would read the column for rows that fail it.
 */
void
sift_topn_lazy(SiftTopN *t, const SiftSrcLayout *lay, const SiftTopGPred *cond)
{
	t->resid = NULL;
	t->nresid = 0;
	if (cond == NULL || !(t->kbox.has_lo || t->kbox.has_hi) || gcond_loose(cond, -1, true) > 0 || !resid_leaves(t, lay, cond))
		return;
	t->resid = cond;
}

/* Does the unit go to its sorted run without its filters (the paths that test every row of the mask come before the run in sift_topn_unit) */
bool
sift_topn_lazy_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit)
{
	uint32		nvals,
				nnull;
	uint64		id;

	return t->resid != NULL && !t->all && t->nexpr == 0 && run_ready(t, src, unit, &nvals, &nnull, &id);
}

/* the equalities of the top of the conjunction on columns other than the first key: a global order grouped by one of them reads the entries of that group */
static int
gcond_pins(const SiftTopGPred *p, SiftGBound *b)
{
	if (p->kind == STG_CMP)
	{
		if (!p->lead && p->op == SCMP_EQ && b->npin < SIFT_GORD_PIN_MAX)
		{
			b->pinleaf[b->npin] = p->leaf;
			b->pinval[b->npin++] = p->val;
		}
		return b->npin;
	}
	if (p->kind == STG_AND)
		for (int i = 0; i < p->nchild; i++)
			if (p->child[i]->kind == STG_CMP || p->child[i]->kind == STG_OR)
				gcond_pins(p->child[i], b);
	/* the equalities of one leaf in an OR (an IN list) are its pins, as many as fit */
	if (p->kind == STG_OR && p->nchild <= SIFT_GORD_PIN_MAX - b->npin)
	{
		int			leaf = p->child[0]->leaf;

		for (int i = 0; i < p->nchild; i++)
			if (p->child[i]->kind != STG_CMP || p->child[i]->op != SCMP_EQ || p->child[i]->lead || p->child[i]->leaf != leaf)
				return b->npin;
		for (int i = 0; i < p->nchild; i++)
		{
			b->pinleaf[b->npin] = leaf;
			b->pinval[b->npin++] = p->child[i]->val;
		}
	}
	return b->npin;
}

/*
 * The scan reads the global order of the key (one entry for every row ever indexed, in the order of the key) in place of the units: the rows of the heap come in
 * the order of the key and the n-th one ends the scan. An entry is for the heap to confirm, as a b-tree's is: the row of the address may be dead, or another one
 * where the first was vacuumed, which holds another key.
 */
bool
sift_topn_gorder(SiftTopN *t, SiftSrcSource *src, const SiftTopGPred *cond)
{
	SiftGBound	b = {0};
	bool		bounds = false,
				loose;
	int			gl;

	/* past the cap of the heap only a walk that throws the OFFSET away unread gives the rows */
	if (!t->gok || t->nogord || (t->all && (t->skip == 0 || t->nkey > 1 || t->gwant == 0)))
		return false;
	/* the array costs the same as the heads of the runs for a few rows on a table of a few units, and does not grow with the units (a key-cut index has thousands) */
	if (cond != NULL && t->gtext)
	{
		SBox		box;

		if (!gcond_coll_ok(cond, t->key[0].collid))
			return false;
		box = gcond_sbox(cond, t->key[0].collid);
		b.has_lo = box.has_lo, b.has_hi = box.has_hi, b.slo = box.lo, b.slolen = box.lolen, b.slo_excl = box.lo_excl;
		b.shi = box.hi, b.shilen = box.hilen, b.shi_excl = box.hi_excl;
		bounds = box.has_lo || box.has_hi;
	}
	else if (cond != NULL)
	{
		GBox		box = gcond_box(cond);

		if (box.empty)
			b.has_lo = b.has_hi = true, b.lo = 1, b.hi = 0;
		else
			b.has_lo = box.has_lo, b.has_hi = box.has_hi, b.lo = box.lo, b.hi = box.hi;
		bounds = box.empty || box.has_lo || box.has_hi;
		(void) gcond_pins(cond, &b);
	}
	t->go = sift_gord_open(src, t->key[0].leaf, t->all ? t->gwant : t->limit, t->key[0].desc, t->key[0].nulls_first, t->gtext ? (uint32) t->key[0].collid : 0, cond != NULL ? &b : NULL, GetMemoryChunkContext(t));
	if (t->go == NULL)
		return false;
	gl = sift_gord_group(t->go);
	/*
	 * A condition that leaves no bound on the key and no group, or that another column decides, is a filter of the whole array: the walk fetches the heap row of every entry
	 * to test it, and gives the rows to the units when the fetches it needs are more than they cost (gord_giveup).
	 */
	loose = cond != NULL && ((!bounds && gl < 0) || gcond_loose(cond, gl, true) > 0);
	t->gcond = t->nkey == 1 && !t->gtext && cond != NULL && gcond_bounds_only(cond) ? NULL : cond;
	t->limit = t->all ? t->gwant : t->limit;
	/* the walk queues every row of the LIMIT and the OFFSET before the first goes out, outside work_mem: a longer page reads the units */
	if (loose && (t->all || (uint64) t->limit * sizeof(GRow) > (uint64) work_mem * 1024))
	{
		t->go = NULL;
		return false;
	}
	t->gprep = t->nkey > 1 || loose;
	t->gtried = t->gpass = 0;
	if (loose)
	{
		Relation	rel = t->pf[0].fetch->rel;
		BlockNumber allvis;

		t->gbudget = Max(4 * (int64) t->limit, t->pf[0].nblocks / GORD_BUDGET_DIV);
		visibilitymap_count(rel, &allvis, NULL);
		t->gdirty = Max((int64) t->pf[0].nblocks - allvis, 0);
		t->grpp = rel->rd_rel->reltuples > 0 && t->pf[0].nblocks > 0 ? rel->rd_rel->reltuples / t->pf[0].nblocks : 100;
		if (sift_topn_gbudget > 0)
			t->gbudget = sift_topn_gbudget, t->gdirty = 0;
	}
	else
		t->gbudget = 0;
	if (t->gprep && !gord_prep(t))
		t->go = NULL;
	else if (!t->gprep && sift_gord_ends(t->go) && !gord_probe(t))
		t->go = NULL;
	return t->go != NULL;
}

bool
sift_topn_gordered(const SiftTopN *t)
{
	return t->go != NULL;
}

/* a row the Limit above throws away: NULLs, the heap row is not read */
bool
skipped_row(SiftTopN *t, TupleTableSlot *slot)
{
	/* the rows the Limit throws away come first, and it does not touch them: the slot of the last one is the NULLs of the next */
	if (TTS_EMPTY(slot))
	{
		memset(slot->tts_isnull, true, t->nout * sizeof(bool));
		ExecStoreVirtualTuple(slot);
	}
	return true;
}
