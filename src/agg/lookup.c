/*
 * agg/lookup.c - the load of a joined table into a lookup table, at scan start
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
#include "agg/stats.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

/* the id of the integer key value v among the values of d, 0 when it is not there; *at: the slot it is in or would go to */
static uint32
kd_int_find(const KeyDict *d, int64 v, uint32 *at)
{
	uint32		h;

	if (d->nslot == 0)
	{
		*at = 0;
		return 0;
	}
	h = murmurhash32((uint32) (v ^ (v >> 32))) & (d->nslot - 1);
	for (uint32 id; (id = d->slot[h]) != 0; h = (h + 1) & (d->nslot - 1))
		if (d->ival[id - 1] == v)
			return id;
	*at = h;
	return 0;
}

/* the id of the integer key value v, which is added to d (in cxt) when new */
static uint32
kd_int(KeyDict *d, int64 v, MemoryContext cxt)
{
	uint32		h,
				id;

	if (d->nslot == 0)
	{
		d->nslot = 64;
		d->cap = 32;
		d->slot = MemoryContextAllocZero(cxt, d->nslot * sizeof(uint32));
		d->ival = MemoryContextAlloc(cxt, d->cap * sizeof(int64));
	}
	if ((id = kd_int_find(d, v, &h)) != 0)
		return id;
	if (d->n == d->cap)
	{
		d->cap *= 2;
		d->ival = repalloc(d->ival, d->cap * sizeof(int64));
	}
	d->ival[d->n] = v;
	d->slot[h] = ++d->n;
	if (d->n * 2 > d->nslot)
	{
		pfree(d->slot);
		d->nslot *= 2;
		d->slot = MemoryContextAllocZero(cxt, d->nslot * sizeof(uint32));
		for (uint32 i = 1; i <= d->n; i++)
		{
			int64		x = d->ival[i - 1];

			for (h = murmurhash32((uint32) (x ^ (x >> 32))) & (d->nslot - 1); d->slot[h]; h = (h + 1) & (d->nslot - 1))
				;
			d->slot[h] = i;
		}
	}
	return d->n;
}

/* the same for a text key value */
static uint32
kd_text_find(const KeyDict *d, const char *b, uint32 len, uint32 hv, uint32 *at)
{
	uint32		h;

	if (d->nslot == 0)
	{
		*at = 0;
		return 0;
	}
	h = hv & (d->nslot - 1);
	for (uint32 id; (id = d->slot[h]) != 0; h = (h + 1) & (d->nslot - 1))
		if (d->hv[id - 1] == hv && d->len[id - 1] == len && memcmp(d->ptr[id - 1], b, len) == 0)
			return id;
	*at = h;
	return 0;
}

/* the id of a text key value, which is added to d (its bytes copied into cxt) when new */
static uint32
kd_text(KeyDict *d, const char *b, uint32 len, MemoryContext cxt)
{
	uint32		hv = hash_bytes((const unsigned char *) b, len),
				h,
				id;
	char	   *cp;

	if (d->nslot == 0)
	{
		d->nslot = 64;
		d->cap = 32;
		d->slot = MemoryContextAllocZero(cxt, d->nslot * sizeof(uint32));
		d->ptr = MemoryContextAlloc(cxt, d->cap * sizeof(char *));
		d->len = MemoryContextAlloc(cxt, d->cap * sizeof(uint32));
		d->hv = MemoryContextAlloc(cxt, d->cap * sizeof(uint32));
	}
	if ((id = kd_text_find(d, b, len, hv, &h)) != 0)
		return id;
	if (d->n == d->cap)
	{
		d->cap *= 2;
		d->ptr = repalloc(d->ptr, d->cap * sizeof(char *));
		d->len = repalloc(d->len, d->cap * sizeof(uint32));
		d->hv = repalloc(d->hv, d->cap * sizeof(uint32));
	}
	cp = MemoryContextAlloc(cxt, Max(len, 1));
	memcpy(cp, b, len);
	d->ptr[d->n] = cp;
	d->len[d->n] = len;
	d->hv[d->n] = hv;
	d->slot[h] = ++d->n;
	if (d->n * 2 > d->nslot)
	{
		pfree(d->slot);
		d->nslot *= 2;
		d->slot = MemoryContextAllocZero(cxt, d->nslot * sizeof(uint32));
		for (uint32 i = 1; i <= d->n; i++)
		{
			for (h = d->hv[i - 1] & (d->nslot - 1); d->slot[h]; h = (h + 1) & (d->nslot - 1))
				;
			d->slot[h] = i;
		}
	}
	return d->n;
}

/* room for one more row */
static void
map_grow(MapTab *m)
{
	uint32		cap = Max(m->cap * 2, 256);

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

	for (int k = 0; k < m->nkey; k++)
	{
		kv[k] = slot_getattr(slot, j->keyatt[k], &knull[k]);
		dead |= knull[k] && !m->knull[k];
	}
	if (dead && !m->keep)
		return;					/* NULL equals nothing */
	if (m->nrow == m->cap)
		map_grow(m);
	kb = (size_t) m->nrow * m->nkey;
	for (int k = 0; k < m->nkey; k++)
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
	for (int c = 0; c < m->ncol; c++)
	{
		MapCol	   *mc = &m->mcol[c];
		bool		cnull;
		Datum		d = slot_getattr(slot, j->colatt[c], &cnull);

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
	m->nrow++;
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
static Relation
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
static MapTab *
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
	return m->kd[0].n == m->nrow;
}

static inline uint32
map_hash_ids(int nkey, const uint32 *kid)
{
	uint64		h = UINT64CONST(0x9E3779B97F4A7C15);

	for (int k = 0; k < nkey; k++)
	{
		h = (h ^ kid[k]) * UINT64CONST(0xD6E8FEB86659FD93);
		h ^= h >> 32;
	}
	h *= UINT64CONST(0x9E3779B97F4A7C15);
	return (uint32) (h >> 32);
}

/* the order columns of the rows of a first-row lookup: the value of each (NULL: 0 and isnull) */
typedef struct FirstOrd
{
	MemoryContext cx;			/* where the values live, deleted once the rows are chosen */
	int64	   *val;
	bool	   *isnull;
	uint32		cap;
} FirstOrd;

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
static void
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
static void
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

/* the hash chains of the rows: the rows of a key are the chain of its slot (a row of the same key is another link: a join repeats a tuple per match); the arrays are cut to the rows */
static void
map_index_rows(MapTab *m)
{
	m->nslot = 64;
	while (m->nslot < m->nrow)
		m->nslot *= 2;
	m->head = MemoryContextAllocZero(m->cxt, m->nslot * sizeof(uint32));
	m->next = MemoryContextAlloc(m->cxt, Max(m->nrow, 1) * sizeof(uint32));
	m->hit = m->keep ? MemoryContextAllocZero(m->cxt, Max(m->nrow, 1)) : NULL;
	for (uint32 r = m->nrow; r-- > 0;)
	{
		const uint32 *kid = m->kid + (size_t) r * m->nkey;
		uint32		h;

		if (map_key_dead(m, kid))
			continue;
		h = map_hash_ids(m->nkey, kid) & (m->nslot - 1);
		m->next[r] = m->head[h];
		m->head[h] = r + 1;
	}
	if (m->cap > m->nrow && m->nrow > 0)
	{
		m->kid = repalloc(m->kid, (size_t) m->nrow * m->nkey * sizeof(uint32));
		for (int c = 0; c < m->ncol; c++)
		{
			MapCol	   *mc = &m->mcol[c];

			if (mc->byval)
			{
				mc->val = repalloc(mc->val, (size_t) m->nrow * sizeof(Datum));
				if (mc->isnull)
					mc->isnull = repalloc(mc->isnull, m->nrow);
			}
			else
				mc->id = repalloc(mc->id, (size_t) m->nrow * sizeof(uint32));
		}
		m->cap = m->nrow;
	}
	/* the tables that found the distinct column values are not needed once the rows are in */
	for (int c = 0; c < m->ncol; c++)
	{
		MapCol	   *mc = &m->mcol[c];

		if (mc->slot)
		{
			pfree(mc->slot);
			pfree(mc->hv);
			mc->slot = NULL;
			mc->hv = NULL;
			mc->nslot = 0;
		}
	}
}

static inline bool
map_ids_eq(const MapTab *m, uint32 row, const uint32 *kid)
{
	const uint32 *rk = m->kid + (size_t) row * m->nkey;

	for (int k = 0; k < m->nkey; k++)
		if (rk[k] != kid[k])
			return false;
	return true;
}

/* the rows whose key ids are kid; palloc'd in the current context, NULL when there are none */
int
map_find_ids(const MapTab *m, const uint32 *kid, uint32 **rows)
{
	uint32		h = map_hash_ids(m->nkey, kid) & (m->nslot - 1);
	uint32		first[8];
	int			n = 0;

	*rows = NULL;
	/* the first matches are collected on the stack: a key has a row or a few */
	for (uint32 r = m->head[h]; r != 0; r = m->next[r - 1])
		if (map_ids_eq(m, r - 1, kid))
		{
			if (n < 8)
				first[n] = r - 1;
			n++;
		}
	if (n == 0)
		return 0;
	*rows = palloc(n * sizeof(uint32));
	if (n <= 8)
		memcpy(*rows, first, n * sizeof(uint32));
	else
	{
		n = 0;
		for (uint32 r = m->head[h]; r != 0; r = m->next[r - 1])
			if (map_ids_eq(m, r - 1, kid))
				(*rows)[n++] = r - 1;
	}
	return n;
}

/* the id of the value of key k among the key's values in the table, 0 when the table has no row of it */
uint32
map_key_id(const MapTab *m, int k, const char *ptr, uint32 len, int64 iv)
{
	uint32		at;

	return m->ktext[k] ? kd_text_find(&m->kd[k], ptr, len, hash_bytes((const unsigned char *) ptr, len), &at) : kd_int_find(&m->kd[k], iv, &at);
}

/* the rows whose key equals the one given; palloc'd in the current context */
int
map_find(const MapTab *m, const char **ptr, const uint32 *len, const int64 *iv, uint32 **rows)
{
	uint32		kid[SIFT_AGG_MAX_JKEYS];

	*rows = NULL;
	if (m->nrow == 0)
		return 0;
	for (int k = 0; k < m->nkey; k++)
		if ((kid[k] = map_key_id(m, k, ptr[k], len[k], iv[k])) == 0)
			return 0;
	return map_find_ids(m, kid, rows);
}

/* EXPLAIN ANALYZE: the rows loaded and the time since t0 */
void
map_loaded(AggExec *ex, int nrows, instr_time t0)
{
	instr_time	t1;

	ex->lookup_loaded += nrows;
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_SUBTRACT(t1, t0);
	ex->lookup_us += INSTR_TIME_GET_MICROSEC(t1);
}

static char *shared_map_key(AggExec *ex, int ci, bool cols);

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

/* a table kept for the queries that follow (lookcache.c): where its rows are loaded, the pages it was loaded from and the columns to load */
typedef struct KeptTry
{
	char	   *key;
	LookFinger *before;
	MemoryContext cx;
	LookCols	cols;
} KeptTry;

/* the table as an arm reads it: its columns, in its order, of those the rows were loaded with */
static MapTab *
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
static MapTab *
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
static void
map_kept_keep(Relation rel, const KeptTry *t, MapTab *m, MemoryContext cxt)
{
	LookFinger *after;

	if (t->before != NULL && (after = lookcache_finger(rel)) != NULL)
		lookcache_keep(t->key, t->before, after, m, &t->cols, t->cx, cxt);
}

/* the rows of the joined table, visible to the scan's snapshot, hashed by key; from an index when its own conditions name a value of a btree, from the cache of an earlier query when the table did not change */
static MapTab *
map_load_table(AggExec *ex, int ci, MemoryContext cxt)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ExprState  *own = sift_agg_lookup_pushdown && j->fslot >= 0 && !sift_aggjoin_preserves(j->kind) && !slot_reads_leaf(cb, j->fslot) ?
		ex->cs[ci].slot[j->fslot] : NULL;
	MapTab	   *m;
	Relation	rel = table_open(j->relid, AccessShareLock);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	ScanKeyData keys[SIFT_AGG_MAX_JIDX];
	Datum		val[SIFT_AGG_MAX_JIDX];
	int			nkeys = 0;
	bool		none = false;
	Relation	idx;
	KeptTry		kept = {.cx = cxt};
	SiftAggJoin lj = *j;
	ExprState  *firstq = NULL;
	ExprContext *firstcx = NULL;
	FirstOrd	ord = {0};
	instr_time	t0;

	INSTR_TIME_SET_CURRENT(t0);
	for (int k = 0; sift_agg_lookup_pushdown && k < j->nidx && !none; k++)
	{
		bool		isnull;
		Oid			t = exprType(j->idxval[k]);
		Datum		d = ExecEvalExprSwitchContext(ExecInitExpr((Expr *) j->idxval[k], EXPR_PARENT(ex)), ex->lutcx, &isnull);

		none = isnull;			/* the equality is never true of a NULL: no row can match */
		val[k] = isnull ? (Datum) 0 : datumCopy(d, get_typbyval(t), get_typlen(t));
	}
	ResetExprContext(ex->lutcx);
	idx = sift_agg_lookup_pushdown && j->nidx > 0 && !none ? map_index(rel, j, val, keys, &nkeys) : NULL;
	if (idx == NULL && !none && (m = map_kept_find(ex, ci, rel, cxt, own != NULL, &kept)) != NULL)
	{
		MemoryContextSwitchTo(old);
		ExecDropSingleTupleTableSlot(slot);
		table_close(rel, NoLock);
		map_loaded(ex, 0, t0);
		return m;
	}
	/* a table kept for other arms too is loaded with the columns of them all */
	if (kept.cols.n > 0)
	{
		lj.ncol = kept.cols.n;
		for (int c = 0; c < lj.ncol; c++)
		{
			lj.colatt[c] = kept.cols.att[c];
			lj.coltype[c] = kept.cols.type[c];
		}
	}
	if (j->first.qual != NIL)
	{
		firstq = ExecInitQual(j->first.qual, NULL);
		firstcx = CreateStandaloneExprContext();
	}
	MemoryContextSwitchTo(kept.cx);
	m = map_new(&lj, own != NULL, kept.cx);
	if (idx)
	{
		IndexScanDesc iscan = sift_index_beginscan(rel, idx, ex->snap, nkeys);

		index_rescan(iscan, keys, nkeys, NULL, 0);
		while (index_getnext_slot(iscan, ForwardScanDirection, slot))
			map_row(ex, m, slot, &lj, cb, own);
		index_endscan(iscan);
		index_close(idx, AccessShareLock);
		ex->lookup_index++;
	}
	else if (!none)
	{
		/* the heap tests `column = value` on the page, before it makes a tuple of a row; another access method ignores the keys, the filter holds them too */
		bool		heap = rel->rd_tableam == GetHeapamTableAmRoutine();
		TableScanDesc scan;

		for (int k = 0; heap && sift_agg_lookup_pushdown && k < j->nidx; k++)
			if (!j->idxany[k])
				ScanKeyEntryInitialize(&keys[nkeys++], 0, j->idxatt[k], BTEqualStrategyNumber, InvalidOid, j->idxcoll[k], get_opcode(j->idxop[k]), val[k]);
		scan = sift_table_beginscan(rel, ex->snap, nkeys, keys);

		/* a first-row lookup of a child that tests more than its key: the rows it would not return are not loaded */
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			bool		pass = true;

			if (firstq)
			{
				firstcx->ecxt_scantuple = slot;
				pass = ExecQual(firstq, firstcx);
				ResetExprContext(firstcx);
			}
			if (pass)
			{
				uint32		n = m->nrow;

				map_row(ex, m, slot, &lj, cb, own);
				if (j->first.nord > 0 && m->nrow > n)
					first_order_row(m, &j->first, &ord, slot);
			}
		}
		table_endscan(scan);
	}
	if (ord.cx)
	{
		map_keep_first(m, &j->first, &ord);
		MemoryContextDelete(ord.cx);
	}
	map_index_rows(m);
	if (firstcx)
		FreeExprContext(firstcx, true);
	MemoryContextSwitchTo(old);
	map_kept_keep(rel, &kept, m, cxt);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, NoLock);
	map_loaded(ex, m->nrow, t0);
	return kept.cols.n > 0 ? map_view(m, &kept.cols, j, cxt) : m;
}

/* the rows of a child plan (a sub-select, VALUES list or function), run once; the rows are the plan's, in the query's snapshot */
static MapTab *
map_load_child(AggExec *ex, int ci)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ExprState  *own = sift_agg_lookup_pushdown && j->fslot >= 0 && !sift_aggjoin_preserves(j->kind) && !slot_reads_leaf(cb, j->fslot) ?
		ex->cs[ci].slot[j->fslot] : NULL;
	PlanState  *child = j->subplan > 0 ? list_nth(ex->css.ss.ps.state->es_subplanstates, j->subplan - 1) : list_nth(ex->css.custom_ps, j->child);
	MemoryContext caller = MemoryContextSwitchTo(ex->cxt);
	MapTab	   *m = map_new(j, own != NULL, ex->cxt);
	instr_time	t0;

	INSTR_TIME_SET_CURRENT(t0);
	for (;;)
	{
		TupleTableSlot *slot;

		MemoryContextSwitchTo(caller);
		slot = ExecProcNode(child);
		MemoryContextSwitchTo(ex->cxt);
		if (TupIsNull(slot))
			break;
		map_row(ex, m, slot, j, cb, own);
	}
	map_index_rows(m);
	MemoryContextSwitchTo(caller);
	map_loaded(ex, m->nrow, t0);
	return m;
}

/*
 * The arms of one statement (UNION ALL, an inlined CTE read several times) that join the same table with the same columns and own conditions read
 * the same rows in the same snapshot: the first arm loads them into a context of the statement and the others use that table, which no scan changes.
 * A table is shared within one EState only; the entry goes with the context (a parallel worker has no EState and loads its own).
 */
typedef struct SharedMap
{
	EState	   *estate;
	char	   *key;
	MapTab	   *map;
	MemoryContextCallback cb;
} SharedMap;

static List *shared_maps = NIL;

static void
shared_map_forget(void *arg)
{
	shared_maps = list_delete_ptr(shared_maps, arg);
}

/* what decides the rows of a table: the relation, its key and (cols) output columns and the conditions that narrow the load */
static char *
shared_map_key(AggExec *ex, int ci, bool cols)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	bool		own = sift_agg_lookup_pushdown && j->fslot >= 0 && !slot_reads_leaf(cb, j->fslot);
	StringInfoData k;

	initStringInfo(&k);
	/* an own condition reads the table's columns as Params nleaves + i: the same text over another number of leaves is another condition */
	appendStringInfo(&k, "%u %d %d %d %d|", j->relid, own, sift_agg_lookup_pushdown, own ? cb->nleaves : 0, j->nkey);
	for (int i = 0; i < j->nkey; i++)
		appendStringInfo(&k, "%d/%u/%d ", j->keyatt[i], j->keytype[i], j->keynull[i]);
	if (cols || own)
	{
		appendStringInfo(&k, "|%d|", j->ncol);
		for (int i = 0; i < j->ncol; i++)
			appendStringInfo(&k, "%d/%u ", j->colatt[i], j->coltype[i]);
	}
	if (own)
		appendStringInfo(&k, "|%s", nodeToString(cb->slot[j->fslot].expr));
	if (j->first.qual != NIL)
		appendStringInfo(&k, "|first %s", nodeToString(j->first.qual));
	for (int i = 0; i < j->first.nord; i++)
		appendStringInfo(&k, "|ord %d/%d/%d", j->first.ordatt[i], j->first.orddesc[i], j->first.ordnullsfirst[i]);
	for (int i = 0; sift_agg_lookup_pushdown && i < j->nidx; i++)
		appendStringInfo(&k, "|%d/%u/%u/%d/%s", j->idxatt[i], j->idxop[i], j->idxcoll[i], j->idxany[i], nodeToString(j->idxval[i]));
	return k.data;
}

static MapTab *
map_load_shared(AggExec *ex, int ci)
{
	EState	   *estate = ex->css.ss.ps.state;
	char	   *key;
	SharedMap  *sm;
	MemoryContext mcxt;
	MemoryContext old;

	if (estate == NULL)
		return map_load_table(ex, ci, ex->cxt);
	key = shared_map_key(ex, ci, true);
	foreach_ptr(SharedMap, e, shared_maps)
		if (e->estate == estate && strcmp(e->key, key) == 0)
		{
			ex->nshared++;
			return e->map;
		}
	mcxt = AllocSetContextCreate(estate->es_query_cxt, "sift agg shared lookup", ALLOCSET_DEFAULT_SIZES);
	sm = MemoryContextAlloc(mcxt, sizeof(SharedMap));
	sm->estate = estate;
	sm->key = MemoryContextStrdup(mcxt, key);
	sm->map = map_load_table(ex, ci, mcxt);
	sm->cb.func = shared_map_forget;
	sm->cb.arg = sm;
	MemoryContextRegisterResetCallback(mcxt, &sm->cb);
	old = MemoryContextSwitchTo(TopMemoryContext);
	shared_maps = lappend(shared_maps, sm);
	MemoryContextSwitchTo(old);
	return sm->map;
}

/* the rows of the joined table or plan, hashed by key */
MapTab *
map_load(AggExec *ex, int ci)
{
	const SiftAggJoin *j = &ex->spec->join[ex->spec->combo[ci].join];

	if (ex->pmaps && ex->pmaps[ci])
		return map_share_attach(ex->pmaps[ci], j, ex->cxt);
	if (j->params != NIL)
		return map_new(j, false, ex->cxt);
	if (j->child >= 0)
		return map_load_child(ex, ci);
	return sift_agg_share_lookups && !sift_aggjoin_preserves(j->kind) ? map_load_shared(ex, ci) : map_load_table(ex, ci, ex->cxt);
}

/*
 * A semi or anti join on the integer leaf of the rows (`col IN (SELECT k FROM t)`, EXISTS, NOT EXISTS) that gives no column and keeps no
 * other condition is a test of the leaf against the keys of the table: a byte table over their range, built once, answers it for every
 * tuple, where the generic path makes a cache entry per distinct value of the leaf (a hash of the table's rows each).
 */
/* does an expression read one of the first n parameters (the leaves of a combo)? */
static bool
reads_leaf_param(Node *n, int *nleaves)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid < *nleaves)
		return true;
	return expression_tree_walker(n, reads_leaf_param, nleaves);
}

/* does the expression of a slot of the combo read one of its leaves? */
bool
slot_reads_leaf(const SiftAggCombo *cb, int sl)
{
	int			nleaves = cb->nleaves;

	return reads_leaf_param((Node *) cb->slot[sl].expr, &nleaves);
}
