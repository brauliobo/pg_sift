/*
 * agg/lookshare.c - the rows of a lookup join, in the shared memory of a parallel scan.
 *
 * The leader has the rows of every joined table before it starts the workers (read from the table, from the cache of an earlier query,
 * or run from the child plan, which a worker cannot do). It writes each table once in a flat form without pointers: the arrays of the
 * table as they are, the bytes of the text keys and of the by-reference columns after them, the references as offsets. A worker maps the
 * segment and only makes the two arrays that hold addresses (the text keys and the columns), the rest is read in place. The table is
 * never written to by a scan that shares it (a preserved side is: its join keeps its own copy, sift_aggspec_parallel_ok).
 */
#include "postgres.h"

#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/agg_int.h"


#define MAP_NOOFF	PG_UINT64_MAX

static void
col_kinds(const SiftAggJoin *j, bool *byval, int16 *len)
{
	for (int c = 0; c < j->ncol; c++)
		get_typlenbyval(j->coltype[c], &len[c], &byval[c]);
}

/* the bytes the segment of the table takes: *l is its layout, which map_share_write follows */
Size
map_share_size(const MapTab *m, const SiftAggJoin *j, MapShared *l)
{
	bool		byval[SIFT_AGG_MAX_JCOLS];
	int16		len[SIFT_AGG_MAX_JCOLS];
	Size		off = MAXALIGN(sizeof(MapShared)),
				data = 0;
	size_t		nk = (size_t) m->nrow * m->nkey,
				nc = (size_t) m->nrow * m->ncol;

	Assert(!m->keep && m->ncol <= SIFT_AGG_MAX_JCOLS);
	col_kinds(j, byval, len);
	memset(l, 0, sizeof(MapShared));
	l->nkey = m->nkey;
	l->ncol = m->ncol;
	l->nrow = m->nrow;
	l->nslot = m->nslot;
	l->filtered = m->filtered;
	memcpy(l->ktext, m->ktext, sizeof(l->ktext));
	for (uint32 r = 0; r < m->nrow; r++)
	{
		for (int k = 0; k < m->nkey; k++)
			if (m->ktext[k] && m->kptr[(size_t) r * m->nkey + k] != NULL)
				data += MAXALIGN(m->klen[(size_t) r * m->nkey + k]);
		for (int c = 0; c < m->ncol; c++)
			if (!byval[c] && !m->cnull[(size_t) r * m->ncol + c])
				data += MAXALIGN(datumGetSize(m->col[(size_t) r * m->ncol + c], false, len[c]));
	}
	l->klen = off;
	off += MAXALIGN(nk * sizeof(uint32));
	l->kint = off;
	off += MAXALIGN(nk * sizeof(int64));
	l->koff = off;
	off += MAXALIGN(nk * sizeof(uint64));
	l->cval = off;
	off += MAXALIGN(nc * sizeof(uint64));
	l->cnull = off;
	off += MAXALIGN(nc);
	l->hv = off;
	off += MAXALIGN((size_t) m->nrow * sizeof(uint32));
	l->next = off;
	off += MAXALIGN((size_t) m->nrow * sizeof(uint32));
	l->head = off;
	off += MAXALIGN((size_t) m->nslot * sizeof(uint32));
	l->data = off;
	l->size = off + data;
	return l->size;
}

void
map_share_write(const MapTab *m, const SiftAggJoin *j, const MapShared *l, char *dst)
{
	bool		byval[SIFT_AGG_MAX_JCOLS];
	int16		len[SIFT_AGG_MAX_JCOLS];
	size_t		nk = (size_t) m->nrow * m->nkey,
				nc = (size_t) m->nrow * m->ncol;
	uint64	   *koff = (uint64 *) (dst + l->koff),
			   *cval = (uint64 *) (dst + l->cval);
	Size		at = l->data;

	col_kinds(j, byval, len);
	memcpy(dst, l, sizeof(MapShared));
	memcpy(dst + l->klen, m->klen, nk * sizeof(uint32));
	memcpy(dst + l->kint, m->kint, nk * sizeof(int64));
	memcpy(dst + l->cnull, m->cnull, nc);
	memcpy(dst + l->hv, m->hv, (size_t) m->nrow * sizeof(uint32));
	memcpy(dst + l->next, m->next, (size_t) m->nrow * sizeof(uint32));
	memcpy(dst + l->head, m->head, (size_t) m->nslot * sizeof(uint32));
	for (uint32 r = 0; r < m->nrow; r++)
	{
		for (int k = 0; k < m->nkey; k++)
		{
			size_t		i = (size_t) r * m->nkey + k;

			if (!m->ktext[k] || m->kptr[i] == NULL)
			{
				koff[i] = MAP_NOOFF;
				continue;
			}
			koff[i] = at - l->data;
			memcpy(dst + at, m->kptr[i], m->klen[i]);
			at += MAXALIGN(m->klen[i]);
		}
		for (int c = 0; c < m->ncol; c++)
		{
			size_t		i = (size_t) r * m->ncol + c;

			if (byval[c] || m->cnull[i])
				cval[i] = (uint64) m->col[i];
			else
			{
				Size		n = datumGetSize(m->col[i], false, len[c]);

				cval[i] = at - l->data;
				memcpy(dst + at, DatumGetPointer(m->col[i]), n);
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
	bool		byval[SIFT_AGG_MAX_JCOLS];
	int16		len[SIFT_AGG_MAX_JCOLS];
	size_t		nk = (size_t) l->nrow * l->nkey,
				nc = (size_t) l->nrow * l->ncol;
	const uint64 *koff = (const uint64 *) (src + l->koff),
			   *cval = (const uint64 *) (src + l->cval);

	col_kinds(j, byval, len);
	m->cxt = cxt;
	m->nkey = l->nkey;
	m->ncol = l->ncol;
	m->nrow = l->nrow;
	m->cap = l->nrow;
	m->nslot = l->nslot;
	m->filtered = l->filtered;
	memcpy(m->ktext, l->ktext, sizeof(m->ktext));
	m->klen = (uint32 *) (src + l->klen);
	m->kint = (int64 *) (src + l->kint);
	m->cnull = (bool *) (src + l->cnull);
	m->hv = (uint32 *) (src + l->hv);
	m->next = (uint32 *) (src + l->next);
	m->head = (uint32 *) (src + l->head);
	m->kptr = MemoryContextAlloc(cxt, Max(nk, 1) * sizeof(char *));
	for (size_t i = 0; i < nk; i++)
		m->kptr[i] = koff[i] == MAP_NOOFF ? NULL : src + l->data + koff[i];
	m->col = MemoryContextAlloc(cxt, Max(nc, 1) * sizeof(Datum));
	for (uint32 r = 0; r < l->nrow; r++)
		for (int c = 0; c < l->ncol; c++)
		{
			size_t		i = (size_t) r * l->ncol + c;

			m->col[i] = byval[c] || m->cnull[i] ? (Datum) cval[i] : PointerGetDatum(src + l->data + cval[i]);
		}
	return m;
}
