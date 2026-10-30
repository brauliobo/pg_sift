/*
 * agg/lookup/first.c - the first row of a key in a lookup table
 * The order that picks the first row of each key, the keep of agreeing rows, the count of the
 * first rows of a table and the load and peek of a first map.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"

#include "agg/lookup/int.h"

/* the child runs of the first-row lookups of a statement that read the same table, over its arms (a worker counts its own) */
typedef struct FirstCount
{
	EState	   *estate;
	char	   *key;
	uint32		n;
	MemoryContextCallback cb;
} FirstCount;

static List *first_counts = NIL;

/* does the row a come before b in the child's ORDER BY? */
static bool
first_before(const SiftAggFirst *f, const FirstOrd *o, uint32 a, uint32 b)
{
	for (int i = 0; i < f->nord; i++)
	{
		size_t		x = (size_t) a * f->nord + i,
					y = (size_t) b * f->nord + i;

		if (o->isnull[x] != o->isnull[y])
			return o->isnull[x] == f->ordnullsfirst[i];
		if (!o->isnull[x] && o->val[x] != o->val[y])
			return (o->val[x] < o->val[y]) != f->orddesc[i];
	}
	return false;
}

/* the order columns of the row just added to m */
void
first_order_row(const MapTab *m, const SiftAggFirst *f, FirstOrd *o, TupleTableSlot *slot)
{
	size_t		at = (size_t) (m->nrow - 1) * f->nord;

	if (o->cx == NULL)
		o->cx = AllocSetContextCreate(CurrentMemoryContext, "sift first-row order", ALLOCSET_DEFAULT_SIZES);
	if (m->nrow > o->cap)
	{
		o->cap = Max(o->cap * 2, 1024);
		o->val = o->val ? repalloc(o->val, o->cap * f->nord * sizeof(int64)) : MemoryContextAlloc(o->cx, o->cap * f->nord * sizeof(int64));
		o->isnull = o->isnull ? repalloc(o->isnull, o->cap * f->nord) : MemoryContextAlloc(o->cx, o->cap * f->nord);
	}
	for (int i = 0; i < f->nord; i++)
	{
		Datum		d = slot_getattr(slot, f->ordatt[i], &o->isnull[at + i]);

		o->val[at + i] = o->isnull[at + i] ? 0 : sift_aggspec_operand_int(f->ordtype[i], d);
	}
}

/*
 * A child `ORDER BY .. LIMIT 1` returns the first row of a key by its order; a unique index holds the key and the order columns, so there is no
 * tie. The other rows of the key are out of the chains (a row with its first key id 0 is in none, map_index_rows).
 */
void
map_keep_first(MapTab *m, const SiftAggFirst *f, const FirstOrd *o)
{
	uint32		nslot = 64;
	uint32	   *at;

	while (nslot < 2 * m->nrow)
		nslot *= 2;
	at = palloc0(nslot * sizeof(uint32));
	for (uint32 r = 0; r < m->nrow; r++)
	{
		uint32	   *kid = m->kid + (size_t) r * m->nkey;
		uint32		h;
		bool		anynull = false;

		for (int k = 0; k < m->nkey; k++)
			anynull |= kid[k] == 0;
		if (anynull)
			continue;
		for (h = map_hash_ids(m->nkey, kid) & (nslot - 1); at[h] != 0; h = (h + 1) & (nslot - 1))
			if (memcmp(m->kid + (size_t) (at[h] - 1) * m->nkey, kid, m->nkey * sizeof(uint32)) == 0)
				break;
		if (at[h] == 0)
			at[h] = r + 1;
		else if (first_before(f, o, r, at[h] - 1))
		{
			m->kid[(size_t) (at[h] - 1) * m->nkey] = 0;
			at[h] = r + 1;
		}
		else
			kid[0] = 0;
	}
	pfree(at);
}

/* do rows a and b hold the same value in every column (a NULL is the NULL's equal)? The values of a column kept once per distinct value are the same when their ids are */
static bool
map_rows_agree(const MapTab *m, uint32 a, uint32 b)
{
	for (int c = 0; c < m->ncol; c++)
	{
		const MapCol *mc = &m->mcol[c];

		if (mc->byval)
		{
			bool		na = mc->isnull != NULL && mc->isnull[a],
						nb = mc->isnull != NULL && mc->isnull[b];

			if (na != nb || (!na && mc->val[a] != mc->val[b]))
				return false;
		}
		else if (mc->id[a] != mc->id[b])
			return false;
	}
	return true;
}

/*
 * A child `LIMIT 1` of a table that may hold a key more than once returns one of its rows, whichever its plan meets first. Where the rows of a key
 * hold the same columns the query reads, that is no choice: one of them is in the chain. The others (and none of a key whose rows differ) stay,
 * and a key with more than one row in the chain is the one the child must run for (map_first_row).
 */
void
map_keep_agreeing(MapTab *m)
{
	uint32		nslot = 64;
	uint32	   *rep,
			   *same;
	bool	   *differ;

	while (nslot < 2 * m->nrow)
		nslot *= 2;
	rep = palloc0(nslot * sizeof(uint32));
	differ = palloc0(nslot);
	same = palloc(Max(m->nrow, 1) * sizeof(uint32));
	for (uint32 r = 0; r < m->nrow; r++)
	{
		uint32	   *kid = m->kid + (size_t) r * m->nkey;
		uint32		h;
		bool		anynull = false;

		same[r] = 0;
		for (int k = 0; k < m->nkey; k++)
			anynull |= kid[k] == 0;
		if (anynull)
			continue;
		for (h = map_hash_ids(m->nkey, kid) & (nslot - 1); rep[h] != 0; h = (h + 1) & (nslot - 1))
			if (memcmp(m->kid + (size_t) (rep[h] - 1) * m->nkey, kid, m->nkey * sizeof(uint32)) == 0)
				break;
		same[r] = h + 1;
		if (rep[h] == 0)
			rep[h] = r + 1;
		else if (!map_rows_agree(m, rep[h] - 1, r))
			differ[h] = true;
	}
	/* a row after the first of a key that agrees is out of the chains (a row with its first key id 0 is in none, map_index_rows) */
	for (uint32 r = 0; r < m->nrow; r++)
		if (same[r] != 0 && rep[same[r] - 1] != r + 1 && !differ[same[r] - 1])
			m->kid[(size_t) r * m->nkey] = 0;
	pfree(rep);
	pfree(differ);
	pfree(same);
}

/* the table of a first-row lookup that may repeat a key (SiftAggFirst.hybrid): loaded as the lookup table its columns and keys make (agg/first.c) */
MapTab *
map_load_first(AggExec *ex, int ci)
{
	int			ji = ex->spec->combo[ci].join;
	SiftAggJoin keep = ex->spec->join[ji];
	MapTab	   *m;

	if (ex->pfmaps && ex->pfmaps[ci])
		return map_share_attach(ex->pfmaps[ci], ex->fjoin[ji], ex->cxt);
	ex->spec->join[ji] = *ex->fjoin[ji];
	PG_TRY();
	{
		m = map_load(ex, ci);
	}
	PG_FINALLY();
	{
		ex->spec->join[ji] = keep;
	}
	PG_END_TRY();
	return m;
}

static void
first_count_forget(void *arg)
{
	first_counts = list_delete_ptr(first_counts, arg);
}

static uint32 *
first_count(AggExec *ex, const char *key)
{
	EState	   *estate = ex->css.ss.ps.state;
	FirstCount *fc;
	MemoryContext old;

	if (estate == NULL)
		return MemoryContextAllocZero(ex->cxt, sizeof(uint32));
	foreach_ptr(FirstCount, e, first_counts)
		if (e->estate == estate && strcmp(e->key, key) == 0)
			return &e->n;
	fc = MemoryContextAllocZero(estate->es_query_cxt, sizeof(FirstCount));
	fc->estate = estate;
	fc->key = MemoryContextStrdup(estate->es_query_cxt, key);
	fc->cb.func = first_count_forget;
	fc->cb.arg = fc;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, &fc->cb);
	old = MemoryContextSwitchTo(TopMemoryContext);
	first_counts = lappend(first_counts, fc);
	MemoryContextSwitchTo(old);
	return &fc->n;
}

/* the table of a hybrid first-row lookup if a cache holds it (or the leader shared it): NULL otherwise */
MapTab *
map_first_peek(AggExec *ex, int ci)
{
	int			ji = ex->spec->combo[ci].join;
	SiftAggJoin keep = ex->spec->join[ji];
	MapTab	   *m;

	if (ex->pfmaps && ex->pfmaps[ci])
	{
		ex->cs[ci].fruns = MemoryContextAllocZero(ex->cxt, sizeof(uint32));
		return map_share_attach(ex->pfmaps[ci], ex->fjoin[ji], ex->cxt);
	}
	ex->spec->join[ji] = *ex->fjoin[ji];
	ex->first_peek = true;
	PG_TRY();
	{
		ex->cs[ci].fruns = first_count(ex, shared_map_key(ex, ci, true));
		m = map_load_table(ex, ci, ex->cxt);
	}
	PG_FINALLY();
	{
		ex->first_peek = false;
		ex->spec->join[ji] = keep;
	}
	PG_END_TRY();
	return m;
}

/*
 * The row a first-row lookup gives for the child's parameters, when the table can say: 1 (appended to the join's rows), 0 (the child has none), or
 * -1 (the key has rows that differ on the columns read: the child runs, as without the table).
 */
int
map_first_row(AggExec *ex, int ci, EState *estate)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];
	const MapTab *fm = cs->fmap;
	MapTab	   *m = cs->map;
	uint32		kid[SIFT_AGG_MAX_JKEYS];
	uint32	   *rows;
	int			nm;

	/* a table that is no cache's costs a pass over its rows: the child runs for the keys until that is what it costs (a few eighths of the rows), then the table is read */
	if (fm == NULL)
	{
		if (++*cs->fruns < cs->fthresh)
			return -1;
		fm = cs->fmap = map_load_first(ex, ci);
	}
	if (cs->fpq)
	{
		bool		isnull;
		Datum		d = ExecEvalExprSwitchContext(cs->fpq, ex->lutcx, &isnull);

		if (isnull || !DatumGetBool(d))
			return 0;
	}
	for (int k = 0; k < fm->nkey; k++)
	{
		const ParamExecData *prm = &estate->es_param_exec_vals[list_nth_int(j->params, j->first.keyarg[k])];

		if (prm->isnull)
			return 0;			/* a comparison with NULL is never true */
		if (fm->ktext[k])
		{
			struct varlena *t = PG_DETOAST_DATUM_PACKED(prm->value);

			kid[k] = map_key_id(fm, k, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), 0);
		}
		else
			kid[k] = map_key_id(fm, k, NULL, 0, sift_aggspec_operand_int(exprType(list_nth(j->args, j->first.keyarg[k])), prm->value));
		if (kid[k] == 0)
			return 0;
	}
	nm = map_find_ids(fm, kid, &rows);
	if (nm == 0)
		return 0;
	if (nm > 1)
	{
		ex->first_differ++;
		return -1;
	}
	if (m->nrow == m->cap)
		map_grow(m);
	for (int c = 0; c < m->ncol; c++)
	{
		bool		isnull;
		Datum		d = map_col(fm, rows[0], c, &isnull);

		map_col_put(m, c, d, isnull);
	}
	m->nrow++;
	ex->first_hit++;
	return 1;
}
