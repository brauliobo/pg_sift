/*
 * agg/lookup/find.c - the probes of a lookup table
 * The index over the rows of a map, the load on demand, and the find of a row by key ids
 * or by a key.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "portability/instr_time.h"

#include "agg/lookup/int.h"

/* the hash chains of the rows: the rows of a key are the chain of its slot (a row of the same key is another link: a join repeats a tuple per match); the arrays are cut to the rows */
void
map_index_rows(MapTab *m)
{
	m->hit = m->keep ? MemoryContextAllocZero(m->cxt, Max(m->nrow, 1)) : NULL;
	if (!m->lazy)
	{
		m->nslot = 64;
		while (m->nslot < m->nrow)
			m->nslot *= 2;
		m->head = MemoryContextAllocZero(m->cxt, m->nslot * sizeof(uint32));
		m->next = MemoryContextAlloc(m->cxt, Max(m->nrow, 1) * sizeof(uint32));
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
	}
	if (m->cap > m->nrow && m->nrow > 0)
	{
		if (m->lazy)
			m->rkey = repalloc(m->rkey, (size_t) m->nrow * sizeof(int64));
		else
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

/* the dictionary of the keys, the ids of the rows and their chains of a table that was loaded as its keys alone (MapTab.rkey): what every use of the table but the byte table of its keys needs */
void
map_ensure(MapTab *m)
{
	int64	   *rkey = m->rkey;

	if (!m->lazy)
		return;
	m->rkey = NULL;
	m->lazy = false;
	m->kid = MemoryContextAlloc(m->cxt, Max(m->nrow, 1) * sizeof(uint32));
	for (uint32 r = 0; r < m->nrow; r++)
		m->kid[r] = kd_int(&m->kd[0], rkey[r], m->cxt);
	m->cap = m->nrow;
	map_index_rows(m);
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
	uint32		h,
				first[8];
	int			n = 0;

	map_ensure((MapTab *) m);
	h = map_hash_ids(m->nkey, kid) & (m->nslot - 1);
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

	map_ensure((MapTab *) m);
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
