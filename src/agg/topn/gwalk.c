/*
 * agg/topn/gwalk.c - the walk of a top-N over the global order: the groups of ties, the entries read ahead for a
 * condition on another column, the queue of the rows before the first goes out
 */
#include "postgres.h"

#include "access/visibilitymap.h"
#include "miscadmin.h"

#include "port/pg_bitutils.h"
#include "lib/stringinfo.h"
#include "utils/memutils.h"

#include "agg/gord.h"
#include "agg/topn/int.h"

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
	if (t->gcond != NULL && gcond_entry(t->gcond, sift_gord_group(t->go), true, key, isnull, NULL) != 1)
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
	int		   *seg;			/* per entry: the source it came from (sift_gord_src) */
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

/* A group of at least this many entries has the keys after the first of its exact entries read from the units, not fetched */
#define GORD_VALUES_MIN	64

/* The keys after the first are integers the units hold (sift_gord_values) */
static bool
gord_int_keys(const SiftTopN *t)
{
	for (int j = 1; j < t->nkey; j++)
		if (t->gscale[j] != 0 || (t->gtyp[j] != INT2OID && t->gtyp[j] != INT4OID && t->gtyp[j] != INT8OID && t->gtyp[j] != DATEOID && t->gtyp[j] != TIMESTAMPOID && t->gtyp[j] != TIMESTAMPTZOID))
			return false;
	return t->nkey > 1;
}

/* The keys after the first of the exact entries of g: val[j * n + i] with st[j * n + i] 1 (value) or 2 (NULL); 0 for an entry whose keys the heap has to give */
static void
gord_values(SiftTopN *t, const GGroup *g, int64 *val, uint8 *st)
{
	int		   *seg = palloc(g->n * sizeof(int));
	SiftKey	   *tid = palloc(g->n * sizeof(SiftKey));
	uint32	   *at = palloc(g->n * sizeof(uint32)),
				m = 0;
	int64	   *v = palloc((g->n + SIFT_COL_PAD / 8) * sizeof(int64));
	uint8	   *s = palloc(g->n);

	memset(st, 0, (size_t) t->nkey * g->n);
	for (int i = 0; i < g->n; i++)
		if (g->exact[i] && !g->dead[i])
		{
			seg[m] = g->seg[i];
			tid[m] = sift_key_from_tid(&g->tid[i]);
			at[m++] = i;
		}
	for (int j = 1; j < t->nkey; j++)
	{
		sift_gord_values(t->go, t->key[j].leaf, seg, tid, m, v, s);
		for (uint32 k = 0; k < m; k++)
		{
			val[(size_t) j * g->n + at[k]] = v[k];
			st[(size_t) j * g->n + at[k]] = s[k];
		}
	}
	pfree(seg);
	pfree(tid);
	pfree(at);
	pfree(v);
	pfree(s);
}

/* The keys of entry i of a group of n from gord_values, when it has them all */
static bool
gord_keys_read(const SiftTopN *t, GRow *r, const int64 *val, const uint8 *st, int n, int i, int64 gkey, bool gnull)
{
	for (int j = 1; j < t->nkey; j++)
		if (st[(size_t) j * n + i] == 0)
			return false;
	r->isnull[0] = gnull;
	r->key[0] = gkey;
	for (int j = 1; j < t->nkey; j++)
	{
		r->isnull[j] = st[(size_t) j * n + i] == 2;
		r->key[j] = r->isnull[j] ? 0 : val[(size_t) j * n + i];
	}
	return true;
}

/* false: the walk gave up */
static bool
gord_group(SiftTopN *t, GGroup *g, int64 gkey, bool gnull)
{
	TopFetch   *f = &t->pf[t->gpart];
	int			gstart = t->gqn;
	bool		thrown = t->gbulk + t->gqn + g->n <= t->skip;
	int64	   *val = NULL;
	uint8	   *vst = NULL;

	if (!thrown && g->n >= GORD_VALUES_MIN && gord_int_keys(t))
	{
		val = palloc((size_t) t->nkey * g->n * sizeof(int64));
		vst = palloc((size_t) t->nkey * g->n);
		gord_values(t, g, val, vst);
	}

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
		/* the entries the units decided are tried too, so that the pass rate the budget extrapolates is the walk's, not that of the dirty pages the heap was asked about */
		if (t->gbudget > 0 && gord_giveup(t))
			return false;
		if (g->dead[i])
		{
			t->gtried += t->gbudget > 0;
			continue;
		}
		memset(r, 0, sizeof(GRow));
		r->tid = entry;
		if (thrown)
		{
			r->exact = true;
			r->isnull[0] = gnull;
			r->key[0] = gkey;
		}
		if (!(vst != NULL && gord_keys_read(t, r, val, vst, g->n, i, gkey, gnull)) && (!thrown || !g->exact[i]))
		{
			bool		call_again = false,
						all_dead,
						hnull;
			Datum		d;

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
		else if (t->gbudget > 0)
			t->gtried++, t->gpass++;
		t->gqn++;
	}
	if (!thrown && t->gqn - gstart > 1)
		qsort_arg(t->gq + gstart, t->gqn - gstart, sizeof(GRow), grow_cmp, t);
	g->n = 0;
	if (vst != NULL)
	{
		pfree(val);
		pfree(vst);
	}
	return true;
}

/* One entry of the walk with what is known of it: the state (sift_gord_state, the condition counting), the source and whether the merge dropped a repeat of the entry before it */
typedef struct GEnt
{
	int64		key;
	bool		isnull;
	bool		repeat;
	int			st;
	int			seg;
	ItemPointerData tid;
} GEnt;

/*
 * The entries the walk reads ahead, so that the columns of the condition are read from the units for many of them at once: a call costs about 20,000 instructions a unit it reads, which
 * the heap fetches of 2,000 entries cost, so the walk is read ahead by what it still wants (at most GORD_FEED_MAX) and only from GORD_FEED entries on
 */
#define GORD_FEED		2048
#define GORD_FEED_MAX	16384

typedef struct GFeed
{
	GEnt	   *buf;
	int			max;
	int			n;
	int			pos;
	bool		end;
	bool		endrep;			/* the merge dropped a repeat of the last entry on its way to the end */
	int			nleaf;			/* the columns other than the key that the condition tests, when the units can give them */
	int			leaf[GORD_COND_LEAVES];
} GFeed;

/* The integer and time columns other than the key that the condition tests; false when there are none or more than GORD_COND_LEAVES */
static bool
gord_cond_leaves(const SiftTopGPred *p, GFeed *f)
{
	if (p->kind == STG_CMP || p->kind == STG_NOTNULL || p->kind == STG_ISNULL)
	{
		for (int i = 0; !p->lead && p->leaf >= 0 && i <= f->nleaf; i++)
			if (i == f->nleaf)
			{
				if (f->nleaf == GORD_COND_LEAVES)
					return false;
				f->leaf[f->nleaf++] = p->leaf;
				break;
			}
			else if (f->leaf[i] == p->leaf)
				break;
		return true;
	}
	for (int i = 0; i < p->nchild; i++)
		if (!gord_cond_leaves(p->child[i], f))
			return false;
	return true;
}

/*
 * The next entries of the walk, up to f->max. An entry that is live on an all-visible page, whose condition another column decides, has that column read from the units: it passes (exact, not
 * fetched), or fails (dropped as a dead entry is), or the units could not say and the heap decides.
 */
static void
gord_feed(SiftTopN *t, GFeed *f)
{
	int			gl = sift_gord_group(t->go),
				m = 0;
	int		   *seg = NULL;
	int		   *at = NULL;
	SiftKey	   *tid = NULL;
	int64	   *val = NULL;
	uint8	   *st = NULL;

	int			want = Min(f->max, Max(64, t->limit - t->gbulk - t->gqn));

	f->n = f->pos = 0;
	while (f->n < want && !f->end)
	{
		GEnt	   *e = &f->buf[f->n];
		int			d;

		if (!sift_gord_next(t->go, &e->key, &e->isnull, &e->tid))
		{
			f->end = true;
			f->endrep = sift_gord_repeated(t->go);
			break;
		}
		e->repeat = sift_gord_repeated(t->go);
		e->seg = sift_gord_src(t->go);
		d = t->gcond == NULL ? 1 : gcond_entry(t->gcond, gl, true, e->key, e->isnull, NULL);
		e->st = d == 1 || f->nleaf == 0 ? gord_entry_state(t, &e->tid, e->key, e->isnull) : sift_gord_state(t->go, &e->tid);
		if (d != 1 && f->nleaf > 0)
		{
			/* undecided by the key: asked of the units when the entry is exact, else the heap decides */
			if (e->st == SIFT_GORD_EXACT && d == -1)
			{
				if (seg == NULL)
				{
					seg = palloc(f->max * sizeof(int));
					at = palloc(f->max * sizeof(int));
					tid = palloc(f->max * sizeof(SiftKey));
				}
				seg[m] = e->seg;
				tid[m] = sift_key_from_tid(&e->tid);
				at[m++] = f->n;
			}
			e->st = SIFT_GORD_CHECK;
		}
		f->n++;
	}
	if (m == 0)
		return;
	val = palloc((size_t) f->nleaf * m * sizeof(int64) + SIFT_COL_PAD);
	st = palloc((size_t) f->nleaf * m);
	for (int c = 0; c < f->nleaf; c++)
		sift_gord_values(t->go, f->leaf[c], seg, tid, m, val + (size_t) c * m, st + (size_t) c * m);
	for (int k = 0; k < m; k++)
	{
		GEnt	   *e = &f->buf[at[k]];
		GEntVals	ev = {.n = f->nleaf};
		int			d;

		for (int c = 0; c < f->nleaf; c++)
		{
			ev.leaf[c] = f->leaf[c];
			ev.val[c] = val[(size_t) c * m + k];
			ev.st[c] = st[(size_t) c * m + k];
		}
		d = gcond_entry(t->gcond, gl, true, e->key, e->isnull, &ev);
		e->st = d == 1 ? SIFT_GORD_EXACT : d == 0 ? SIFT_GORD_DEAD : SIFT_GORD_CHECK;
	}
	pfree(seg);
	pfree(at);
	pfree(tid);
	pfree(val);
	pfree(st);
}

bool
gord_prep(SiftTopN *t)
{
	int64		cap = Max(GORD_SCAN_MIN, 64 * (int64) t->limit),
				scanned = 0,
				gkey = 0,
				key;
	bool		isnull,
				gnull = false,
				closed = false;
	GGroup		g = {.tid = palloc(64 * sizeof(ItemPointerData)), .exact = palloc(64 * sizeof(bool)), .dead = palloc(64 * sizeof(bool)), .seg = palloc(64 * sizeof(int)), .room = 64};
	GFeed		f = {.max = 1};
	GEnt		e;
	const char *str = NULL;
	uint32		slen = 0;

	initStringInfo(&g.str);
	/* the columns of a condition are read from the units for the entries of a batch; a text key's string is the cursor's, kept for the one entry */
	if (t->gcond != NULL && !t->gtext && t->limit - t->gbulk >= GORD_FEED && gord_cond_leaves(t->gcond, &f) && f.nleaf > 0)
		f.max = Min(GORD_FEED_MAX, t->limit - t->gbulk);
	else
		f.nleaf = 0;
	f.buf = palloc(f.max * sizeof(GEnt));

	t->gbulk = gord_skip(t);
	t->bulk = t->gbulk;
	t->gqroom = 64;
	t->gq = MemoryContextAlloc(GetMemoryChunkContext(t), t->gqroom * sizeof(GRow));
	for (;;)
	{
		if (f.pos == f.n)
		{
			gord_feed(t, &f);
			if (f.n == 0)
				break;
		}
		e = f.buf[f.pos++];
		key = e.key;
		isnull = e.isnull;
		if (g.n > 0 && e.repeat)
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
			g.seg = repalloc(g.seg, g.room * sizeof(int));
		}
		g.exact[g.n] = e.st == SIFT_GORD_EXACT;
		g.dead[g.n] = e.st == SIFT_GORD_DEAD;
		g.seg[g.n] = e.seg;
		g.tid[g.n++] = e.tid;
	}
	/* the end of the array closes the last group */
	if (g.n > 0 && f.endrep)
		g.dead[g.n - 1] = false;
	if (!closed && !gord_group(t, &g, gkey, gnull))
		return false;
	pfree(f.buf);
	pfree(g.tid);
	pfree(g.exact);
	pfree(g.dead);
	pfree(g.seg);
	pfree(g.str.data);
	return closed || !sift_gord_short(t->go) || t->gbulk + t->gqn >= t->limit;
}

/*
 * The scan of a segment that keeps the ends of its order only (column format minor 40) can run out of entries that are not its own: the rows of an end that the heap
 * finds dead, an end that is a few rows. That must be known before the first row goes out, so the scan reads the n it will give once, with the confirmation of the heap
 * (the pages stay in the buffers for the scan), and gives the rows to the units when the ends do not hold them.
 */
bool
gord_probe(SiftTopN *t)
{
	TopFetch   *f = &t->pf[t->gpart];
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
