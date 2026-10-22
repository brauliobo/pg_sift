/*
 * agg/combo.c - combos: expressions PostgreSQL evaluates over combinations of values, and their ids
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

#define CHUNK_PAD	(VEC + PADW)

/* the id (1..) of a value among the distinct values of a dictionary leaf; the bytes are copied when new */
static uint32
intern_id(AggExec *ex, Intern *t, const char *b, uint32 len)
{
	uint32		hv = hash_bytes((const unsigned char *) b, len),
				h;
	char	   *cp;

	if (t->nslot == 0)
	{
		t->nslot = 64;
		t->cap = 16;
		t->slot = MemoryContextAllocZero(ex->cxt, t->nslot * sizeof(uint32));
		t->ptr = MemoryContextAlloc(ex->cxt, t->cap * sizeof(char *));
		t->len = MemoryContextAlloc(ex->cxt, t->cap * sizeof(uint32));
		t->hv = MemoryContextAlloc(ex->cxt, t->cap * sizeof(uint32));
	}
	h = hv & (t->nslot - 1);
	for (uint32 id; (id = t->slot[h]) != 0; h = (h + 1) & (t->nslot - 1))
		if (t->hv[id - 1] == hv && t->len[id - 1] == len && memcmp(t->ptr[id - 1], b, len) == 0)
			return id;
	if (t->n == t->cap)
	{
		t->cap *= 2;
		t->ptr = repalloc(t->ptr, t->cap * sizeof(char *));
		t->len = repalloc(t->len, t->cap * sizeof(uint32));
		t->hv = repalloc(t->hv, t->cap * sizeof(uint32));
	}
	cp = MemoryContextAlloc(ex->cxt, len + 1);
	memcpy(cp, b, len);
	cp[len] = '\0';
	t->ptr[t->n] = cp;
	t->len[t->n] = len;
	t->hv[t->n] = hv;
	t->slot[h] = ++t->n;
	if (t->n * 2 > t->nslot)
	{
		pfree(t->slot);
		t->nslot *= 2;
		t->slot = MemoryContextAllocZero(ex->cxt, t->nslot * sizeof(uint32));
		for (uint32 id = 1; id <= t->n; id++)
		{
			h = t->hv[id - 1] & (t->nslot - 1);
			while (t->slot[h])
				h = (h + 1) & (t->nslot - 1);
			t->slot[h] = id;
		}
	}
	return t->n;
}

#define CMAP_UNSET	UINT32_MAX

/* the ids over all dictionaries of the entries of d, for dictionary leaf p of combo ci; an entry gets its id when a tuple first reads it (cmap_id) */
static uint32 *
dict_cmap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ci, int p)
{
	int			slot = ex->spec->nkeys + ex->spec->naggs + ex->clbase[ci] + p;
	uint32	   *map = s->kmap[slot];

	if (map)
		return map;
	map = MemoryContextAlloc(ex->cxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
	memset(map, 0xFF, d->n * sizeof(uint32));
	map[d->n] = 0;
	s->kmap[slot] = map;
	return map;
}

/* the id of entry c of the dictionary: a dictionary has far more entries than a scan reads (every distinct free text of a column) */
static inline uint32
cmap_id(AggExec *ex, uint32 *map, const SiftSrcDict *d, int ci, int p, uint32 c)
{
	if (map[c] == CMAP_UNSET)
		map[c] = intern_id(ex, &ex->cs[ci].in[p], d->bytes + d->off[c], d->off[c + 1] - d->off[c]);
	return map[c];
}

static void
id64_init(Id64 *m)
{
	m->cxt = CurrentMemoryContext;
	m->nslot = 4096;
	m->n = 0;
	m->cap = 2048;
	m->nan_id = 0;
	m->tab = palloc0(m->nslot * sizeof(Id64Ent));
	m->val = palloc(m->cap * sizeof(int64));
}

static inline uint32
id64_hash(int64 k)
{
	uint64		x = (uint64) k * UINT64CONST(0x9E3779B97F4A7C15);

	return (uint32) ((x ^ (x >> 29)) * UINT64CONST(0xD6E8FEB86659FD93) >> 32);
}

static uint32
id64_new(Id64 *m, int64 k)
{
	if (m->n == m->cap)
	{
		m->cap *= 2;
		m->val = repalloc(m->val, m->cap * sizeof(int64));
	}
	m->val[m->n] = k;
	return ++m->n;
}

static uint32
id64_id(Id64 *m, int64 k)
{
	uint32		mask = m->nslot - 1,
				h = id64_hash(k) & mask;

	for (; m->tab[h].id; h = (h + 1) & mask)
		if (m->tab[h].key == k)
			return m->tab[h].id;
	m->tab[h] = (Id64Ent) {k, id64_new(m, k)};
	if (m->n * 2 > m->nslot)
	{
		Id64Ent    *old = m->tab;

		m->nslot *= 4;
		m->tab = MemoryContextAllocZero(m->cxt, m->nslot * sizeof(Id64Ent));
		mask = m->nslot - 1;
		for (uint32 id = 1; id <= m->n; id++)
		{
			if (id == m->nan_id)
				continue;
			for (h = id64_hash(m->val[id - 1]) & mask; m->tab[h].id; h = (h + 1) & mask)
				;
			m->tab[h] = (Id64Ent) {m->val[id - 1], id};
		}
		pfree(old);
		return m->n;
	}
	return m->tab[h].id;
}

#define WIDE_AHEAD	12

static uint32
id64_nan(Id64 *m)
{
	if (m->nan_id == 0)
		m->nan_id = id64_new(m, 0);
	return m->nan_id;
}

/* a stored value of a 64-bit leaf (a float: its bits) as the Datum of the leaf's type; a numeric is the integer times 10^-scale, with that many digits */
static Datum
leaf64_datum(const Id64 *m, const SiftSrcLeaf *lf, uint32 id)
{
	int64		v = m->val[id - 1];

	if (lf->vtype == SCV_F64)
		return lf->typid == FLOAT4OID ? Float4GetDatum((float4) float_from_key(v)) : Float8GetDatum(float_from_key(v));
	if (lf->typid != NUMERICOID)
		return Int64GetDatum(v);
	if (id == m->nan_id)
		return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	return NumericGetDatum(lf->scale > 0 ? int64_div_fast_to_numeric(v, lf->scale) : int64_to_numeric(v));
}

/* the leaf values of an entry (v: ids) as the Params of the combo's expressions */
void
combo_bind_leaves(AggExec *ex, int ci, const uint64 *v)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	ComboSt    *cs = &ex->cs[ci];

	bind_param(ex, SIFT_AGG_NULL_INPUT, BoolGetDatum(ex->null_input), false);

	for (int p = 0; p < cb->nleaves; p++)
	{
		const SiftSrcLeaf *lf;

		if (cb->leaf[p] < 0)
		{
			ComboSt    *prior = &ex->cs[cb->refcombo[p]];
			uint32		row = prior->maprow[v[p] - 1];

			if (row == UINT32_MAX)
				bind_param(ex, p, (Datum) 0, true);
			else
				map_bind(ex, p, prior->map, row, cb->refcol[p]);
			continue;
		}
		lf = &ex->lay->leaf[cb->leaf[p]];

		if (v[p] == 0)
			bind_param(ex, p, (Datum) 0, true);
		else if (lf->vtype == SCV_CODE)
			bind_param(ex, p, text_datum(cb->ptype[p], cb->ptypmod[p], cs->in[p].ptr[v[p] - 1], cs->in[p].len[v[p] - 1]), false);
		else if (leaf_wide(lf))
			bind_param(ex, p, leaf64_datum(&cs->id64[p], lf, (uint32) v[p]), false);
		else
			bind_param(ex, p, leaf_datum(cb->ptype[p], (int32) (v[p] - 1)), false);
	}
}

/* A native correlated child runs for this input only; cached output rows keep their own identity. */
static int
combo_child_rows(AggExec *ex, int ci, uint32 **rows)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	EState     *estate = ex->css.ss.ps.state;
	PlanState  *child = list_nth(estate->es_subplanstates, j->subplan - 1);
	ListCell   *lp, *la;
	int         start = cs->map->nrow, n = 0;
	MemoryContext caller = CurrentMemoryContext;
	instr_time  t0;

	INSTR_TIME_SET_CURRENT(t0);
	forboth(lp, j->params, la, cs->args)
	{
		int         id = lfirst_int(lp);
		ParamExecData *prm = &estate->es_param_exec_vals[id];

		prm->value = ExecEvalExprSwitchContext(lfirst(la), ex->lutcx, &prm->isnull);
		Assert(prm->execPlan == NULL);
		child->chgParam = bms_add_member(child->chgParam, id);
	}
	MemoryContextSwitchTo(estate->es_query_cxt);
	ExecReScan(child);
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);

		if (TupIsNull(slot))
			break;
		MemoryContextSwitchTo(ex->cxt);
		map_row(ex, cs->map, slot, j, cb, NULL);
		MemoryContextSwitchTo(estate->es_query_cxt);
	}
	MemoryContextSwitchTo(caller);
	n = cs->map->nrow - start;
	map_loaded(ex, n, t0);
	*rows = palloc(Max(n, 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
		(*rows)[i] = start + i;
	return n;
}

/* the key of the entry (v: the ids of its leaves, NULL when every key is an expression); false when a key is NULL: no row matches */
static bool
combo_keys(AggExec *ex, int ci, const uint64 *v, const char **ptr, uint32 *len, int64 *iv)
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
				return false;
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
			return false;
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
combo_find(AggExec *ex, int ci, const uint64 *v, const char **ptr, const uint32 *len, const int64 *iv, uint32 **rows)
{
	const SiftAggJoin *j = &ex->spec->join[ex->spec->combo[ci].join];
	MapTab	   *m = ex->cs[ci].map;
	uint32		kid[SIFT_AGG_MAX_JKEYS];

	*rows = NULL;
	if (m->nrow == 0)
		return 0;
	for (int k = 0; k < j->nkey; k++)
		if ((kid[k] = combo_key_id(ex, ci, k, v, ptr[k], len[k], iv[k])) == 0)
			return 0;
	return map_find_ids(m, kid, rows);
}

/* the rows of the table with the entry's key (none when a key is NULL) */
static int
combo_matches(AggExec *ex, int ci, const uint64 *v, uint32 **rows)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const char *ptr[SIFT_AGG_MAX_JKEYS] = {0};
	uint32		len[SIFT_AGG_MAX_JKEYS] = {0};
	int64		iv[SIFT_AGG_MAX_JKEYS] = {0};

	if (j->params != NIL)
		return combo_child_rows(ex, ci, rows);
	*rows = NULL;
	return combo_keys(ex, ci, v, ptr, len, iv) ? combo_find(ex, ci, v, ptr, len, iv, rows) : 0;
}

typedef struct PriorKeyed
{
	const SiftAggCombo *cb;
	int			prior;
	bool		ok;
} PriorKeyed;

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
		if (j->keyexpr[k])
			reads_prior_only(j->keyexpr[k], &w);
		else
			w.ok = false;
	return w.ok;
}

static int combo_filter_rows(AggExec *ex, int ci, uint32 *rows, int nm);

/* does the row of the table of combo `prior` have a match in the table of ci (combo_prior_keyed)? */
bool
combo_prior_matches(AggExec *ex, int ci, int prior, uint32 row)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const MapTab *pm = ex->cs[prior].map;
	const char *ptr[SIFT_AGG_MAX_JKEYS] = {0};
	uint32		len[SIFT_AGG_MAX_JKEYS] = {0};
	int64		iv[SIFT_AGG_MAX_JKEYS] = {0};
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	uint32	   *rows;
	int			nm;

	for (int p = 0; p < cb->nleaves; p++)
		if (cb->leaf[p] < 0 && cb->refcombo[p] == prior)
			map_bind(ex, p, pm, row, cb->refcol[p]);
	if (!combo_keys(ex, ci, NULL, ptr, len, iv))
		return false;
	nm = map_find(ex->cs[ci].map, ptr, len, iv, &rows);
	return j->fslot >= 0 && !ex->cs[ci].map->filtered && nm > 0 ? combo_filter_rows(ex, ci, rows, nm) > 0 : nm > 0;
}

/* the rows that hold the rest of the ON clause, moved to the front: how many */
static int
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

/* the output rows of a tuple whose key has nm matches */
static uint32
join_nout(SiftJoinKind kind, int nm)
{
	switch (kind)
	{
		case SJ_INNER:
		case SJ_RIGHT:
			return nm;
		case SJ_LEFT:
		case SJ_FULL:
			return nm > 0 ? nm : 1;
		case SJ_SEMI:
			return nm > 0;
		case SJ_ANTI:
			return nm == 0;
	}
	return 0;
}

static void
combo_grow_out(AggExec *ex, ComboSt *cs, int nslots, uint32 add)
{
	if (cs->nout + add > cs->outcap)
	{
		cs->outcap = Max(Max(cs->outcap * 2, cs->nout + add), 64);
		cs->val = cs->val ? repalloc(cs->val, (size_t) cs->outcap * Max(nslots, 1) * sizeof(uint32)) :
			MemoryContextAlloc(cs->cxt, (size_t) cs->outcap * Max(nslots, 1) * sizeof(uint32));
		cs->maprow = cs->maprow ? repalloc(cs->maprow, cs->outcap * sizeof(uint32)) :
			MemoryContextAlloc(cs->cxt, cs->outcap * sizeof(uint32));
	}
}

/* the table of the distinct values a slot gives ids to: of the key, or of the aggregate it is the argument of */
static OutTable *
slot_ot(AggExec *ex, const SiftAggSlot *sl)
{
	return sl->agg >= 0 ? &ex->aot[sl->agg] : &ex->ks[sl->key].ot;
}

/*
 * A finite numeric as the integer times 10^-dscale. This reads the stored form of the type (a short or long header, base 10000
 * digits, the weight of the first one), which is what pg_upgrade keeps stable; NaN, the infinities and what does not fit 38 digits
 * are not ok and stay numerics (the sums add them with numeric_add).
 */
static void
numeric_decode(Datum d, DNum *out)
{
	const char *data = VARDATA_ANY(DatumGetPointer(d));
	Size		len = VARSIZE_ANY_EXHDR(DatumGetPointer(d));
	const sift_i128 *p10 = pow10_tab();
	uint16		hdr;
	int			hsz,
				weight,
				dscale;
	bool		neg;
	sift_i128	m = 0;

	out->ok = false;
	memcpy(&hdr, data, sizeof(hdr));
	if ((hdr & 0xC000) == 0xC000)
		return;
	if (hdr & 0x8000)
	{
		hsz = 2;
		dscale = (hdr & 0x1F80) >> 7;
		weight = (hdr & 0x0040) ? (int) (~0x003F | (hdr & 0x003F)) : (int) (hdr & 0x003F);
		neg = (hdr & 0x2000) != 0;
	}
	else
	{
		int16		w;

		hsz = 4;
		memcpy(&w, data + 2, sizeof(w));
		dscale = hdr & 0x3FFF;
		weight = w;
		neg = (hdr & 0xC000) == 0x4000;
	}
	for (int i = 0; i < (int) ((len - hsz) / 2); i++)
	{
		int16		dg;
		int			e = 4 * (weight - i) + dscale;
		sift_i128	t;

		memcpy(&dg, data + hsz + 2 * i, sizeof(dg));
		if (dg == 0)
			continue;
		if (e < 0)
		{
			if (e < -3 || dg % (int) p10[-e] != 0)
				return;
			t = dg / (int) p10[-e];
		}
		else if (e > 38 || __builtin_mul_overflow((sift_i128) dg, p10[e], &t))
			return;
		if (__builtin_add_overflow(m, t, &m))
			return;
	}
	out->m = neg ? -m : m;
	out->scale = (int16) dscale;
	out->ok = dscale <= 38;
}

/* a new value of a direct slot, kept in the cache; the number (from 1) it goes by */
static uint32
dat_put(AggExec *ex, ComboSt *cs, int sl, Datum d)
{
	Datum		v = d;

	if (cs->ndat == cs->datcap)
	{
		MemoryContext old = MemoryContextSwitchTo(cs->cxt);

		cs->datcap = Max(cs->datcap * 2, 64);
		cs->dat = cs->dat ? repalloc(cs->dat, cs->datcap * sizeof(Datum)) : palloc(cs->datcap * sizeof(Datum));
		if (cs->decode)
			cs->dnum = cs->dnum ? repalloc(cs->dnum, cs->datcap * sizeof(DNum)) : palloc(cs->datcap * sizeof(DNum));
		MemoryContextSwitchTo(old);
	}
	if (!cs->tbyval[sl])
	{
		Size		sz = cs->tlen[sl] == -1 ? VARSIZE_ANY(DatumGetPointer(d)) : cs->tlen[sl];

		if (cs->arena_left < sz)
		{
			cs->arena_left = Max(sz, 65536);
			cs->arena = MemoryContextAlloc(cs->cxt, cs->arena_left);
		}
		memcpy(cs->arena, DatumGetPointer(d), sz);
		v = PointerGetDatum(cs->arena);
		sz = MAXALIGN(sz);
		cs->arena += Min(sz, cs->arena_left);
		cs->arena_left -= Min(sz, cs->arena_left);
	}
	if (cs->decode && cs->decode[sl])
		numeric_decode(v, &cs->dnum[cs->ndat]);
	cs->dat[cs->ndat++] = v;
	return cs->ndat;
}

/*
 * The value of slot sl in the output row `row` of a combo without a join (its entry is the row), evaluated by PostgreSQL when the first
 * tuple that needs it comes: an expression may raise an error (a division by zero) that the normal plan raises only for the rows that
 * reach the expression, and a slot is read only for the tuples that do.
 */
uint32
combo_eval_slot(AggExec *ex, int ci, uint32 row, int sl)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	ComboSt    *cs = &ex->cs[ci];
	MemoryContext old = MemoryContextSwitchTo(ex->comcxt);
	bool		isnull;
	Datum		d;
	uint32		r;

	combo_bind_leaves(ex, ci, &cs->vid[(size_t) row * cs->ninputs]);
	d = ExecEvalExprSwitchContext(cs->slot[sl], ex->lutcx, &isnull);
	if (isnull)
		r = 0;
	else if (cb->slot[sl].direct)
		r = dat_put(ex, cs, sl, d);
	else if (!cb->slot[sl].is_key)
		r = DatumGetBool(d);
	else
	{
		MemoryContextSwitchTo(ex->cxt);
		r = ot_id(slot_ot(ex, &cb->slot[sl]), d);
	}
	MemoryContextSwitchTo(old);
	/* the datums of the leaves and the temporaries of the expression are dropped every few evaluations, not after each */
	if ((++cs->nevals & 63) == 0)
	{
		ResetExprContext(ex->lutcx);
		MemoryContextReset(ex->comcxt);
	}
	cs->val[(size_t) row * cb->nslots + sl] = r;
	return r;
}

/* the output rows of an entry with nm matching rows (none: a row of NULL columns), every slot evaluated; in the combo's work context */
void
combo_emit(AggExec *ex, int ci, ComboEnt *ent, const uint32 *rows, int nm, uint32 nout)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];
	MapTab	   *m = cs->map;

	combo_grow_out(ex, cs, cb->nslots, nout);
	ent->nout = nout;
	ent->base = cs->nout;
	for (uint32 o = 0; o < nout; o++)
	{
		cs->maprow[cs->nout] = nm > 0 ? rows[o] : UINT32_MAX;
		for (int c = 0; c < j->ncol; c++)
			if (nm > 0)
				map_bind(ex, cb->nleaves + c, m, rows[o], c);
			else
				bind_param(ex, cb->nleaves + c, (Datum) 0, true);
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			bool		isnull;
			Datum		d = ExecEvalExprSwitchContext(cs->slot[sl], ex->lutcx, &isnull);
			uint32		r;

			if (!cb->slot[sl].is_key)
				r = !isnull && DatumGetBool(d);
			else if (isnull)
				r = 0;
			else
			{
				MemoryContextSwitchTo(ex->cxt);
				r = ot_id(slot_ot(ex, &cb->slot[sl]), d);
				MemoryContextSwitchTo(ex->comcxt);
			}
			cs->val[(size_t) cs->nout * cb->nslots + sl] = r;
		}
		cs->nout++;
		ResetExprContext(ex->lutcx);
	}
}

/* the output rows of a new entry: with a join its matches, every slot evaluated; without, one row whose slots wait for the tuples that need them */
static void
combo_make(AggExec *ex, int ci, const uint64 *v, ComboEnt *ent)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = cb->join >= 0 ? &ex->spec->join[cb->join] : NULL;
	ComboSt    *cs = &ex->cs[ci];
	MemoryContext old;
	uint32	   *rows;
	int			nm;

	if (j == NULL)
	{
		combo_grow_out(ex, cs, cb->nslots, 1);
		ent->nout = 1;
		ent->base = cs->nout;
		for (int sl = 0; sl < cb->nslots; sl++)
			cs->val[(size_t) cs->nout * cb->nslots + sl] = COMBO_UNEVAL;
		cs->nout++;
		return;
	}
	old = MemoryContextSwitchTo(ex->comcxt);
	combo_bind_leaves(ex, ci, v);
	nm = combo_matches(ex, ci, v, &rows);
	if (j->fslot >= 0 && nm > 0)
		nm = combo_filter_rows(ex, ci, rows, nm);
	for (int r = 0; cs->map->keep && r < nm; r++)
		cs->map->hit[rows[r]] = true;
	combo_emit(ex, ci, ent, rows, nm, join_nout(j->kind, nm));
	MemoryContextSwitchTo(old);
	MemoryContextReset(ex->comcxt);
}

/* the entry of the tuple of NULL leaves whose output rows are the rows of the preserved side that no tuple matched (their columns, NULL leaves) */
ComboEnt
combo_unmatched(AggExec *ex, int ci, const uint32 *prior)
{
	const MapTab *m = ex->cs[ci].map;
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	uint64		v[SIFT_AGG_MAX_CLEAVES] = {0};
	ComboEnt	ent;
	MemoryContext old = MemoryContextSwitchTo(ex->comcxt);
	uint32	   *rows = palloc(Max(m->nrow, 1) * sizeof(uint32));
	int			nm = 0;

	for (uint32 r = 0; r < m->nrow; r++)
		if (!m->hit[r])
			rows[nm++] = r;
	for (int p = 0; p < cb->nleaves; p++)
		if (cb->leaf[p] < 0)
			v[p] = (uint64) prior[cb->refcombo[p]] + 1;
	combo_bind_leaves(ex, ci, v);
	combo_emit(ex, ci, &ent, rows, nm, nm);
	MemoryContextSwitchTo(old);
	MemoryContextReset(ex->comcxt);
	return ent;
}

/* the ids are small and dense: every input is multiplied in and folded, or the entries of a few hundred thousand combinations pile up in a few slots */
static inline uint32
combo_hash(const uint64 *v, int ninputs, bool null_input)
{
	uint64		h = UINT64CONST(0x9E3779B97F4A7C15);

	for (int p = 0; p < ninputs; p++)
	{
		h = (h ^ v[p]) * UINT64CONST(0xD6E8FEB86659FD93);
		h ^= h >> 32;
	}
	h *= UINT64CONST(0x9E3779B97F4A7C15);
	return (uint32) (h >> 32) ^ (null_input ? 0x85EBCA6BU : 0);
}

/* room for one more entry */
static void
combo_grow_ent(ComboSt *cs)
{
	if (cs->nent < cs->entcap)
		return;
	cs->entcap = Max(cs->entcap * 2, 64);
	cs->vid = cs->vid ? repalloc(cs->vid, (size_t) cs->entcap * cs->ninputs * sizeof(uint64)) :
		MemoryContextAlloc(cs->cxt, (size_t) cs->entcap * cs->ninputs * sizeof(uint64));
	cs->ent = cs->ent ? repalloc(cs->ent, (size_t) cs->entcap * sizeof(ComboEnt)) :
		MemoryContextAlloc(cs->cxt, (size_t) cs->entcap * sizeof(ComboEnt));
	cs->null_input = cs->null_input ? repalloc(cs->null_input, cs->entcap * sizeof(bool)) :
		MemoryContextAlloc(cs->cxt, cs->entcap * sizeof(bool));
}

/* the entry of a value id of a by_id combo: the entries up to it exist (ids come in order from 1, 0 is the NULL) */
static uint32
combo_ent_by_id(AggExec *ex, int ci, uint32 id)
{
	ComboSt    *cs = &ex->cs[ci];

	if (id >= (uint32) 1 << 24)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: too many distinct value combinations for the aggregate scan"),
				 errhint("Set sift.enable_agg to off for this query.")));
	while (cs->nent <= id)
	{
		uint64	   *v;

		combo_grow_ent(cs);
		v = &cs->vid[(size_t) cs->nent * cs->ninputs];
		memset(v, 0, cs->ninputs * sizeof(uint64));
		v[0] = cs->nent;
		cs->null_input[cs->nent] = false;
		combo_make(ex, ci, v, &cs->ent[cs->nent]);
		cs->nent++;
	}
	return id;
}

/* entry e (1..) is the one of id k: the table grows to hold k */
static void
combo_dir_set(ComboSt *cs, uint64 k, uint32 e)
{
	if (k >= cs->dircap)
	{
		uint32		cap = Max(cs->dircap, 1024);

		while (cap <= k)
			cap *= 2;
		cs->dir = cs->dir ? repalloc(cs->dir, (size_t) cap * sizeof(uint32)) : MemoryContextAlloc(cs->cxt, (size_t) cap * sizeof(uint32));
		memset(cs->dir + cs->dircap, 0, (size_t) (cap - cs->dircap) * sizeof(uint32));
		cs->dircap = cap;
	}
	cs->dir[k] = e;
}

/* the entry of a tuple of leaf values, made on its first sight */
static uint32
combo_ent(AggExec *ex, int ci, const uint64 *v)
{
	ComboSt    *cs = &ex->cs[ci];
	uint32		h,
				e;
	bool		direct;

	if (cs->by_id)
		return combo_ent_by_id(ex, ci, (uint32) v[0]);
	/* one input with a small id (a dictionary code, a small integer): the entry is an array index, no hash */
	direct = cs->ninputs == 1 && !ex->null_input && v[0] < COMBO_DIR_MAX;
	if (direct && v[0] < cs->dircap && cs->dir[v[0]])
		return cs->dir[v[0]] - 1;
	h = combo_hash(v, cs->ninputs, ex->null_input) & (cs->nht - 1);
	for (; (e = cs->ht[h]) != 0; h = (h + 1) & (cs->nht - 1))
		if (cs->null_input[e - 1] == ex->null_input &&
			(cs->ninputs == 1 ? cs->vid[e - 1] == v[0] :
			 memcmp(&cs->vid[(size_t) (e - 1) * cs->ninputs], v, cs->ninputs * sizeof(uint64)) == 0))
			return e - 1;
	combo_grow_ent(cs);
	if (cs->nent >= (uint32) 1 << 24)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: too many distinct value combinations for the aggregate scan"),
				 errhint("Set sift.enable_agg to off for this query.")));
	memcpy(&cs->vid[(size_t) cs->nent * cs->ninputs], v, cs->ninputs * sizeof(uint64));
	cs->null_input[cs->nent] = ex->null_input;
	combo_make(ex, ci, v, &cs->ent[cs->nent]);
	cs->ht[h] = ++cs->nent;
	if (direct)
		combo_dir_set(cs, v[0], cs->nent);
	if (cs->nent * 2 > cs->nht)
	{
		pfree(cs->ht);
		cs->nht *= 2;
		cs->ht = MemoryContextAllocZero(cs->cxt, cs->nht * sizeof(uint32));
		for (uint32 x = 0; x < cs->nent; x++)
		{
			h = combo_hash(&cs->vid[(size_t) x * cs->ninputs], cs->ninputs, cs->null_input[x]) & (cs->nht - 1);
			while (cs->ht[h])
				h = (h + 1) & (cs->nht - 1);
			cs->ht[h] = x + 1;
		}
	}
	return cs->nent - 1;
}

/* the cache starts again: the entries, the outputs of the slots, the values and the ids of the leaves (nothing else holds them) */
void
combo_reset(AggExec *ex, int ci)
{
	ComboSt    *cs = &ex->cs[ci];
	SiftAggCombo *cb = &ex->spec->combo[ci];
	MemoryContext old;

	MemoryContextReset(cs->cxt);
	cs->nent = cs->entcap = cs->nout = cs->outcap = cs->ndat = cs->datcap = 0;
	cs->vid = NULL;
	cs->null_input = NULL;
	cs->ent = NULL;
	cs->val = NULL;
	cs->maprow = NULL;
	cs->dat = NULL;
	cs->dnum = NULL;
	cs->arena = NULL;
	cs->arena_left = 0;
	cs->nht = 64;
	cs->dir = NULL;
	cs->dircap = 0;
	old = MemoryContextSwitchTo(cs->cxt);
	cs->ht = palloc0(cs->nht * sizeof(uint32));
	for (int p = 0; p < cb->nleaves; p++)
		if (cb->leaf[p] >= 0 && leaf_wide(&ex->lay->leaf[cb->leaf[p]]))
			id64_init(&cs->id64[p]);
	MemoryContextSwitchTo(old);
}

/* the id (1..) of the value of a 64-bit leaf at index i of the vector, 0 for NULL; a NaN of a numeric leaf is a value */
static uint64
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
 * eid[i] = the entry of the combo's leaf values at tuple i: an element's leaf is read at tidx, a row's at ridx. A tuple that is not
 * alive (alive: bit i; NULL: all) has none (COMBO_NONE): no value of it reaches PostgreSQL's evaluation.
 */
void
combo_resolve(AggExec *ex, int ci, int qlevel, int n, const uint32 *tidx, const uint32 *ridx, uint32 *eid, const uint64 *alive, const Tup *tuples)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	ComboSt    *cs = &ex->cs[ci];

	if (cs->resettable && cs->nent >= (uint32) sift_agg_combo_cache && !ex->cfree_ok[ci])
		combo_reset(ex, ci);
	for (int p = 0; p < SIFT_AGG_MAX_CLEAVES; p++)
	{
		uint64	   *vid = ex->cvid[p];

		if (p >= cb->nleaves)
		{
			memset(vid, 0, n * sizeof(uint64));
			continue;
		}
		else if (cb->leaf[p] < 0)
		{
			Assert(tuples != NULL);
			for (int i = 0; i < n; i++)
				vid[i] = (uint64) tuples->cout[cb->refcombo[p]][i] + 1;
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
				if (i + WIDE_AHEAD < n && leaf_wide(lf) && cs->id64[p].nslot > 4096)
					__builtin_prefetch(&cs->id64[p].tab[id64_hash(((const int64 *) v->data)[idx[i + WIDE_AHEAD]]) & (cs->id64[p].nslot - 1)]);
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
	for (int i = 0; i < n; i++)
	{
		uint64		v[SIFT_AGG_MAX_CLEAVES] = {0};

		for (int p = 0; p < cb->nleaves; p++)
			v[p] = ex->cvid[p][i];

		eid[i] = alive && !sift_colsrc_bit(alive, i) ? COMBO_NONE : combo_ent(ex, ci, v);
	}
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
