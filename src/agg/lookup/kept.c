/*
 * agg/lookup/kept.c - the kept conditions of a lookup table
 * The conditions a map keeps for the rows it holds, the view of a map under them and the
 * keyed dependence between maps.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/stats.h"
#include "agg/lookup/int.h"

/*
 * A table is kept when the scan only reads it and what narrows its load is the same for every query: the table's own columns (the Params after
 * the leaves) and constants. A preserved side is written to by the scan (the rows a tuple matched), and is loaded for its query alone.
 */
typedef struct KeptWalk
{
	int			first;
	int			last;
} KeptWalk;

static bool
kept_cond_bad(Node *n, KeptWalk *w)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param))
		return ((Param *) n)->paramkind != PARAM_EXEC || ((Param *) n)->paramid < w->first || ((Param *) n)->paramid >= w->last;
	if (IsA(n, SubPlan) || IsA(n, SubLink))
		return true;
	return expression_tree_walker(n, kept_cond_bad, w);
}

static bool
map_kept_conds(const SiftAggCombo *cb, const SiftAggJoin *j, bool own)
{
	KeptWalk	w = {cb->nleaves, cb->nleaves + j->ncol};

	if (sift_aggjoin_preserves(j->kind))
		return false;
	if (own && (kept_cond_bad((Node *) cb->slot[j->fslot].expr, &w) || contain_mutable_functions((Node *) cb->slot[j->fslot].expr)))
		return false;
	for (int i = 0; i < j->nidx; i++)
		if (kept_cond_bad((Node *) j->idxval[i], &w) || contain_mutable_functions((Node *) j->idxval[i]))
			return false;
	return true;
}

/* the table as an arm reads it: its columns, in its order, of those the rows were loaded with */
MapTab *
map_view(MapTab *m, const LookCols *have, const SiftAggJoin *j, MemoryContext mcxt)
{
	MapTab	   *v;
	bool		same = have->n == j->ncol;

	for (int c = 0; c < j->ncol && same; c++)
		same = have->att[c] == j->colatt[c] && have->type[c] == j->coltype[c];
	if (same)
		return m;
	v = MemoryContextAlloc(mcxt, sizeof(MapTab));
	*v = *m;
	v->ncol = j->ncol;
	v->mcol = MemoryContextAlloc(mcxt, Max(j->ncol, 1) * sizeof(MapCol));
	for (int c = 0; c < j->ncol; c++)
	{
		int			p;

		for (p = 0; p < have->n; p++)
			if (have->att[p] == j->colatt[c] && have->type[p] == j->coltype[c])
				break;
		Assert(p < have->n);
		v->mcol[c] = m->mcol[p];
	}
	return v;
}

/*
 * The rows of an earlier query when the table is as they were; else t->cx is where to load them, in a context the cache can take, and t->cols
 * the columns to load. The rows do not depend on the columns the arm reads unless its own conditions do (they read them by position): the arms
 * of a statement that read other columns of a table share one entry, which a query that reads more columns replaces by the union.
 */
MapTab *
map_kept_find(AggExec *ex, int ci, Relation rel, MemoryContext cxt, bool own, KeptTry *t)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	LookCols	need = {.n = j->ncol},
				have;
	MapTab	   *m;

	t->cx = cxt;
	if (!map_kept_conds(cb, j, own))
		return NULL;
	t->key = psprintf("%u|%s", GetUserId(), shared_map_key(ex, ci, own));
	/* the peek of a first-row lookup asks for a table only a cache holds: without an entry the finger would read every page of the table for nothing */
	if (ex->first_peek && !lookcache_known(t->key))
		return NULL;
	/* a table whose rows did not fit is not read page by page for nothing every time */
	if (lookcache_refused(t->key, rel) || (t->before = lookcache_finger(rel)) == NULL)
		return NULL;
	for (int c = 0; c < j->ncol; c++)
	{
		need.att[c] = j->colatt[c];
		need.type[c] = j->coltype[c];
	}
	if ((m = lookcache_find(t->key, t->before, &need, &have, cxt)) != NULL)
	{
		ex->ncached++;
		sift_stat_add(SIFT_STAT_LOOKUP_HITS, 1);
		return map_view(m, &have, j, cxt);
	}
	/* sessions that run the same query at once: one loads the rows, the others wait for them */
	if (!ex->first_peek && lookshm_claim(t->key, cxt) && (m = lookcache_find(t->key, t->before, &need, &have, cxt)) != NULL)
	{
		ex->ncached++;
		sift_stat_add(SIFT_STAT_LOOKUP_HITS, 1);
		return map_view(m, &have, j, cxt);
	}
	sift_stat_add(SIFT_STAT_LOOKUP_MISSES, 1);
	t->cols = need;
	/* the columns of the entry that lacked some, and these: a table loaded for one arm after another does not make an entry for each */
	for (int p = 0; !own && p < have.n && t->cols.n < SIFT_AGG_MAX_JCOLS; p++)
	{
		int			c;

		for (c = 0; c < need.n; c++)
			if (need.att[c] == have.att[p] && need.type[c] == have.type[p])
				break;
		if (c == need.n)
		{
			t->cols.att[t->cols.n] = have.att[p];
			t->cols.type[t->cols.n++] = have.type[p];
		}
	}
	t->cx = AllocSetContextCreate(cxt, "sift cache lookup table", ALLOCSET_DEFAULT_SIZES);
	return NULL;
}

/* keeps the rows just loaded for the next query, when the table was not written to meanwhile */
void
map_kept_keep(Relation rel, const KeptTry *t, MapTab *m, MemoryContext cxt)
{
	LookFinger *after;

	if (t->before != NULL && (after = lookcache_finger(rel)) != NULL)
		lookcache_keep(t->key, t->before, after, m, &t->cols, t->cx, cxt);
	if (t->before != NULL)
		lookshm_unclaim(t->key);
}

/*
 * A lookup table whose rows make a tuple only with the keys of another lookup: a later inner or semi join keyed by a column of the table alone (the ids of the
 * request joined to the table of their rows), or a child plan (a CTE or sub-select of ids) inner or semi joined on the same leaf of the scan as the table is.
 * The dependent of the fewest keys, with the column of the table that holds them (att, of the integer type typ): -1 when there is none.
 */
int
map_keyed_dependent(AggExec *ex, int ci, int *att, Oid *typ)
{
	const SiftAggSpec *s = ex->spec;
	const SiftAggCombo *cb = &s->combo[ci];
	const SiftAggJoin *j = &s->join[cb->join];
	int			best = -1;
	uint32		nbest = 0;

	if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || j->nidx > 0 || j->first.qual != NIL || j->first.nord > 0 || cb->level != 0)
		return -1;
	for (int cd = 0; cd < s->ncombo; cd++)
	{
		const SiftAggCombo *cc = &s->combo[cd];
		const SiftAggJoin *dj;
		int			a = 0;
		Oid			t = InvalidOid;

		if (cd == ci || cc->join < 0 || cc->level != 0)
			continue;
		dj = &s->join[cc->join];
		if ((dj->kind != SJ_INNER && dj->kind != SJ_SEMI) || dj->params != NIL || dj->nkey != 1 || dj->keynull[0])
			continue;
		if (combo_prior_keyed(ex, cd, ci))
		{
			int			c = dj->keyexpr[0] != NULL && IsA(dj->keyexpr[0], Param) ? cc->refcol[((Param *) dj->keyexpr[0])->paramid] : -1;

			if (c >= 0 && c < j->ncol)
			{
				a = j->colatt[c];
				t = j->coltype[c];
			}
		}
		else if (dj->child >= 0 && j->nkey == 1 && !j->keynull[0] && j->tkey[0] == NULL && j->keypos[0] >= 0 && dj->keypos[0] >= 0 &&
				 cc->leaf[dj->keypos[0]] == cb->leaf[j->keypos[0]] && cb->leaf[j->keypos[0]] >= 0)
		{
			a = j->keyatt[0];
			t = j->keytype[0];
		}
		if (a <= 0 || (t != INT4OID && t != INT8OID))
			continue;
		if (ex->cs[cd].map == NULL)
			ex->cs[cd].map = map_load(ex, cd);
		if (ex->cs[cd].map->ktext[0])
			continue;
		map_ensure(ex->cs[cd].map);
		if (best < 0 || ex->cs[cd].map->kd[0].n < nbest)
		{
			best = cd;
			nbest = ex->cs[cd].map->kd[0].n;
			*att = a;
			*typ = t;
		}
	}
	return best;
}
