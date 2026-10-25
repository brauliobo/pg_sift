/*
 * agg/init_table.c - the lookup tables: the join key filters, the load of a table and its reset
 */
#include "postgres.h"

#include "executor/nodeAgg.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/wide.h"
#include "agg/agg_int.h"

/*
 * A tuple of an inner or semi join needs a row of the table with its key, so the keys of the loaded table are a conjunct of the scan,
 * `leaf IN (keys)`: the leaf's zones, the source's filters and the row bitmaps apply it before a tuple is made, where the join would
 * resolve every tuple through the combo cache. The table is loaded here (the table's own conditions already hold for its rows).
 */
static bool
join_filter_leaf(AggExec *ex, int ci, int k, int *leaf)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const SiftSrcLeaf *lf;

	if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || j->keypos[k] < 0 || j->keynull[k] || cb->level != 0)
		return false;
	*leaf = cb->leaf[j->keypos[k]];
	if (*leaf < 0 || *leaf >= ex->nbase)
		return false;
	lf = &ex->lay->leaf[*leaf];
	return lf->kind == SCL_SCALAR && lf->parent < 0 && (lf->vtype == SCV_I32 || lf->typid == INT8OID);
}

/*
 * The rows of the table of combo ci that make a tuple: a later inner or semi join keyed by columns of the row alone (`JOIN ids ON ids.id =
 * p.id` after `JOIN p ON p.id = f.p_id`) drops the rows it has no match for, so the keys of the others are all that can pass. NULL: every row.
 */
static bool *
join_live_rows(AggExec *ex, int ci, MemoryContext caller)
{
	const MapTab *m = ex->cs[ci].map;
	bool	   *live = NULL;

	for (int cd = 0; cd < ex->spec->ncombo; cd++)
	{
		if (!combo_prior_keyed(ex, cd, ci))
			continue;
		if (ex->cs[cd].map == NULL)
		{
			/* a child plan runs in the context the scan was started in */
			MemoryContext old = MemoryContextSwitchTo(caller);

			ex->cs[cd].map = map_load(ex, cd);
			MemoryContextSwitchTo(old);
		}
		if (live == NULL)
		{
			live = palloc(Max(m->nrow, 1) * sizeof(bool));
			memset(live, true, m->nrow);
		}
		for (uint32 r = 0; r < m->nrow; r++)
		{
			live[r] = live[r] && combo_prior_matches(ex, cd, ci, r);
			if ((r & 1023) == 1023)
				ResetExprContext(ex->lutcx);
		}
		ResetExprContext(ex->lutcx);
	}
	return live;
}

/*
 * A top-N has no join to test the rows of its heap again, so the keys must be those of the rows that hold the rest of the ON clause (the table's
 * own conditions, which the planner has checked read no leaf of the scan) when the table was loaded whole. live: the rows kept so far, NULL all.
 */
static bool *
join_own_rows(AggExec *ex, int ci, bool *live)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const MapTab *m = ex->cs[ci].map;

	if (j->fslot < 0 || m->filtered)
		return live;
	if (live == NULL)
	{
		live = palloc(Max(m->nrow, 1) * sizeof(bool));
		memset(live, true, m->nrow);
	}
	for (uint32 r = 0; r < m->nrow; r++)
	{
		bool		isnull;
		Datum		d;

		if (!live[r])
			continue;
		for (int c = 0; c < j->ncol; c++)
			map_bind(ex, cb->nleaves + c, m, r, c);
		d = ExecEvalExprSwitchContext(ex->cs[ci].slot[j->fslot], ex->lutcx, &isnull);
		live[r] = !isnull && DatumGetBool(d);
		if ((r & 1023) == 1023)
			ResetExprContext(ex->lutcx);
	}
	ResetExprContext(ex->lutcx);
	return live;
}

static SiftAggPred *
join_filter_pred(AggExec *ex, int ci, int k, int leaf, int id, const bool *live)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const MapTab *m = ex->cs[ci].map;
	PredState  *st = &ex->ps[id];
	SiftAggPred *p;
	int64	   *vals = palloc(Max(m->nrow, 1) * sizeof(int64));
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	int			nv = 0;

	for (uint32 r = 0; r < m->nrow; r++)
	{
		int64		v = map_kint(m, r, k);

		if ((live && !live[r]) || (ex->lay->leaf[leaf].vtype == SCV_I32 && (v < PG_INT32_MIN || v > PG_INT32_MAX)))
			continue;				/* no int4 value equals it, or no tuple of the row survives the joins after it */
		vals[nv++] = v;
		lo = Min(lo, v);
		hi = Max(hi, v);
	}
	in_build(st, vals, nv, lo, hi);
	if (ex->spec->nparts > 0 && !IsParallelWorker() && ex->lay->leaf[leaf].path == NIL)
		ex->parts_pruned += sift_aggpart_prune_keys(ex->src, ex->spec->heapoid, list_nth(ex->lay->attexprs, ex->lay->leaf[leaf].attno - 1), vals, nv);
	pfree(vals);
	st->ready = true;
	p = palloc0(sizeof(SiftAggPred));
	p->kind = SAP_IN;
	p->id = id;
	p->leaf = leaf;
	p->leaf2 = -1;
	p->argtype = ex->lay->leaf[leaf].typid;
	p->desc = psprintf("%s IN (keys of %s)", ex->lay->leaf[leaf].name, j->desc);
	return p;
}

void
join_filters(AggExec *ex, MemoryContext caller)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	List	   *add = NIL;
	int			total = s->npreds_total;

	/* the joins of a top-N are only these filters: whatever the setting says */
	if (!sift_agg_join_filter && !s->topn)
		goto done;
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggJoin *j;
		int			leaf;

		if (s->combo[ci].join < 0)
			continue;
		j = &s->join[s->combo[ci].join];
		for (int k = 0; k < j->nkey; k++)
		{
			SiftAggPred *p;
			bool	   *live;

			if (!join_filter_leaf(ex, ci, k, &leaf))
				continue;
			if (ex->cs[ci].map == NULL)
			{
				/* a child plan runs in the context the scan was started in */
				MemoryContextSwitchTo(caller);
				ex->cs[ci].map = map_load(ex, ci);
				MemoryContextSwitchTo(ex->cxt);
			}
			if (ex->cs[ci].map->ktext[k])
				continue;
			/* the table of the predicate is the state of the id it is given: make room first */
			ex->ps = repalloc(ex->ps, (total + 1) * sizeof(PredState));
			memset(&ex->ps[total], 0, sizeof(PredState));
			live = join_live_rows(ex, ci, caller);
			if (s->topn)
				live = join_own_rows(ex, ci, live);
			if ((p = join_filter_pred(ex, ci, k, leaf, total, live)) != NULL)
			{
				add = lappend(add, p);
				total++;
				ex->cs[ci].kfilt = j->nkey == 1 && (ex->cs[ci].map->filtered || j->fslot < 0);
			}
			if (live)
				pfree(live);
		}
	}
	if (add != NIL)
	{
		SiftAggSpec *c = palloc(sizeof(SiftAggSpec));

		*c = *s;
		c->pred = palloc((s->npred + list_length(add)) * sizeof(SiftAggPred *));
		memcpy(c->pred, s->pred, s->npred * sizeof(SiftAggPred *));
		foreach_ptr(SiftAggPred, p, add)
			c->pred[c->npred++] = p;
		c->npreds_total = total;
		ex->spec = c;
		ex->njfilter = list_length(add);
	}
done:
	MemoryContextSwitchTo(old);
}

/*
 * Does an inner or semi join have no row to match (the table, with its own conditions, is empty)? Then no tuple of the scan exists and the
 * scan has nothing to read: the keys of an integer column are a conjunct that prunes every unit (join_filters), those of another type are
 * not, and the rows of the scan would be tested against an empty table one by one. The tables are loaded by now (scan_buffers).
 */
bool
join_none(AggExec *ex)
{
	const SiftAggSpec *s = ex->spec;

	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggCombo *cb = &s->combo[ci];
		const SiftAggJoin *j;

		if (cb->join < 0 || cb->level != 0 || ex->cs[ci].map == NULL)
			continue;
		j = &s->join[cb->join];
		if ((j->kind == SJ_INNER || j->kind == SJ_SEMI) && j->params == NIL && ex->cs[ci].map->nrow == 0)
			return true;
	}
	return false;
}

/* An empty group table, all in tcxt: key ids, group arrays, count(DISTINCT) sets */
void
table_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);
	uint64		total = 1;

	ex->hash = false;
	ex->direct_max = spill_started(ex) ? 0 : ex->lanes || ex->lane_serial ? LANE_DIRECT_MAX : DIRECT_MAX;
	/*
	 * The arrays of the direct table are allocated for every slot of the product of the key ranges, whether a group falls in it or not: a
	 * statement of 28 aggregates over keys that fill 1,200 of a million slots holds 218 MB. Past twice hash memory the groups go to the hash
	 * table, which holds what they use (at least the slots a lane gets).
	 */
	ex->direct_max = Min((uint64) ex->direct_max,
						 Max((uint64) LANE_DIRECT_MAX, 2 * (uint64) get_hash_memory_limit() / (sizeof(int64) * (1 + Max(s->naggs, 1)))));
	ex->ngroups = 0;
	ex->hkey = NULL;
	ex->hkcap = 0;
	ex->hslot = NULL;
	ex->nhslot = 0;
	ex->row_base = 0;
	for (int j = 0; j < s->nkeys; j++)
	{
		KeyState   *k = &ex->ks[j];
		KeyState	keep = {.expr = k->expr, .param = k->param, .ident = k->ident, .numk = k->numk, .bucket = k->bucket, .cal = k->cal};

		*k = keep;
		ot_init(&k->ot, s->key[j].eqop, s->key[j].typid, s->key[j].collid);
		rt_init(&k->rt, sizeof(uint32));
		k->cap = 4;
		k->nullid = UINT32_MAX;
		if (s->key[j].kind == SAK_SET)
			for (int i = 0; i < s->nsets; i++)
				ident_add(ex, k, i);
		else if (k->numk)
			idmap_init(&k->im);
		else if ((s->key[j].kind == SAK_LEAF && key_lf(ex, j)->vtype == SCV_F64) || k->bucket || (k->ident && spill_started(ex)))
			ident_go_wide(ex, k);
	}
	/* room for the groups the planner expects of one key, instead of doubling the arrays as the ids come */
	if (s->nkeys == 1 && !ex->lanes && spill_presize_ok(ex))
	{
		double		eg = part_flush_on(ex) ? s->est_groups / s->nparts : s->est_groups;

		if (eg > PRESIZE_MIN && eg <= ex->direct_max)
			ex->ks[0].cap = pg_nextpower2_32((uint32) eg);
	}
	for (int j = 0; j < s->nkeys; j++)
		total = Min((uint64) ex->direct_max + 1, total * ex->ks[j].cap);
	if (total > ex->direct_max || direct_oversized(ex, total))
	{
		total = Max(1024, spill_keep(ex));
		hash_init(ex, (uint32) total);
	}
	else
		direct_layout(ex);
	ex->gcap = total;
	ex->seen = palloc0(total * sizeof(int64));
	ex->acc = acc_alloc(ex, total);
	arms_resize(ex, total, NULL, 0);
	ex->dset = palloc0(Max(s->naggs, 1) * sizeof(DSet));
	ex->dist = palloc0(Max(s->naggs, 1) * sizeof(Dist *));
	ex->aot = palloc0(Max(s->naggs, 1) * sizeof(OutTable));
	ex->amax = palloc0(Max(s->naggs, 1) * sizeof(double));
	ex->amap = palloc0(Max(s->naggs, 1) * sizeof(IdMap));
	ex->osa = palloc0(Max(s->naggs, 1) * sizeof(Osa *));
	ex->oval = palloc0(Max(s->naggs, 1) * sizeof(OsaVal *));
	ex->xcmp = palloc0(Max(s->naggs, 1) * sizeof(FmgrInfo *));
	ex->xlen = palloc0(Max(s->naggs, 1) * sizeof(int16));
	ex->xbyval = palloc0(Max(s->naggs, 1) * sizeof(bool));
	for (int i = 0; i < s->naggs; i++)
	{
		SiftAggAgg *a = &s->agg[i];
		TypeCacheEntry *tc;

		if (a->kind == SAA_X_MIN || a->kind == SAA_X_MAX)
		{
			ex->xcmp[i] = &lookup_type_cache(a->argtype, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo;
			get_typlenbyval(a->argtype, &ex->xlen[i], &ex->xbyval[i]);
		}
		if (is_ordered_agg(a))
			ex->osa[i] = a->share == i ? osa_new(ex) : ex->osa[a->share];
		if (a->num.n && (a->kind == SAA_COUNT_DISTINCT || a->distinct))
			idmap_init(&ex->amap[i]);
		if (a->kind == SAA_GEN)
		{
			/* the hash set of a DISTINCT aggregate holds the byte images of its values; so does the table of the values of a joined table */
			for (int k = 0; k < a->gen->nargs; k++)
				if (a->gen->arg[k].mapped)
					ot_init_image(&ex->aot[i], a->gen->arg[k].typid);
			if (a->gen->distinct && !a->gen->sorted)
				ot_init_image(&ex->aot[i], a->gen->arg[a->gen->darg].typid);
			continue;
		}
		if (a->kind == SAA_COUNT_DISTINCT)
		{
			ex->dist[i] = dist_new(ex, !a->num.n && a->leaf >= 0 && ex->lay->leaf[a->leaf].vtype != SCV_CODE);
			if (i < 64 && (ex->dist_hash >> i & 1))
				dist_start_hash(ex->dist[i]);
		}
		else if (a->kind == SAA_O_COUNT_KEY)
			ex->dist[i] = dist_new(ex, false);
		if ((a->kind >= SAA_M_COUNT && a->kind <= SAA_M_AVG) || a->kind == SAA_M_PG)
			ex->dset[i].counted = a->kind == SAA_M_SUM || a->kind == SAA_M_AVG || a->kind == SAA_M_PG;
		else if (a->kind != SAA_COUNT_DISTINCT || a->leaf < 0 || ex->lay->leaf[a->leaf].vtype != SCV_CODE)
			continue;
		tc = lookup_type_cache(a->argtype, TYPECACHE_EQ_OPR);
		ot_init(&ex->aot[i], tc->eq_opr, a->argtype, a->collid);
	}
	MemoryContextSwitchTo(old);
	spill_floor(ex);
}

/* forgets every group (the dictionary maps hold key ids of the table) */
void
table_reset(AggExec *ex)
{
	int			nk = Max(ex->spec->nkeys + ex->spec->naggs, 1);

	for (int i = 0; i < NDICT_SLOTS; i++)
		if (ex->dslot[i].kmap)
			memset(ex->dslot[i].kmap, 0, nk * sizeof(uint32 *));
	dist_end_all(ex);
	MemoryContextReset(ex->tcxt);
	/* the output rows of a combo hold ids of the key tables just forgotten */
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		ComboSt    *cs = &ex->cs[ci];
		/* the ids it gave to the values of an earlier lookup's output rows are keyed by rows that start again: that memo goes with them (the value ids stay) */
		for (int p = 0; p < ex->spec->combo[ci].nleaves; p++)
			if (ex->spec->combo[ci].leaf[p] < 0)
			{
				if (cs->pv[p].memo)
					pfree(cs->pv[p].memo);
				if (cs->pv[p].rep)
					pfree(cs->pv[p].rep);
				memset(&cs->pv[p], 0, sizeof(cs->pv[p]));
			}
		/* the direct table forgets the ids of the entries only: it can be megabytes */
		for (uint32 x = 0; cs->dir && x < cs->nent; x++)
			if (!cs->null_input[x] && cs->vid[x] < cs->dircap)
				cs->dir[cs->vid[x]] = 0;
		cs->nent = cs->nout = 0;
		memset(cs->ht, 0, cs->nht * sizeof(uint32));
		combo_forget_priors(ex, ci);
	}
	table_init(ex);
	if (ex->spine)
		spine_reset(ex);
}
