/*
 * agg/lookclass.c - a lookup table as classes of its keys, read from the leaves of one of its sift_col indexes
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "common/hashfn.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/agg_int.h"
#include "agg/leaf_int.h"
#include "agg/lookclass_int.h"
#include "agg/plan_int.h"
#include "agg/wide.h"
#include "core/hash.h"

/*
 * A table of one int4 key whose other columns take few distinct tuples of values (a flag, a small code, a mask of what is present) is not
 * kept row by row: each key is given the class of its tuple, a table over the keys, and the map holds one row for each class, the first
 * it met. A join resolves a tuple by its key's class (combo/resolve.c), so the work of a row of the scan is an array read where a hash
 * lookup, a cache entry and an evaluation of the expressions were made for each distinct key. Exact: the columns of a key are those of its
 * class, a key twice in the table (a join that repeats rows) is not a class and keeps the table whole.
 */
static uint32
strids_find(StrIds *s, const char *str, uint32 len, uint32 h, uint32 *where)
{
	uint32		p;

	for (p = h & (s->nslot - 1); s->slot[p] != 0; p = (p + 1) & (s->nslot - 1))
	{
		const char *b = s->buf + s->off[s->slot[p] - 1];
		uint32		blen;

		memcpy(&blen, b, sizeof(blen));
		if (blen == len && memcmp(b + sizeof(blen), str, len) == 0)
			return s->slot[p];
	}
	*where = p;
	return 0;
}

/* the id (1..) of the string, made on its first sight */
uint32
strids_id(StrIds *s, const char *str, uint32 len)
{
	uint32		h = (uint32) (sift_fib64(hash_bytes((const unsigned char *) str, len)) >> 32),
				where,
				id;

	if (s->nslot == 0)
	{
		s->nslot = 1024;
		/* check-memory: allow the strings of the classes: at most DENSE_MAX_CLASSES of each text column, a load that has more stops */
		s->slot = MemoryContextAllocZero(s->cxt, s->nslot * sizeof(uint32));
	}
	if ((id = strids_find(s, str, len, h, &where)) != 0)
		return id;
	if (s->n * 2 >= s->nslot)
	{
		uint32		nslot = s->nslot * 2;
		/* check-memory: allow as above */
		uint32	   *slot = MemoryContextAllocZero(s->cxt, nslot * sizeof(uint32));

		for (uint32 i = 0; i < s->n; i++)
		{
			const char *b = s->buf + s->off[i];
			uint32		blen,
						p;

			memcpy(&blen, b, sizeof(blen));
			p = (uint32) (sift_fib64(hash_bytes((const unsigned char *) b + sizeof(blen), blen)) >> 32) & (nslot - 1);
			while (slot[p] != 0)
				p = (p + 1) & (nslot - 1);
			slot[p] = i + 1;
		}
		pfree(s->slot);
		s->slot = slot;
		s->nslot = nslot;
		strids_find(s, str, len, h, &where);
	}
	if (s->len + sizeof(len) + len > s->cap)
	{
		s->cap = Max(s->cap * 2, Max(4096, s->len + sizeof(len) + len));
		s->buf = s->buf ? repalloc(s->buf, s->cap) : MemoryContextAlloc(s->cxt, s->cap);
	}
	if (s->n == s->ocap)
	{
		s->ocap = Max(s->ocap * 2, 64);
		s->off = s->off ? repalloc(s->off, s->ocap * sizeof(uint32)) : MemoryContextAlloc(s->cxt, s->ocap * sizeof(uint32));
	}
	s->off[s->n] = (uint32) s->len;
	memcpy(s->buf + s->len, &len, sizeof(len));
	memcpy(s->buf + s->len + sizeof(len), str, len);
	s->len += sizeof(len) + len;
	s->slot[where] = ++s->n;
	return s->n;
}

/* the string of an id as text, or, for a wide leaf, as the datum of its type (the entry is the datum) */
static Datum
strids_datum(const StrIds *s, uint32 id, const SiftSrcLeaf *lf)
{
	const char *b = s->buf + s->off[id - 1];
	uint32		len;

	memcpy(&len, b, sizeof(len));
	if (lf->wide)
		return sift_aggwide_datum(lf->typid, memcpy(palloc(len), b + sizeof(len), len), len);
	return PointerGetDatum(cstring_to_text_with_len(b + sizeof(len), len));
}

/* the value of a cell that tells classes apart: the integer, the bits of a float, or the id of a string */
static inline int64
dense_value(DenseLoad *d, ColNeed *nd, int64 i)
{
	uint32		c;

	if (nd->lf->vtype != SCV_CODE)
		return lookcol_scalar_value(nd, i);
	c = ((const uint32 *) nd->vec.data)[i];
	if (nd->dids[c] == 0)
	{
		char		buf[SIFT_SRC_DICT_ENTRY];
		uint32		len;
		const char *str = sift_srcdict_str(nd->vec.dict, c, &len, buf);

		nd->dids[c] = strids_id(&d->strs, str, len);
	}
	return nd->dids[c];
}

/* the Datum of the value of column c of a class */
static Datum
dense_datum(const DenseLoad *d, const ColNeed *nd, int64 tv)
{
	if (nd == NULL)
		return BoolGetDatum(tv);
	return nd->lf->vtype == SCV_CODE ? strids_datum(&d->strs, (uint32) tv, nd->lf) : lookcol_scalar_datum(nd->lf, tv);
}

/* the class (1..) of the tuple, made on its first sight; 0 when there are too many. pos: where its row lies, kept for a shared load */
uint32
dense_intern(DenseLoad *d, const int64 *tv, uint32 nulls, int32 key, uint64 pos)
{
	uint64		h = SIFT_FIB64;
	uint32		s;

	for (int c = 0; c < d->ncol; c++)
	{
		h = sift_fold64(h, (uint64) tv[c]);
		h ^= h >> 32;
	}
	h = sift_fib64(h ^ nulls);
	for (s = (uint32) (h >> 32) & (DENSE_SLOTS - 1); d->ctab[s] != 0; s = (s + 1) & (DENSE_SLOTS - 1))
	{
		uint32		k = d->ctab[s] - 1;

		if (d->tnull[k] == nulls && memcmp(&d->tv[(size_t) k * d->ncol], tv, d->ncol * sizeof(int64)) == 0)
			return k + 1;
	}
	if (d->ncls == DENSE_MAX_CLASSES)
		return 0;
	memcpy(&d->tv[(size_t) d->ncls * d->ncol], tv, d->ncol * sizeof(int64));
	d->tnull[d->ncls] = nulls;
	d->ckey[d->ncls] = key;
	if (d->cpos != NULL)
		d->cpos[d->ncls] = pos;
	d->ctab[s] = ++d->ncls;
	return d->ncls;
}

/* the class of the tuple of row i, 0 when there are too many */
static uint32
dense_class(DenseLoad *d, ColNeed *need, int64 i, int32 key)
{
	int64		tv[SIFT_AGG_MAX_JCOLS];
	uint32		nulls = 0;
	int			k = 0;

	for (int c = 0; c < d->ncol; c++)
	{
		ColNeed    *nd = d->col[c] >= 0 ? &need[d->col[c]] : NULL;

		if (nd == NULL ? d->cvnull[k] : nd->vec.isnull != NULL && nd->vec.isnull[i])
		{
			nulls |= 1U << c;
			tv[c] = 0;
		}
		else
			tv[c] = nd == NULL ? d->cv[k] : dense_value(d, nd, i);
		k += nd == NULL;
	}
	return dense_intern(d, tv, nulls, key, d->upos | (uint64) i);
}

/* the tables of the classes: DENSE_MAX_CLASSES of them at most, and the positions of their first rows for a shared load */
void
dense_tables(DenseLoad *d, bool positions)
{
	d->tv = palloc((Size) DENSE_MAX_CLASSES * Max(d->ncol, 1) * sizeof(int64));
	d->tnull = palloc(DENSE_MAX_CLASSES * sizeof(uint32));
	d->ckey = palloc(DENSE_MAX_CLASSES * sizeof(int32));
	d->ctab = palloc0(DENSE_SLOTS * sizeof(uint16));
	d->cpos = positions ? palloc(DENSE_MAX_CLASSES * sizeof(uint64)) : NULL;
}

/*
 * The computed tests are a function of the columns they read alone (they are immutable, join_test_expr), so the outcomes of a tuple of those columns' values are
 * found once and kept: a test over a few columns takes a handful of tuples, where the expression was evaluated for each row of a table of millions. A text column is
 * known by the code of its unit's dictionary, which means another string in the next unit: the outcomes of such a table are kept for one unit.
 */
#define MEMO_MAX	(1 << 16)

struct CompMemo
{
	int			n;				/* the columns the tests read, in need[] at idx[] */
	int			idx[MAX_NEED];
	bool		perunit;
	uint32		nent;
	uint32	   *slot;			/* the entry in a hash slot (1..), 0 free */
	int64	   *val;			/* [entry * n + u] */
	uint32	   *nulls;			/* per entry: bit u set when column u is NULL */
	uint32	   *out;			/* per entry: bit k the outcome k of the tests, bit 16 + k when it is NULL */
};

static CompMemo *
comp_memo_make(const CompCol *comp, int ncomp, const ColNeed *need, int nneed)
{
	CompMemo   *m = palloc0(sizeof(CompMemo));

	for (int k = 0; k < ncomp; k++)
		foreach_ptr(Var, u, pull_var_clause(comp[k].expr, 0))
		{
			int			p = 0,
						x = 0;

			while (p < nneed && !(need[p].expr == NULL && need[p].att == u->varattno))
				p++;
			while (x < m->n && m->idx[x] != p)
				x++;
			if (p < nneed && x == m->n)
			{
				m->idx[m->n++] = p;
				m->perunit |= need[p].lf->vtype == SCV_CODE;
			}
		}
	/* check-memory: allow the entries are bounded by MEMO_MAX */
	m->slot = palloc0(2 * MEMO_MAX * sizeof(uint32));
	m->val = palloc(MEMO_MAX * Max(m->n, 1) * sizeof(int64));
	m->nulls = palloc(MEMO_MAX * sizeof(uint32));
	m->out = palloc(MEMO_MAX * sizeof(uint32));
	return m;
}

/* the outcome of each test over row i: from the memo when its columns' values were met, else evaluated over the slot, which gets the columns the tests read */
static void
dense_comp(DenseLoad *d, ColNeed *need, int nneed, int64 i)
{
	CompMemo   *m = d->memo;
	int64		tv[MAX_NEED];
	uint32		nulls = 0,
				s;
	uint64		h = SIFT_FIB64;

	for (int u = 0; u < m->n; u++)
	{
		ColNeed    *nd = &need[m->idx[u]];

		if (nd->vec.isnull != NULL && nd->vec.isnull[i])
		{
			nulls |= 1U << u;
			tv[u] = 0;
		}
		else
			tv[u] = nd->lf->vtype == SCV_CODE ? ((const uint32 *) nd->vec.data)[i] : lookcol_scalar_value(nd, i);
		h = sift_fold64(h, (uint64) tv[u]);
		h ^= h >> 32;
	}
	h = sift_fib64(h ^ nulls);
	for (s = (uint32) (h >> 32) & (2 * MEMO_MAX - 1); m->slot[s] != 0; s = (s + 1) & (2 * MEMO_MAX - 1))
	{
		uint32		e = m->slot[s] - 1;

		if (m->nulls[e] == nulls && memcmp(&m->val[(size_t) e * m->n], tv, m->n * sizeof(int64)) == 0)
		{
			for (int k = 0; k < d->ncomp; k++)
			{
				d->cv[k] = (m->out[e] >> k) & 1;
				d->cvnull[k] = (m->out[e] >> (16 + k)) & 1;
			}
			return;
		}
	}
	for (int u = 0; u < m->n; u++)
	{
		const ColNeed *nd = &need[m->idx[u]];

		d->slot->tts_values[nd->att - 1] = lookcol_cell_datum((ColNeed *) nd, i, &d->slot->tts_isnull[nd->att - 1]);
	}
	ResetExprContext(d->ectx);
	d->ectx->ecxt_scantuple = d->slot;
	for (int k = 0; k < d->ncomp; k++)
		d->cv[k] = DatumGetBool(ExecEvalExprSwitchContext(d->comp[k].st, d->ectx, &d->cvnull[k]));
	if (m->nent < MEMO_MAX)
	{
		uint32		out = 0;

		for (int k = 0; k < d->ncomp; k++)
			out |= (uint32) (d->cv[k] != 0) << k | (uint32) d->cvnull[k] << (16 + k);
		memcpy(&m->val[(size_t) m->nent * m->n], tv, m->n * sizeof(int64));
		m->nulls[m->nent] = nulls;
		m->out[m->nent] = out;
		m->slot[s] = ++m->nent;
	}
}

/* the key gets its class; false when it has one already (a key twice) */
static bool
dense_store(DenseLoad *d, uint32 key, uint32 cls)
{
	if (d->shared)
	{
		uint16		none = 0;

		if (key >= d->cap)
		{
			d->over = true;
			return false;
		}
		return __atomic_compare_exchange_n(&d->lut[key], &none, (uint16) (d->idbase + cls), false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
	}
	if (key >= d->cap)
	{
		uint32		cap = Max(d->cap * 2, 1U << 16);

		while (cap <= key)
			cap *= 2;
		d->lut = d->lut ? repalloc_huge(d->lut, (Size) cap * sizeof(uint16)) : MemoryContextAllocHuge(d->cxt, (Size) cap * sizeof(uint16));
		memset(d->lut + d->cap, 0, (Size) (cap - d->cap) * sizeof(uint16));
		d->cap = cap;
	}
	if (d->lut[key] != 0)
		return false;
	d->lut[key] = cls;
	return true;
}

void
dense_unit(void *arg, const SiftSrcUnit *unit, ColNeed *need, int nneed, uint64 *rows)
{
	DenseLoad  *d = arg;
	const ColNeed *kn = &need[d->key];
	int			nw = (unit->nrow + 63) / 64;

	if (unit->sb != d->lastsb)
	{
		d->lastsb = unit->sb;
		d->ord = 0;
	}
	else
		d->ord++;
	d->upos = (uint64) unit->sb << 40 | (uint64) d->ord << 20;
	if (d->memo != NULL && d->memo->perunit)
	{
		memset(d->memo->slot, 0, 2 * MEMO_MAX * sizeof(uint32));
		d->memo->nent = 0;
	}
	for (int w = 0; w < nw && !d->bad; w++)
		for (uint64 bits = rows[w]; bits != 0 && !d->bad; bits &= bits - 1)
		{
			int64		i = (int64) w * 64 + __builtin_ctzll(bits);
			int32		key;
			uint32		cls;

			if (kn->vec.isnull != NULL && kn->vec.isnull[i])
				continue;
			key = ((const int32 *) kn->vec.data)[i];
			if (d->ncomp > 0 && key >= 0)
				dense_comp(d, need, nneed, i);
			if (key < 0 || (uint32) key >= d->limit || (cls = dense_class(d, need, i, key)) == 0 || !dense_store(d, key, cls))
			{
				d->bad = true;
				break;
			}
			d->n = Max(d->n, (uint32) key + 1);
			d->rows++;
		}
}

/* is the leaf of the key an int4? The columns may be of any type the load decodes: the integers, a float's bits, a numeric's scaled integer, a string's id */
static bool
dense_leaves_ok(const ColNeed *need, int key, const CompCol *comp, int ncomp)
{
	/* a computed column is kept as the outcome of a test (true, false, NULL) */
	for (int k = 0; k < ncomp; k++)
		if (exprType(comp[k].expr) != BOOLOID)
			return false;
	return need[key].lf->vtype == SCV_I32 && need[key].lf->typid == INT4OID;
}

/*
 * The index of the table (open, the caller closes it) that holds the key and the columns of the join as integer leaves (map_load_dense), else NULL. bound[c], when
 * given, is how many values the index says column c of the join takes (its range and its NULLs: the index knows them, whatever the statistics of the table or of an
 * expression are), 0 when it cannot tell.
 */
Relation
map_dense_index(Relation rel, const SiftAggJoin *j, double *bound, Relation *ix2)
{
	ColNeed		need[MAX_NEED];
	CompCol		comp[SIFT_AGG_MAX_JCOLS];
	int			nneed,
				ncomp,
				key = 0;
	Relation	second,
				ix = lookcol_load_index(rel, j, false, true, ix2, need, &nneed, comp, &ncomp);
	SiftSrcStats st,
				st2;

	if (ix == NULL)
		return NULL;
	second = ix2 != NULL ? *ix2 : NULL;
	while (need[key].att != j->keyatt[0])
		key++;
	if (!dense_leaves_ok(need, key, comp, ncomp))
	{
		lookcol_close(ix, second);
		return NULL;
	}
	sift_colsrc_stats(ix, &st);
	if (second != NULL)
		sift_colsrc_stats(second, &st2);
	/* the classes are an array over the keys: a table whose keys span more than the lookup budget (a sparse key) is read as rows */
	if ((need[key].ixn ? st2 : st).min[need[key].lf->id] < 0 ||
		!dense_span_ok((need[key].ixn ? st2 : st).max[need[key].lf->id] + 1, st.nrows, (double) sift_agg_lookup_max_kb * 1024 / sizeof(uint16)))
	{
		lookcol_close(ix, second);
		return NULL;
	}
	if (bound != NULL)
	{
		for (int c = 0; c < j->ncol; c++)
			for (int p = 0; p < nneed; p++)
				if (j->colexpr[c] ? need[p].xcol == c : need[p].expr == NULL && need[p].att == j->colatt[c])
					bound[c] = (need[p].ixn ? st2 : st).ndistinct[need[p].lf->id] > 0 ? (need[p].ixn ? st2 : st).ndistinct[need[p].lf->id] + 1 : 0;
		for (int k = 0; k < ncomp; k++)
			bound[comp[k].xcol] = 3;	/* true, false, NULL: a computed column is a test */
	}
	return ix;
}

bool
map_load_dense_possible(Relation rel, const SiftAggJoin *j, double *bound)
{
	Relation	ix2,
				ix = map_dense_index(rel, j, bound, &ix2);

	if (ix == NULL)
		return false;
	lookcol_close(ix, ix2);
	return true;
}

/*
 * Loads the table as classes of its keys (above) into m, from the leaves of an index of it. false, m unchanged, when the table is not one: no index
 * holds the columns, a column is not an integer, a key repeats or is negative or sparse, or there are more than DENSE_MAX_CLASSES tuples of values.
 */
bool
map_load_dense(AggExec *ex, MapTab *m, const SiftAggJoin *j, const SiftAggCombo *cb, Relation rel)
{
	ColNeed		need[MAX_NEED];
	CompCol		comp[SIFT_AGG_MAX_JCOLS];
	int			nneed,
				ncomp;
	Relation	ix,
				ix2;
	DenseLoad	d = {.cxt = m->cxt,.lastsb = PG_UINT32_MAX,.limit = (uint32) Min((int64) sift_agg_lookup_max_kb * 1024 / sizeof(uint16), PG_INT32_MAX)};
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot;
	bool		ok;

	if ((ix = lookcol_load_index(rel, j, false, true, &ix2, need, &nneed, comp, &ncomp)) == NULL)
		return false;
	for (d.key = 0; need[d.key].att != j->keyatt[0]; d.key++)
		;
	if (!dense_leaves_ok(need, d.key, comp, ncomp))
	{
		lookcol_close(ix, ix2);
		return false;
	}
	d.strs.cxt = m->cxt;
	for (int p = 0; p < nneed; p++)
		if (p != d.key && need[p].att != j->keyatt[0] && !need[p].feeder)
			d.col[d.ncol++] = p;
	for (int k = 0; k < ncomp; k++)
		d.col[d.ncol++] = -1;
	d.comp = comp;
	d.ncomp = ncomp;
	for (int k = 0; k < ncomp; k++)
		comp[k].st = ExecInitExpr((Expr *) comp[k].expr, NULL);
	if (ncomp > 0)
	{
		d.memo = comp_memo_make(comp, ncomp, need, nneed);
		d.ectx = CreateStandaloneExprContext();
		d.slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
		memset(d.slot->tts_isnull, true, desc->natts * sizeof(bool));
		ExecStoreVirtualTuple(d.slot);
	}
	dense_tables(&d, false);
	ok = ((ix2 == NULL && dense_gang(ex, rel, ix, need, nneed, &d)) || lookcol_read_units(&(UnitScope) {.snap = ex->snap}, rel, ix, ix2, need, nneed, dense_unit, &d)) &&
		!d.bad && d.rows > 0 && dense_span_ok(d.n, d.rows, d.limit);
	lookcol_close(ix, ix2);
	if (ncomp > 0)
	{
		ExecDropSingleTupleTableSlot(d.slot);
		FreeExprContext(d.ectx, true);
		pfree(d.memo->slot);
		pfree(d.memo->val);
		pfree(d.memo->nulls);
		pfree(d.memo->out);
		pfree(d.memo);
	}
	if (ok)
	{
		slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
		memset(slot->tts_isnull, true, desc->natts * sizeof(bool));
		ExecStoreVirtualTuple(slot);
		for (uint32 k = 0; k < d.ncls; k++)
		{
			int			x = 0;

			slot->tts_values[j->keyatt[0] - 1] = Int32GetDatum(d.ckey[k]);
			slot->tts_isnull[j->keyatt[0] - 1] = false;
			for (int c = 0; c < d.ncol; c++)
			{
				const ColNeed *nd = d.col[c] >= 0 ? &need[d.col[c]] : NULL;
				bool		isnull = (d.tnull[k] >> c) & 1;
				int64		tv = d.tv[(size_t) k * d.ncol + c];
				Datum		v = isnull ? (Datum) 0 : dense_datum(&d, nd, tv);
				int			xcol = nd == NULL ? comp[x++].xcol : nd->xcol;

				if (xcol >= 0)
				{
					m->xv[xcol] = v;
					m->xn[xcol] = isnull;
				}
				else
				{
					slot->tts_isnull[nd->att - 1] = isnull;
					slot->tts_values[nd->att - 1] = v;
				}
			}
			m->xfed = true;
			map_row(ex, m, slot, j, cb, NULL);
			m->xfed = false;
		}
		ExecDropSingleTupleTableSlot(slot);
		m->dcls = d.lut;
		m->dn = d.n;
		ex->lookup_cols++;
		ex->lookup_dense += d.rows;
	}
	else if (d.lut)
		pfree(d.lut);
	pfree(d.tv);
	pfree(d.tnull);
	pfree(d.ckey);
	pfree(d.ctab);
	return ok;
}
