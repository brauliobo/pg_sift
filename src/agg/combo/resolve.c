/*
 * agg/combo/resolve.c - combos: the resolve of a chunk
 * The vid of a wide value, the resolve of the combos of the rows of a chunk and the rows and
 * outputs it leaves.
 */
/* check-memory: allow-allocs the entries of the classes of a dense table, at most DENSE_MAX_CLASSES + 1 */
#include "postgres.h"

#include "agg/combo/int.h"

/* the id (1..) of the value of a 64-bit leaf at index i of the vector, 0 for NULL; a NaN of a numeric leaf is a value */
uint64
wide_vid(ComboSt *cs, int p, const SiftSrcLeaf *lf, const SiftSrcVec *v, uint32 i)
{
	int64		bits;

	if (v->ovf && sift_colsrc_bit(v->ovf, i))
		return id64_nan(&cs->id64[p]);
	if (v->isnull && v->isnull[i])
		return 0;
	if (lf->vtype == SCV_F64)
		memcpy(&bits, &((const double *) v->data)[i], sizeof(bits));
	else
		bits = ((const int64 *) v->data)[i];
	return id64_id(&cs->id64[p], bits);
}

/*
 * The entry of the key x (the value + 1 of an int4 leaf) of a dense table: the keys of a class share one, made for the first key seen of it (the table's own
 * row of the class, the key it was loaded with), and the keys without a row share one made for the first of them.
 */
static uint32
dense_ent(AggExec *ex, int ci, uint64 x, uint32 c)
{
	ComboSt    *cs = &ex->cs[ci];
	const MapTab *m = cs->map;
	uint64		v = c == 0 ? x : (uint64) (uint32) map_kint(m, c - 1, 0) + 1;

	if (cs->cent == NULL)
	{
		cs->cent = MemoryContextAllocZero(cs->cxt, ((Size) m->nrow + 1) * sizeof(uint32));
		cs->cbase = MemoryContextAllocZero(cs->cxt, ((Size) m->nrow + 1) * sizeof(uint32));
	}
	cs->cent[c] = combo_ent(ex, ci, &v) + 1;
	return cs->cent[c] - 1;
}

/*
 * The entries of the keys of a leaf without NULLs against a dense table: the class of a key is one array read and the entry of a class another, so the work of a
 * row is two loads and a store, where the general loop below tests each row for the NULL, the repeat of the value before it and the kind of the leaf.
 */
static void
resolve_dense(AggExec *ex, int ci, int n, const uint32 *idx, uint32 *eid, const SiftSrcVec *v)
{
	ComboSt    *cs = &ex->cs[ci];
	const MapTab *m = cs->map;
	const int32 *key = (const int32 *) v->data;
	const uint16 *cls = m->dcls;
	uint32		dn = m->dn;
	const uint32 *cent = cs->cent;

	for (int i = 0; i < n; i++)
	{
		int32		k = key[idx[i]];
		uint32		c = (uint32) k < dn ? cls[k] : 0;
		uint32		e = cent != NULL ? cent[c] : 0;

		if (e == 0)
		{
			/* the entry of a class not seen yet is made, which may make the table of the entries */
			e = dense_ent(ex, ci, (uint64) (uint32) k + 1, c) + 1;
			cent = cs->cent;
		}
		eid[i] = e - 1;
	}
}

#define CBASE_MANY	UINT32_MAX

/*
 * The output row of each tuple's key against a dense table whose every class has one row, in one pass: the class of a key and the row of a class are two
 * array reads, with no entry of the tuple made in between. False, with nothing written that matters, when the join is not that kind (a leaf that is not an
 * int4 without NULLs) or a class has no row (an orphan key of an INNER join) or several: the caller resolves the entries as for any join, and this
 * one is not tried again.
 */
bool
combo_dense_outs(AggExec *ex, int ci, int qlevel, int n, const uint32 *tidx, const uint32 *ridx, uint32 *cout)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const MapTab *m = cs->map;
	const SiftSrcLeaf *lf;
	const SiftSrcVec *v;
	const int32 *key;
	const uint32 *idx;
	uint32		dn;

	if (m == NULL || m->dcls == NULL || cs->dmany || cb->nleaves != 1 || cb->leaf[0] < 0 || cs->by_id || combo_null_input(ex))
		return false;
	lf = &ex->lay->leaf[cb->leaf[0]];
	v = get_vec(ex, cb->leaf[0]);
	if (leaf_wide(lf) || lf->vtype != SCV_I32 || v->isnull != NULL)
		return false;
	if (cs->resettable && cs->nent >= (uint32) sift_agg_combo_cache && !ex->cfree_ok[ci])
		combo_reset(ex, ci);
	idx = leaf_lvl(ex, cb->leaf[0]) == qlevel ? tidx : ridx;
	key = (const int32 *) v->data;
	dn = m->dn;
	for (int i = 0; i < n; i++)
	{
		int32		k = key[idx[i]];
		uint32		c = (uint32) k < dn ? m->dcls[k] : 0;
		uint32		b = cs->cbase != NULL ? cs->cbase[c] : 0;

		if (b == 0)
		{
			uint32		e = dense_ent(ex, ci, (uint64) (uint32) k + 1, c);

			b = cs->cbase[c] = cs->ent[e].nout == 1 ? cs->ent[e].base + 1 : CBASE_MANY;
		}
		if (b == CBASE_MANY)
		{
			cs->dmany = true;
			return false;
		}
		cout[i] = b - 1;
	}
	return true;
}

/*
 * combo_resolve of a combo of one leaf of the scan, the common case: a loop per kind of leaf (`kind` is a constant where it is inlined), the
 * directory of the entries probed inline, and the value of a position read once for the tuples that share it (a row seen from each of its
 * elements). The values and the entries come in the order of the tuples, as in the general loop, so the ids are the same.
 */
static pg_attribute_always_inline void
resolve_one(AggExec *ex, int ci, int n, const uint32 *idx, uint32 *eid, const uint64 *alive, const SiftSrcLeaf *lf, const SiftSrcVec *v,
			const SiftSrcVecType kind)
{
	ComboSt    *cs = &ex->cs[ci];
	uint32	   *map = kind == SCV_CODE ? dict_cmap(ex, dict_slot(ex, v->dict), v->dict, ci, 0) : NULL;
	uint32		last = UINT32_MAX;
	uint64		x = 0;
	bool		dense = kind == SCV_I32 && cs->map != NULL && cs->map->dcls != NULL;

	if (dense && alive == NULL && v->isnull == NULL)
	{
		resolve_dense(ex, ci, n, idx, eid, v);
		return;
	}
	for (int i = 0; i < n; i++)
	{
		uint32		d;

		if (alive && !sift_colsrc_bit(alive, i))
		{
			eid[i] = COMBO_NONE;
			continue;
		}
		if (idx[i] != last)
		{
			last = idx[i];
			if (kind == SCV_I64)
				x = wide_vid(cs, 0, lf, v, last);
			else if (v->isnull && v->isnull[last])
				x = 0;
			else if (kind == SCV_CODE)
				x = cmap_id(ex, map, v->dict, ci, 0, ((const uint32 *) v->data)[last]);
			else
				x = (uint64) (uint32) ((const int32 *) v->data)[last] + 1;
		}
		if (dense && x != 0)
		{
			uint32		c = map_dense_class(cs->map, (int64) (int32) (x - 1));

			eid[i] = cs->cent != NULL && cs->cent[c] != 0 ? cs->cent[c] - 1 : dense_ent(ex, ci, x, c);
		}
		else
			eid[i] = x < cs->dircap && (d = cs->dir[x]) != 0 ? d - 1 : combo_ent(ex, ci, &x);
	}
}

/* eid[0..n) of the leaf values in cvid; reuse: a tuple with the values of the one before it takes its entry; returns how many did */
static pg_attribute_always_inline int
combo_ents(AggExec *ex, int ci, int n, uint32 *eid, const uint64 *alive, bool reuse)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	int			hits = 0,
				last = -1;

	for (int i = 0; i < n; i++)
	{
		uint64		v[SIFT_AGG_MAX_CLEAVES];
		bool		same = reuse && last >= 0;

		if (alive && !sift_colsrc_bit(alive, i))
		{
			eid[i] = COMBO_NONE;
			continue;
		}
		for (int p = 0; same && p < cb->nleaves; p++)
			same = ex->cvid[p][i] == ex->cvid[p][last];
		if (same)
		{
			eid[i] = eid[last];
			hits++;
			continue;
		}
		/* combo_ent reads ninputs values: one even for a combo without leaves, whose value is 0 */
		v[0] = cb->nleaves > 0 ? ex->cvid[0][i] : 0;
		for (int p = 1; p < cb->nleaves; p++)
			v[p] = ex->cvid[p][i];
		eid[i] = combo_ent(ex, ci, v);
		last = i;
	}
	return hits;
}

/*
 * eid[i] = the entry of the combo's leaf values at tuple i: an element's leaf is read at tidx, a row's at ridx. A tuple that is not
 * alive (alive: bit i; NULL: all) has none (COMBO_NONE): no value of it reaches PostgreSQL's evaluation.
 */
static void
combo_resolve_gen(AggExec *ex, int ci, int qlevel, int n, const uint32 *tidx, const uint32 *ridx, uint32 *eid, const uint64 *alive, const Tup *tuples)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	ComboSt    *cs = &ex->cs[ci];
	int			hits;

	if (cs->resettable && cs->nent >= (uint32) sift_agg_combo_cache && !ex->cfree_ok[ci])
		combo_reset(ex, ci);
	if (cb->nleaves == 1 && cb->leaf[0] >= 0 && !cs->by_id && !combo_null_input(ex))
	{
		const SiftSrcLeaf *lf = &ex->lay->leaf[cb->leaf[0]];
		const SiftSrcVec *v = get_vec(ex, cb->leaf[0]);
		const uint32 *idx = leaf_lvl(ex, cb->leaf[0]) == qlevel ? tidx : ridx;

		if (leaf_wide(lf))
			resolve_one(ex, ci, n, idx, eid, alive, lf, v, SCV_I64);
		else if (lf->vtype == SCV_CODE)
			resolve_one(ex, ci, n, idx, eid, alive, lf, v, SCV_CODE);
		else
			resolve_one(ex, ci, n, idx, eid, alive, lf, v, SCV_I32);
		return;
	}
	if (cs->fstep && !combo_null_input(ex))
	{
		const SiftSrcLeaf *lf = &ex->lay->leaf[cb->leaf[0]];
		const uint32 *idx = leaf_lvl(ex, cb->leaf[0]) == qlevel ? tidx : ridx;

		if (combo_fstep_resolve(ex, ci, n, idx, eid, alive, lf, get_vec(ex, cb->leaf[0])))
			return;
	}
	for (int p = 0; p < cb->nleaves; p++)
	{
		uint64	   *vid = ex->cvid[p];

		if (cb->leaf[p] < 0)
		{
			Assert(tuples != NULL);
			for (int i = 0; i < n; i++)
				vid[i] = alive && !sift_colsrc_bit(alive, i) ? 0 : combo_prior_vid(ex, ci, p, tuples->cout[cb->refcombo[p]][i]);
		}
		else
		{
			const SiftSrcLeaf *lf = &ex->lay->leaf[cb->leaf[p]];
			const SiftSrcVec *v = get_vec(ex, cb->leaf[p]);
			const uint32 *idx = leaf_lvl(ex, cb->leaf[p]) == qlevel ? tidx : ridx;
			uint32	   *map = NULL;

			if (lf->vtype == SCV_CODE)
				map = dict_cmap(ex, dict_slot(ex, v->dict), v->dict, ci, p);
			for (int i = 0; i < n; i++)
			{
				/* the table of a column with many values does not fit the cache: ask for the line a few values ahead */
				if (i + WIDE_AHEAD < n && leaf_wide(lf))
					intids_prefetch(&cs->id64[p].ix, ((const int64 *) v->data)[idx[i + WIDE_AHEAD]]);
				if (alive && !sift_colsrc_bit(alive, i))
					vid[i] = 0;
				else if (leaf_wide(lf))
					vid[i] = wide_vid(cs, p, lf, v, idx[i]);
				else if (v->isnull && v->isnull[idx[i]])
					vid[i] = 0;
				else if (map)
					vid[i] = cmap_id(ex, map, v->dict, ci, p, ((const uint32 *) v->data)[idx[i]]);
				else
					vid[i] = (uint64) (uint32) ((const int32 *) v->data)[idx[i]] + 1;
			}
		}
	}
	/*
	 * The tuples of a row come together and often carry the same values. The test costs about as much as a hit saves a sixth of the times: a
	 * combo whose misses outrun what its hits pay back stops testing for the rest of the query.
	 */
	if (cs->reuse_off)
	{
		combo_ents(ex, ci, n, eid, alive, false);
		return;
	}
	hits = combo_ents(ex, ci, n, eid, alive, true);
	cs->reuse_debt = Max(cs->reuse_debt + (n - hits) - 5 * hits, -COMBO_REUSE_CREDIT);
	cs->reuse_off = cs->reuse_debt > COMBO_REUSE_CREDIT / 4;
}

/*
 * The earlier lookup whose output row alone decides the entries of combo ci, or -1: every input of ci is a column of that lookup's output row or the
 * one leaf the lookup itself is keyed by (so the row names the leaf's value too). A tuple's entry is then an array index by the row (byrow).
 */
static int
combo_row_parent(AggExec *ex, int ci)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	int			par = -1;

	for (int p = 0; p < cb->nleaves; p++)
		if (cb->leaf[p] < 0)
		{
			if (par >= 0 && par != cb->refcombo[p])
				return -1;
			par = cb->refcombo[p];
		}
	if (par < 0 || ex->spec->combo[par].join < 0 || ex->spec->combo[par].nleaves != 1)
		return -1;
	for (int p = 0; p < cb->nleaves; p++)
		if (cb->leaf[p] >= 0 && cb->leaf[p] != ex->spec->combo[par].leaf[0])
			return -1;
	return par;
}

/*
 * The tuples miss of a chunk are those whose parent row has no entry of this combo yet: resolved on their own (the chunk of a table with thousands of rows meets the
 * rare ones for many chunks, and resolves the rest of the chunk from the cache), and their entries are remembered by row.
 */
static void
resolve_missed(AggExec *ex, int ci, int qlevel, const uint32 *tidx, const uint32 *ridx, uint32 *eid, const Tup *tuples, const uint32 *miss, int nmiss)
{
	ComboSt    *cs = &ex->cs[ci];
	const uint32 *row = tuples->cout[cs->prow - 1];
	uint32	   *mem = palloc(4 * (Size) nmiss * sizeof(uint32)),
			   *mt = mem,
			   *mr = mem + nmiss,
			   *mrow = mem + 2 * (Size) nmiss,
			   *me = mem + 3 * (Size) nmiss;
	uint32	  **cout = memcpy(palloc(ex->spec->ncombo * sizeof(uint32 *)), tuples->cout, ex->spec->ncombo * sizeof(uint32 *));
	Tup			sub = *tuples;

	for (int j = 0; j < nmiss; j++)
	{
		mt[j] = tidx[miss[j]];
		mr[j] = ridx[miss[j]];
		mrow[j] = row[miss[j]];
	}
	cout[cs->prow - 1] = mrow;
	sub.cout = cout;
	sub.n = nmiss;
	combo_resolve_gen(ex, ci, qlevel, nmiss, mt, mr, me, NULL, &sub);
	for (int j = 0; j < nmiss; j++)
	{
		uint32		i = miss[j];

		eid[i] = me[j];
		if (me[j] == COMBO_NONE || row[i] == COMBO_NONE)
			continue;
		if (row[i] >= cs->nbyrow)
		{
			uint32		cap = Max(cs->nbyrow, 1024);

			while (cap <= row[i])
				cap *= 2;
			SIFT_REGROW_IN(cs->cxt, cs->byrow, (Size) cap * sizeof(uint32));
			memset(cs->byrow + cs->nbyrow, 0, (Size) (cap - cs->nbyrow) * sizeof(uint32));
			cs->nbyrow = cap;
		}
		cs->byrow[row[i]] = me[j] + 1;
	}
	pfree(cout);
	pfree(mem);
}

static void
combo_resolve_in(AggExec *ex, int ci, int qlevel, int n, const uint32 *tidx, const uint32 *ridx, uint32 *eid, const uint64 *alive, const Tup *tuples)
{
	ComboSt    *cs = &ex->cs[ci];
	const uint32 *row;
	uint32	   *miss;
	int			nmiss = 0;

	if (cs->prow == 0)
	{
		int			par = combo_row_parent(ex, ci);

		cs->prow = par < 0 ? -1 : par + 1;
	}
	if (cs->prow < 0 || tuples == NULL || combo_null_input(ex))
	{
		combo_resolve_gen(ex, ci, qlevel, n, tidx, ridx, eid, alive, tuples);
		return;
	}
	row = tuples->cout[cs->prow - 1];
	miss = palloc(n * sizeof(uint32));
	for (int i = 0; i < n; i++)
		if (alive && !sift_colsrc_bit(alive, i))
			eid[i] = COMBO_NONE;
		else if (row[i] < cs->nbyrow && cs->byrow[row[i]])
			eid[i] = cs->byrow[row[i]] - 1;
		else
			miss[nmiss++] = i;
	if (nmiss > 0)
		resolve_missed(ex, ci, qlevel, tidx, ridx, eid, tuples, miss, nmiss);
	pfree(miss);
}

/* a combo whose join runs a correlated child makes the entries a chunk adds together, so that their children are probed as one batch (combo/probe.c) */
void
combo_resolve(AggExec *ex, int ci, int qlevel, int n, const uint32 *tidx, const uint32 *ridx, uint32 *eid, const uint64 *alive, const Tup *tuples)
{
	ComboSt    *cs = &ex->cs[ci];

	if (!combo_probe_on(ex, ci))
	{
		combo_resolve_in(ex, ci, qlevel, n, tidx, ridx, eid, alive, tuples);
		return;
	}
	cs->dbatch = true;
	combo_resolve_in(ex, ci, qlevel, n, tidx, ridx, eid, alive, tuples);
	cs->dbatch = false;
	combo_probe_flush(ex, ci);
}

/* the output rows of the combo for the current tuples that are alive in `alive` (bit j of tuple j; NULL: all), COMBO_NONE for the others; into o */
void
combo_rows(AggExec *ex, int ci, const uint64 *alive, uint32 *o)
{
	ComboSt    *cs = &ex->cs[ci];

	combo_resolve(ex, ci, ex->qlevel, ex->cur->n, ex->tidx, ex->ridx, o, alive, ex->cur);
	for (int i = 0; i < ex->cur->n; i++)
		if (o[i] != COMBO_NONE)
			o[i] = cs->ent[o[i]].base;
}

/* the output row of the combo for each of the current tuples */
const uint32 *
combo_outs(AggExec *ex, int ci)
{
	uint32	   *o = ex->cfree[ci];

	if (ex->spec->combo[ci].join >= 0)
		return ex->cur->cout[ci];
	if (!ex->cfree_ok[ci])
	{
		combo_rows(ex, ci, NULL, o);
		ex->cfree_ok[ci] = true;
	}
	return o;
}
