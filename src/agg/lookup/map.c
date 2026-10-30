/*
 * agg/lookup/map.c - the lookup table of a joined table: the map and its build
 * The growth of the map, the add of a row by its key ids, the binding to a column and the
 * index over the rows of a unique map.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "agg/wide.h"
#include "agg/lookup/int.h"

/* room for one more row */
void
map_grow(MapTab *m)
{
	uint32		cap = Max(m->cap * 2, 256);

	if (m->lazy)
		m->rkey = m->rkey ? repalloc(m->rkey, (size_t) cap * sizeof(int64)) : MemoryContextAlloc(m->cxt, (size_t) cap * sizeof(int64));
	else
		m->kid = m->kid ? repalloc(m->kid, (size_t) cap * m->nkey * sizeof(uint32)) : MemoryContextAlloc(m->cxt, (size_t) cap * m->nkey * sizeof(uint32));
	for (int c = 0; c < m->ncol; c++)
	{
		MapCol	   *mc = &m->mcol[c];

		if (mc->byval)
		{
			mc->val = mc->val ? repalloc(mc->val, (size_t) cap * sizeof(Datum)) : MemoryContextAlloc(m->cxt, (size_t) cap * sizeof(Datum));
			if (mc->isnull)
			{
				mc->isnull = repalloc(mc->isnull, cap);
				memset(mc->isnull + m->cap, 0, cap - m->cap);
			}
		}
		else
			mc->id = mc->id ? repalloc(mc->id, (size_t) cap * sizeof(uint32)) : MemoryContextAlloc(m->cxt, (size_t) cap * sizeof(uint32));
	}
	m->cap = cap;
}

/* the id of a by-reference column value among the distinct ones of the column: d is a palloc'd copy, kept when new and freed when not */
static uint32
col_ref_id(MapTab *m, MapCol *mc, Datum d)
{
	Size		sz = datumGetSize(d, false, mc->len);
	uint32		hv = hash_bytes((const unsigned char *) DatumGetPointer(d), sz),
				h;

	if (mc->nslot == 0)
	{
		mc->nslot = 64;
		mc->cap = 32;
		mc->slot = MemoryContextAllocZero(m->cxt, mc->nslot * sizeof(uint32));
		mc->val = MemoryContextAlloc(m->cxt, mc->cap * sizeof(Datum));
		mc->hv = MemoryContextAlloc(m->cxt, mc->cap * sizeof(uint32));
	}
	h = hv & (mc->nslot - 1);
	for (uint32 id; (id = mc->slot[h]) != 0; h = (h + 1) & (mc->nslot - 1))
		if (mc->hv[id - 1] == hv && datumGetSize(mc->val[id - 1], false, mc->len) == sz &&
			memcmp(DatumGetPointer(mc->val[id - 1]), DatumGetPointer(d), sz) == 0)
		{
			pfree(DatumGetPointer(d));
			return id;
		}
	if (mc->n == mc->cap)
	{
		mc->cap *= 2;
		mc->val = repalloc(mc->val, mc->cap * sizeof(Datum));
		mc->hv = repalloc(mc->hv, mc->cap * sizeof(uint32));
	}
	mc->val[mc->n] = d;
	mc->hv[mc->n] = hv;
	mc->slot[h] = ++mc->n;
	if (mc->n * 2 > mc->nslot)
	{
		pfree(mc->slot);
		mc->nslot *= 2;
		mc->slot = MemoryContextAllocZero(m->cxt, mc->nslot * sizeof(uint32));
		for (uint32 id = 1; id <= mc->n; id++)
		{
			for (h = mc->hv[id - 1] & (mc->nslot - 1); mc->slot[h]; h = (h + 1) & (mc->nslot - 1))
				;
			mc->slot[h] = id;
		}
	}
	return mc->n;
}

static void
map_add(AggExec *ex, MapTab *m, TupleTableSlot *slot, const SiftAggJoin *j)
{
	Datum		kv[SIFT_AGG_MAX_JKEYS];
	bool		knull[SIFT_AGG_MAX_JKEYS];
	bool		dead = false;
	size_t		kb;

	if (m->kcx)
		m->kcx->ecxt_scantuple = slot;
	for (int k = 0; k < m->nkey; k++)
	{
		kv[k] = m->kx[k] ? ExecEvalExprSwitchContext(m->kx[k], m->kcx, &knull[k]) : slot_getattr(slot, j->keyatt[k], &knull[k]);
		dead |= knull[k] && !m->knull[k];
	}
	if (dead && !m->keep)
		return;					/* NULL equals nothing */
	if (m->nrow == m->cap)
		map_grow(m);
	kb = (size_t) m->nrow * m->nkey;
	for (int k = 0; k < m->nkey && !m->lazy; k++)
	{
		if (knull[k])
			m->kid[kb + k] = 0;
		else if (m->ktext[k])
		{
			struct varlena *v = PG_DETOAST_DATUM_PACKED(kv[k]);

			m->kid[kb + k] = kd_text(&m->kd[k], VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v), m->cxt);
			if ((Pointer) v != DatumGetPointer(kv[k]))
				pfree(v);
		}
		else
			m->kid[kb + k] = kd_int(&m->kd[k], sift_aggspec_operand_int(j->keytype[k], kv[k]), m->cxt);
	}
	if (m->lazy)
		m->rkey[m->nrow] = sift_aggspec_operand_int(j->keytype[0], kv[0]);
	if (m->kcx)
		ResetExprContext(m->kcx);
	for (int c = 0; c < m->ncol; c++)
	{
		bool		cnull;
		Datum		d = slot_getattr(slot, j->colatt[c], &cnull);

		map_col_put(m, c, d, cnull);
	}
	m->nrow++;
}

/* the value of column c of the row being added (row m->nrow) */
void
map_col_put(MapTab *m, int c, Datum d, bool cnull)
{
	MapCol	   *mc = &m->mcol[c];

	if (mc->byval)
	{
		mc->val[m->nrow] = cnull ? (Datum) 0 : d;
		if (cnull && mc->isnull == NULL)
			mc->isnull = MemoryContextAllocZero(m->cxt, m->cap);
		if (mc->isnull)
			mc->isnull[m->nrow] = cnull;
	}
	else if (cnull)
		mc->id[m->nrow] = 0;
	else
		mc->id[m->nrow] = col_ref_id(m, mc, mc->len == -1 ? PointerGetDatum(PG_DETOAST_DATUM_COPY(d)) : datumCopy(d, false, mc->len));
}

void
map_bind(AggExec *ex, int param, const MapTab *m, uint32 row, int c)
{
	bool		isnull;
	Datum		d = map_col(m, row, c, &isnull);

	bind_param(ex, param, d, isnull);
}

/* a row the scan of the table returned: into the table when it holds the ON clause's own conditions (`own`, when they read no leaf: a row that fails them matches no tuple) */
void
map_row(AggExec *ex, MapTab *m, TupleTableSlot *slot, const SiftAggJoin *j, const SiftAggCombo *cb, ExprState *own)
{
	ex->lookup_read++;
	if (own)
	{
		bool		isnull;
		Datum		d;

		for (int c = 0; c < j->ncol; c++)
		{
			bool		cnull;
			Datum		v = slot_getattr(slot, j->colatt[c], &cnull);

			bind_param(ex, cb->nleaves + c, v, cnull);
		}
		d = ExecEvalExprSwitchContext(own, ex->lutcx, &isnull);
		if ((ex->lookup_read & 1023) == 0)
			ResetExprContext(ex->lutcx);
		if (isnull || !DatumGetBool(d))
			return;
	}
	map_add(ex, m, slot, j);
}

/*
 * The btree of the joined table that starts with columns the table's `column = value` and `column = ANY (array)` conditions give (the planner found them, join_index_quals),
 * opened, with the keys of its scan: the rows that can match are those it returns. NULL when no index fits; *none when a value is NULL (no row).
 */
Relation
map_index(Relation rel, const SiftAggJoin *j, const Datum *val, ScanKeyData *keys, int *nkeys)
{
	Relation	best = NULL;
	int			nbest = 0;

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		ScanKeyData ik[SIFT_AGG_MAX_JIDX];
		int			n = 0;

		if (idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index->indisvalid && heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL))
			for (; n < idx->rd_index->indnkeyatts && n < SIFT_AGG_MAX_JIDX; n++)
			{
				int			k;
				int			strategy;
				Oid			lt,
							rt;

				for (k = 0; k < j->nidx; k++)
					if (j->idxatt[k] == idx->rd_index->indkey.values[n] && op_in_opfamily(j->idxop[k], idx->rd_opfamily[n]) &&
						idx->rd_indcollation[n] == j->idxcoll[k])
						break;
				if (k == j->nidx)
					break;
				get_op_opfamily_properties(j->idxop[k], idx->rd_opfamily[n], false, &strategy, &lt, &rt);
				if (strategy != BTEqualStrategyNumber)
					break;
				ScanKeyEntryInitialize(&ik[n], j->idxany[k] ? SK_SEARCHARRAY : 0, n + 1, strategy, rt, j->idxcoll[k], get_opcode(j->idxop[k]), val[k]);
			}
		if (n > nbest)
		{
			if (best)
				index_close(best, AccessShareLock);
			best = idx;
			nbest = n;
			memcpy(keys, ik, n * sizeof(ScanKeyData));
		}
		else
			index_close(idx, AccessShareLock);
	}
	*nkeys = nbest;
	return best;
}

/* an empty table of the rows of a join; filtered: only rows that hold the ON clause's own conditions are loaded (map_row) */
MapTab *
map_new(const SiftAggJoin *j, bool filtered, MemoryContext cxt)
{
	MapTab	   *m = palloc0(sizeof(MapTab));

	m->cxt = cxt;
	m->nkey = j->nkey;
	m->ncol = j->ncol;
	m->filtered = filtered;
	m->keep = sift_aggjoin_preserves(j->kind);
	m->mcol = MemoryContextAllocZero(cxt, Max(j->ncol, 1) * sizeof(MapCol));
	for (int k = 0; k < j->nkey; k++)
	{
		m->ktext[k] = j->keytype[k] == TEXTOID || j->keytype[k] == VARCHAROID;
		m->knull[k] = j->keynull[k];
	}
	for (int c = 0; c < j->ncol; c++)
		get_typlenbyval(j->coltype[c], &m->mcol[c].len, &m->mcol[c].byval);
	return m;
}

/* does no key repeat among the rows of a table with one key? (every row of such a table has a value of its own) */
bool
map_unique(const MapTab *m)
{
	map_ensure((MapTab *) m);
	return m->kd[0].n == m->nrow;
}

/*
 * A semi or anti join on the integer leaf of the rows (`col IN (SELECT k FROM t)`, EXISTS, NOT EXISTS) that gives no column and keeps no
 * other condition is a test of the leaf against the keys of the table: a byte table over their range, built once, answers it for every
 * tuple, where the generic path makes a cache entry per distinct value of the leaf (a hash of the table's rows each).
 */

