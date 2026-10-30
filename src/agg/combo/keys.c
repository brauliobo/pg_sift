/*
 * agg/combo/keys.c - combos: the keys of the rows a combo matches
 * The key ids of a combo, the find of its rows, the prior keyed lookups and the filter of the
 * rows of a match.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/nodeFuncs.h"

#include "agg/combo/int.h"

typedef struct PriorKeyed
{
	const SiftAggCombo *cb;
	int			prior;
	bool		ok;
} PriorKeyed;

/*
 * The key of the entry (v: the ids of its leaves, NULL when every key is an expression); false when a key is NULL: no row matches. A NULL
 * key that equals NULL (keynull) is not that: nul[k] says so, and the rows to find are those whose key is NULL too.
 */
static bool
combo_keys(AggExec *ex, int ci, const uint64 *v, const char **ptr, uint32 *len, int64 *iv, bool *nul)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];

	for (int k = 0; k < j->nkey; k++)
	{
		if (j->keyexpr[k])
		{
			bool		isnull;
			Datum		d = ExecEvalExprSwitchContext(cs->kx[k], ex->lutcx, &isnull);

			if (isnull)
			{
				if (!j->keynull[k])
					return false;
				nul[k] = true;
				continue;
			}
			if (cs->map->ktext[k])
			{
				struct varlena *t = PG_DETOAST_DATUM_PACKED(d);

				ptr[k] = VARDATA_ANY(t);
				len[k] = VARSIZE_ANY_EXHDR(t);
			}
			else
				iv[k] = sift_aggspec_operand_int(exprType(j->keyexpr[k]), d);
			continue;
		}
		if (v[j->keypos[k]] == 0)
		{
			if (!j->keynull[k])
				return false;
			nul[k] = true;
			continue;
		}
		if (cs->map->ktext[k])
		{
			ptr[k] = cs->in[j->keypos[k]].ptr[v[j->keypos[k]] - 1];
			len[k] = cs->in[j->keypos[k]].len[v[j->keypos[k]] - 1];
		}
		else if (leaf_wide(&ex->lay->leaf[cb->leaf[j->keypos[k]]]))
			iv[k] = cs->id64[j->keypos[k]].val[v[j->keypos[k]] - 1];
		else
			iv[k] = (int32) (v[j->keypos[k]] - 1);
	}
	return true;
}

/*
 * The id of the table's value of key k: a text key read from a leaf has an interned id, and its translation is made once per value of the leaf
 * for the scan, not once per combination the value is in.
 */
static uint32
combo_key_id(AggExec *ex, int ci, int k, const uint64 *v, const char *ptr, uint32 len, int64 iv)
{
	const SiftAggJoin *j = &ex->spec->join[ex->spec->combo[ci].join];
	ComboSt    *cs = &ex->cs[ci];
	uint64		in;

	if (j->keyexpr[k] || !cs->map->ktext[k])
		return map_key_id(cs->map, k, ptr, len, iv);
	in = v[j->keypos[k]];
	if (in > cs->nmemo[k])
	{
		uint32		cap = Max(Max(cs->nmemo[k] * 2, (uint32) in), 256);

		cs->kmemo[k] = cs->kmemo[k] ? repalloc(cs->kmemo[k], cap * sizeof(uint32)) : MemoryContextAlloc(ex->cxt, cap * sizeof(uint32));
		memset(cs->kmemo[k] + cs->nmemo[k], 0xFF, (cap - cs->nmemo[k]) * sizeof(uint32));
		cs->nmemo[k] = cap;
	}
	if (cs->kmemo[k][in - 1] == COMBO_UNEVAL)
		cs->kmemo[k][in - 1] = map_key_id(cs->map, k, ptr, len, iv);
	return cs->kmemo[k][in - 1];
}

/* the rows of the table whose key is the entry's */
static int
combo_find(AggExec *ex, int ci, const uint64 *v, const char **ptr, const uint32 *len, const int64 *iv, const bool *nul, uint32 **rows)
{
	const SiftAggJoin *j = &ex->spec->join[ex->spec->combo[ci].join];
	MapTab	   *m = ex->cs[ci].map;
	uint32		kid[SIFT_AGG_MAX_JKEYS];

	*rows = NULL;
	if (m->nrow == 0)
		return 0;
	for (int k = 0; k < j->nkey; k++)
	{
		kid[k] = nul[k] ? 0 : combo_key_id(ex, ci, k, v, ptr[k], len[k], iv[k]);
		if (kid[k] == 0 && !nul[k])
			return 0;
	}
	return map_find_ids(m, kid, rows);
}

/* the rows of the table with the entry's key (none when a key is NULL) */
int
combo_matches(AggExec *ex, int ci, const uint64 *v, uint32 **rows)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const char *ptr[SIFT_AGG_MAX_JKEYS] = {0};
	uint32		len[SIFT_AGG_MAX_JKEYS] = {0};
	int64		iv[SIFT_AGG_MAX_JKEYS] = {0};
	bool		nul[SIFT_AGG_MAX_JKEYS] = {0};

	if (j->params != NIL)
		return combo_child_rows(ex, ci, v, rows);
	*rows = NULL;
	return combo_keys(ex, ci, v, ptr, len, iv, nul) ? combo_find(ex, ci, v, ptr, len, iv, nul, rows) : 0;
}

/* does every parameter the expression reads stand for a column of the table of the prior combo? */
static bool
reads_prior_only(Node *n, PriorKeyed *w)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param))
	{
		int			id = ((Param *) n)->paramid;

		w->ok &= ((Param *) n)->paramkind == PARAM_EXEC && id < w->cb->nleaves && w->cb->leaf[id] < 0 && w->cb->refcombo[id] == w->prior;
		return false;
	}
	return expression_tree_walker(n, reads_prior_only, w);
}

/*
 * Is the join of combo ci an inner or semi join whose keys are expressions of the columns of the table of combo `prior` alone? A row
 * of that table with no match in this one makes no tuple (combo_prior_matches).
 */
bool
combo_prior_keyed(AggExec *ex, int ci, int prior)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j;
	PriorKeyed	w = {cb, prior, true};

	if (cb->join < 0 || ci == prior || cb->level != 0)
		return false;
	j = &ex->spec->join[cb->join];
	if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || j->nkey == 0 || (j->fslot >= 0 && slot_reads_leaf(cb, j->fslot)))
		return false;
	for (int k = 0; k < j->nkey && w.ok; k++)
		if (j->keyexpr[k] && !j->keynull[k])
			reads_prior_only(j->keyexpr[k], &w);
		else
			w.ok = false;
	return w.ok;
}

/* does the row of the table of combo `prior` have a match in the table of ci (combo_prior_keyed)? */
bool
combo_prior_matches(AggExec *ex, int ci, int prior, uint32 row)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const MapTab *pm = ex->cs[prior].map;
	const char *ptr[SIFT_AGG_MAX_JKEYS] = {0};
	uint32		len[SIFT_AGG_MAX_JKEYS] = {0};
	int64		iv[SIFT_AGG_MAX_JKEYS] = {0};
	bool		nul[SIFT_AGG_MAX_JKEYS] = {0};
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	uint32	   *rows;
	int			nm;

	for (int p = 0; p < cb->nleaves; p++)
		if (cb->leaf[p] < 0 && cb->refcombo[p] == prior)
			map_bind(ex, p, pm, row, cb->refcol[p]);
	if (!combo_keys(ex, ci, NULL, ptr, len, iv, nul))
		return false;
	nm = map_find(ex->cs[ci].map, ptr, len, iv, &rows);
	return j->fslot >= 0 && !ex->cs[ci].map->filtered && nm > 0 ? combo_filter_rows(ex, ci, rows, nm) > 0 : nm > 0;
}

/* the rows that hold the rest of the ON clause, moved to the front: how many */
int
combo_filter_rows(AggExec *ex, int ci, uint32 *rows, int nm)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];
	int			keep = 0;

	for (int r = 0; r < nm; r++)
	{
		bool		isnull;
		Datum		d;

		for (int c = 0; c < j->ncol; c++)
			map_bind(ex, cb->nleaves + c, cs->map, rows[r], c);
		d = ExecEvalExprSwitchContext(cs->slot[j->fslot], ex->lutcx, &isnull);
		if (!isnull && DatumGetBool(d))
			rows[keep++] = rows[r];
	}
	ResetExprContext(ex->lutcx);
	return keep;
}
