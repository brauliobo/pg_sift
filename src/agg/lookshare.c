/*
 * agg/lookshare.c - the rows of a lookup join, in the shared memory of a parallel scan.
 *
 * The leader has the rows of every joined table before it starts the workers (read from the table, from the cache of an earlier query,
 * or run from the child plan, which a worker cannot do). It writes each table once in a flat form without pointers: the arrays of the
 * table as they are, the bytes of the distinct text keys and of the by-reference column values after them, the references as offsets. A
 * worker maps the segment and only makes the arrays that hold addresses (one per distinct text key and by-reference value), the rest is
 * read in place. The table is never written to by a scan that shares it: a preserved side keeps which of its rows a tuple matched in an array of
 * each participant (hit), which a worker sends to the leader with its tables.
 */
#include "postgres.h"

#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/agg_int.h"

static Size
share_array(Size *off, size_t n, size_t width)
{
	Size		at = *off;

	*off += MAXALIGN(n * width);
	return at;
}

/* the bytes the segment of the table takes: *l is its layout, which map_share_write follows */
Size
map_share_size(const MapTab *m, const SiftAggJoin *j, MapShared *l)
{
	Size		off = MAXALIGN(sizeof(MapShared)),
				data = 0;

	Assert(m->ncol <= SIFT_AGG_MAX_JCOLS);
	memset(l, 0, sizeof(MapShared));
	l->nkey = m->nkey;
	l->ncol = m->ncol;
	l->nrow = m->nrow;
	l->nslot = m->nslot;
	l->filtered = m->filtered;
	l->keep = m->keep;
	memcpy(l->ktext, m->ktext, sizeof(l->ktext));
	memcpy(l->knull, m->knull, sizeof(l->knull));
	l->lazy = m->lazy;
	if (m->lazy)
		l->rkey = share_array(&off, m->nrow, sizeof(int64));
	else
	{
		l->kid = share_array(&off, (size_t) m->nrow * m->nkey, sizeof(uint32));
		l->next = share_array(&off, m->nrow, sizeof(uint32));
		l->head = share_array(&off, m->nslot, sizeof(uint32));
	}
	for (int k = 0; k < m->nkey && !m->lazy; k++)
	{
		const KeyDict *d = &m->kd[k];

		l->key[k].n = d->n;
		l->key[k].nslot = d->nslot;
		l->key[k].slot = share_array(&off, d->nslot, sizeof(uint32));
		if (m->ktext[k])
		{
			l->key[k].len = share_array(&off, d->n, sizeof(uint32));
			l->key[k].hv = share_array(&off, d->n, sizeof(uint32));
			l->key[k].off = share_array(&off, d->n, sizeof(uint64));
			for (uint32 i = 0; i < d->n; i++)
				data += MAXALIGN(d->len[i]);
		}
		else
			l->key[k].ival = share_array(&off, d->n, sizeof(int64));
	}
	for (int c = 0; c < m->ncol; c++)
	{
		const MapCol *mc = &m->mcol[c];

		l->col[c].byval = mc->byval;
		l->col[c].len = mc->len;
		if (mc->byval)
		{
			l->col[c].val = share_array(&off, m->nrow, sizeof(uint64));
			l->col[c].hasnull = mc->isnull != NULL;
			if (mc->isnull)
				l->col[c].isnull = share_array(&off, m->nrow, sizeof(bool));
			continue;
		}
		l->col[c].n = mc->n;
		l->col[c].id = share_array(&off, m->nrow, sizeof(uint32));
		l->col[c].off = share_array(&off, mc->n, sizeof(uint64));
		for (uint32 i = 0; i < mc->n; i++)
			data += MAXALIGN(datumGetSize(mc->val[i], false, mc->len));
	}
	l->data = off;
	l->size = off + data;
	return l->size;
}

/* an array of a table with no rows is a null pointer, which memcpy must not be given even for no bytes */
static void
put_array(char *dst, const void *src, Size n)
{
	if (n > 0)
		memcpy(dst, src, n);
}

void
map_share_write(const MapTab *m, const SiftAggJoin *j, const MapShared *l, char *dst)
{
	Size		at = l->data;

	memcpy(dst, l, sizeof(MapShared));
	if (l->lazy)
		put_array(dst + l->rkey, m->rkey, (size_t) m->nrow * sizeof(int64));
	else
	{
		put_array(dst + l->kid, m->kid, (size_t) m->nrow * m->nkey * sizeof(uint32));
		put_array(dst + l->next, m->next, (size_t) m->nrow * sizeof(uint32));
		put_array(dst + l->head, m->head, (size_t) m->nslot * sizeof(uint32));
	}
	for (int k = 0; k < m->nkey && !l->lazy; k++)
	{
		const KeyDict *d = &m->kd[k];

		put_array(dst + l->key[k].slot, d->slot, (size_t) d->nslot * sizeof(uint32));
		if (m->ktext[k])
		{
			uint64	   *off = (uint64 *) (dst + l->key[k].off);

			put_array(dst + l->key[k].len, d->len, (size_t) d->n * sizeof(uint32));
			put_array(dst + l->key[k].hv, d->hv, (size_t) d->n * sizeof(uint32));
			for (uint32 i = 0; i < d->n; i++)
			{
				off[i] = at - l->data;
				memcpy(dst + at, d->ptr[i], d->len[i]);
				at += MAXALIGN(d->len[i]);
			}
		}
		else
			put_array(dst + l->key[k].ival, d->ival, (size_t) d->n * sizeof(int64));
	}
	for (int c = 0; c < m->ncol; c++)
	{
		const MapCol *mc = &m->mcol[c];

		if (mc->byval)
		{
			put_array(dst + l->col[c].val, mc->val, (size_t) m->nrow * sizeof(uint64));
			if (mc->isnull)
				put_array(dst + l->col[c].isnull, mc->isnull, m->nrow);
		}
		else
		{
			uint64	   *off = (uint64 *) (dst + l->col[c].off);

			put_array(dst + l->col[c].id, mc->id, (size_t) m->nrow * sizeof(uint32));
			for (uint32 i = 0; i < mc->n; i++)
			{
				Size		n = datumGetSize(mc->val[i], false, mc->len);

				off[i] = at - l->data;
				memcpy(dst + at, DatumGetPointer(mc->val[i]), n);
				at += MAXALIGN(n);
			}
		}
	}
	Assert(at == l->size);
}

/* the table of a segment: the arrays that hold no address are the segment's own */
MapTab *
map_share_attach(const char *src, const SiftAggJoin *j, MemoryContext cxt)
{
	const MapShared *l = (const MapShared *) src;
	MapTab	   *m = MemoryContextAllocZero(cxt, sizeof(MapTab));

	m->cxt = cxt;
	m->nkey = l->nkey;
	m->ncol = l->ncol;
	m->nrow = l->nrow;
	m->cap = l->nrow;
	m->nslot = l->nslot;
	m->filtered = l->filtered;
	m->keep = l->keep;
	m->hit = l->keep ? MemoryContextAllocZero(cxt, Max(l->nrow, 1)) : NULL;
	memcpy(m->ktext, l->ktext, sizeof(m->ktext));
	memcpy(m->knull, l->knull, sizeof(m->knull));
	m->lazy = l->lazy;
	if (l->lazy)
		m->rkey = l->nrow > 0 ? (int64 *) (src + l->rkey) : NULL;
	else
	{
		m->kid = (uint32 *) (src + l->kid);
		m->next = (uint32 *) (src + l->next);
		m->head = (uint32 *) (src + l->head);
	}
	for (int k = 0; k < m->nkey && !l->lazy; k++)
	{
		KeyDict    *d = &m->kd[k];

		d->n = d->cap = l->key[k].n;
		d->nslot = l->key[k].nslot;
		d->slot = (uint32 *) (src + l->key[k].slot);
		if (m->ktext[k])
		{
			const uint64 *off = (const uint64 *) (src + l->key[k].off);

			d->len = (uint32 *) (src + l->key[k].len);
			d->hv = (uint32 *) (src + l->key[k].hv);
			d->ptr = MemoryContextAlloc(cxt, Max(d->n, 1) * sizeof(char *));
			for (uint32 i = 0; i < d->n; i++)
				d->ptr[i] = src + l->data + off[i];
		}
		else
			d->ival = (int64 *) (src + l->key[k].ival);
	}
	m->mcol = MemoryContextAllocZero(cxt, Max(m->ncol, 1) * sizeof(MapCol));
	for (int c = 0; c < m->ncol; c++)
	{
		MapCol	   *mc = &m->mcol[c];

		mc->byval = l->col[c].byval;
		mc->len = l->col[c].len;
		if (mc->byval)
		{
			mc->val = (Datum *) (src + l->col[c].val);
			mc->isnull = l->col[c].hasnull ? (bool *) (src + l->col[c].isnull) : NULL;
		}
		else
		{
			const uint64 *off = (const uint64 *) (src + l->col[c].off);

			mc->n = mc->cap = l->col[c].n;
			mc->id = (uint32 *) (src + l->col[c].id);
			mc->val = MemoryContextAlloc(cxt, Max(mc->n, 1) * sizeof(Datum));
			for (uint32 i = 0; i < mc->n; i++)
				mc->val[i] = PointerGetDatum(src + l->data + off[i]);
		}
	}
	return m;
}
