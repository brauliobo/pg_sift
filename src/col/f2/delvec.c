/*
 * col/delvec.c - the immutable deletion vector of a part (col/delvec.h).
 */
#include "core/host.h"

#include "access/transam.h"
#include "storage/procarray.h"

#include "col/f2/delvec.h"
#include "col/f2/delvec_delta.h"
#include "col/f2/part_format.h"
#include "col/f2/part_vis.h"
#include "core/f2/fsm.h"
#include "core/page_io.h"
#include "core/xid_cache.h"

typedef struct Tup
{
	uint32		granule;
	uint16		row;
	SiftDvXmaxEntry x;
	int			seq;
} Tup;

static int
tup_cmp(const void *a, const void *b)
{
	const Tup  *x = a,
			   *y = b;

	if (x->granule != y->granule)
		return x->granule < y->granule ? -1 : 1;
	if (x->row != y->row)
		return x->row < y->row ? -1 : 1;
	return x->seq - y->seq;
}

/* The least and the greatest deleter the granule keeps, as full ids of the epoch of next; both invalid when it keeps none */
static void
xmax_bounds(const SiftDvGran *g, FullTransactionId next, FullTransactionId *lo, FullTransactionId *hi)
{
	*lo = *hi = InvalidFullTransactionId;
	for (int r = 0; r < g->ndead; r++)
		if (TransactionIdIsValid(g->xmax[r].xmax))
		{
			FullTransactionId f = sift_xid_fxid_at(next, g->xmax[r].xmax);

			if (!FullTransactionIdIsValid(*lo) || FullTransactionIdPrecedes(f, *lo))
				*lo = f;
			if (!FullTransactionIdIsValid(*hi) || FullTransactionIdFollows(f, *hi))
				*hi = f;
		}
}

/* The rows of base (ascending) and of the new marks of the granule (ascending, one for each row) as the rows of g, the row of base kept where both have it */
static void
merge_rows(SiftDvGran *g, const SiftDvGran *base, const Tup *m, int nm)
{
	int			cap = (base != NULL ? base->ndead : 0) + nm,
				i = 0,
				k = 0;

	g->rows = palloc(Max(cap, 1) * sizeof(uint16));
	g->xmax = palloc(Max(cap, 1) * sizeof(SiftDvXmaxEntry));
	while ((base != NULL && i < base->ndead) || k < nm)
	{
		if (k == nm || (base != NULL && i < base->ndead && base->rows[i] <= m[k].row))
		{
			if (k < nm && m[k].row == base->rows[i])
				k++;
			g->rows[g->ndead] = base->rows[i];
			g->xmax[g->ndead++] = base->xmax[i++];
		}
		else
		{
			g->rows[g->ndead] = m[k].row;
			g->xmax[g->ndead++] = m[k++].x;
		}
	}
}

/* The marks are sorted by granule and row alone (a mark of a row already dead keeps the first); the granules of base that no mark names are copied, and the others merged row by row */
SiftDv *
sift_dv_merge(const SiftDv *base, const SiftDvMark *marks, int n, uint64 part_gen, uint64 dv_gen, uint32 ngranules, const uint16 *granule_rows)
{
	int			nbase = base != NULL ? base->ndirty : 0,
				b = 0,
				i = 0;
	Tup		   *t = palloc((n + 1) * sizeof(Tup));
	SiftDv	   *dv = palloc0(sizeof(SiftDv));
	FullTransactionId next = ReadNextFullTransactionId(),
				lo;

	for (int k = 0; k < n; k++)
	{
		if (marks[k].granule >= ngranules || marks[k].row >= granule_rows[marks[k].granule])
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: row %u of granule %u is not in the part", marks[k].row, marks[k].granule)));
		t[k] = (Tup) {marks[k].granule, marks[k].row, {marks[k].xmax, marks[k].cmax}, k};
	}
	qsort(t, n, sizeof(Tup), tup_cmp);
	dv->part_gen = part_gen;
	dv->dv_gen = dv_gen;
	dv->ngranules = ngranules;
	dv->gran = palloc0((nbase + n + 1) * sizeof(SiftDvGran));
	while (b < nbase || i < n)
	{
		uint32		gid = b == nbase ? t[i].granule : i == n ? base->gran[b].granule_id : Min(base->gran[b].granule_id, t[i].granule);
		const SiftDvGran *bg = b < nbase && base->gran[b].granule_id == gid ? &base->gran[b++] : NULL;
		SiftDvGran *g = &dv->gran[dv->ndirty++];
		int			j = i,
					m = 0;

		while (j < n && t[j].granule == gid)
		{
			if (j == i || t[j].row != t[j - 1].row)
				t[i + m++] = t[j];
			j++;
		}
		g->granule_id = gid;
		g->granule_rows = granule_rows[gid];
		merge_rows(g, bg, t + i, m);
		xmax_bounds(g, next, &lo, &g->xid_max);
		i = j;
	}
	pfree(t);
	return dv;
}

const SiftDvGran *
sift_dv_granule(const SiftDv *dv, uint32 granule)
{
	int			lo = 0,
				hi = dv->ndirty - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (dv->gran[mid].granule_id == granule)
			return &dv->gran[mid];
		if (dv->gran[mid].granule_id < granule)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

uint64
sift_dv_ndead(const SiftDv *dv)
{
	uint64		n = 0;

	for (int i = 0; i < dv->ndirty; i++)
		n += dv->gran[i].ndead;
	return n;
}

const SiftDvXmaxEntry *
sift_dv_find(const SiftDvGran *g, uint16 row, bool *dead)
{
	int			lo = 0,
				hi = g->ndead - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (g->rows[mid] == row)
		{
			*dead = true;
			return &g->xmax[mid];
		}
		if (g->rows[mid] < row)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	*dead = false;
	return NULL;
}

void
sift_dv_mask(const SiftDvGran *g, uint8 *bitmap)
{
	memset(bitmap, 0, SIFT_DV_BITMAP_BYTES);
	for (int i = 0; i < g->ndead; i++)
		bitmap[g->rows[i] >> 3] |= 1 << (g->rows[i] & 7);
}

/* The deleters of an unsettled granule are a handful of transactions for thousands of rows: the log is asked once for each (xc, made for snap, is not used for a settled granule or no snapshot) */
static inline bool
dv_row_seen(const SiftDvGran *g, int i, Snapshot snap, SiftXidCache *xc)
{
	return snap == NULL || g->xmax == NULL || sift_vis_deleted_cached(xc, g->xmax[i].xmax, g->xmax[i].cmax);
}

uint16
sift_dv_mask64(const SiftDvGran *g, Snapshot snap, uint64 *words128)
{
	uint16		n = 0;
	SiftXidCache xc;

	memset(words128, 0, SIFT_DV_MASK_WORDS * sizeof(uint64));
	if (snap != NULL && g->xmax != NULL)
		sift_xid_cache_init(&xc, snap);
	for (int i = 0; i < g->ndead; i++)
		if (dv_row_seen(g, i, snap, &xc))
		{
			words128[g->rows[i] >> 6] |= UINT64CONST(1) << (g->rows[i] & 63);
			n++;
		}
	return n;
}

uint16
sift_dv_visible_rows(const SiftDvGran *g, Snapshot snap, uint16 *out, uint16 max)
{
	uint16		n = 0;
	SiftXidCache xc;

	if (snap != NULL && g->xmax != NULL)
		sift_xid_cache_init(&xc, snap);
	for (int i = 0; i < g->ndead; i++)
		if (dv_row_seen(g, i, snap, &xc))
		{
			if (n < max)
				out[n] = g->rows[i];
			n++;
		}
	return n;
}

char *
sift_dv_serialize(const SiftDv *dv, uint32 *len)
{
	SiftDvHorizon hz;

	sift_dvd_horizon_open(&hz);
	return sift_dvd_serialize(dv, &hz.ops, len);
}

SiftDv *
sift_dv_deserialize(Relation rel, const char *bytes, uint32 len, uint64 part_gen, uint64 dv_gen, const uint16 *granule_rows)
{
	SiftDvHorizon hz;
	const char *why;
	SiftDv	   *dv;

	sift_dvd_horizon_open(&hz);
	dv = sift_dvd_deserialize(bytes, len, part_gen, dv_gen, granule_rows, &hz.ops, &why);
	if (dv == NULL)
		SIFT_SEG_CORRUPT(rel, part_gen, "The deletion vector %llu is damaged: %s.", (unsigned long long) dv_gen, why);
	return dv;
}

SiftDv *
sift_dv_settle(const SiftDv *dv, FullTransactionId horizon, FullTransactionId *stripped)
{
	SiftDv	   *out = palloc0(sizeof(SiftDv));
	SiftXidHorizonCache hc;
	FullTransactionId lo;

	sift_xid_horizon_init(&hc, horizon);
	*stripped = InvalidFullTransactionId;
	*out = *dv;
	out->ndirty = 0;
	out->gran = palloc0(Max(dv->ndirty, 1) * sizeof(SiftDvGran));
	for (int i = 0; i < dv->ndirty; i++)
	{
		const SiftDvGran *g = &dv->gran[i];
		SiftDvGran *o = &out->gran[out->ndirty];

		*o = *g;
		o->ndead = 0;
		o->rows = palloc(g->ndead * sizeof(uint16));
		o->xmax = palloc(g->ndead * sizeof(SiftDvXmaxEntry));
		for (int r = 0; r < g->ndead; r++)
		{
			SiftDvXmaxEntry x = g->xmax[r];

			if (TransactionIdIsValid(x.xmax) && sift_xid_horizon_older(&hc, x.xmax))
			{
				SiftHorizonStatus st = sift_xid_horizon_status(&hc, x.xmax);

				if (st == SIFT_H_COMMITTED_OLD)
				{
					FullTransactionId f = sift_xid_fxid_at(hc.next, x.xmax);

					if (!FullTransactionIdIsValid(*stripped) || FullTransactionIdFollows(f, *stripped))
						*stripped = f;
					x = (SiftDvXmaxEntry) {InvalidTransactionId, 0};
				}
				else if (st == SIFT_H_ABORTED)
					continue;
			}
			o->rows[o->ndead] = g->rows[r];
			o->xmax[o->ndead++] = x;
		}
		if (o->ndead > 0)
		{
			xmax_bounds(o, hc.next, &lo, &o->xid_max);
			out->ndirty++;
		}
	}
	return out;
}

SiftDv *
sift_dv_unabort(const SiftDv *dv, bool *dropped)
{
	SiftDv	   *out = palloc0(sizeof(SiftDv));
	SiftXidHorizonCache hc;
	FullTransactionId lo;

	sift_xid_horizon_init(&hc, InvalidFullTransactionId);
	*dropped = false;
	*out = *dv;
	out->ndirty = 0;
	out->gran = palloc0(Max(dv->ndirty, 1) * sizeof(SiftDvGran));
	for (int i = 0; i < dv->ndirty; i++)
	{
		const SiftDvGran *g = &dv->gran[i];
		SiftDvGran *o = &out->gran[out->ndirty];

		*o = *g;
		o->ndead = 0;
		o->rows = palloc(g->ndead * sizeof(uint16));
		o->xmax = palloc(g->ndead * sizeof(SiftDvXmaxEntry));
		for (int r = 0; r < g->ndead; r++)
		{
			if (TransactionIdIsNormal(g->xmax[r].xmax) && sift_xid_horizon_status(&hc, g->xmax[r].xmax) == SIFT_H_ABORTED)
			{
				*dropped = true;
				continue;
			}
			o->rows[o->ndead] = g->rows[r];
			o->xmax[o->ndead++] = g->xmax[r];
		}
		if (o->ndead > 0)
		{
			xmax_bounds(o, hc.next, &lo, &o->xid_max);
			out->ndirty++;
		}
	}
	return out;
}

SiftExtent
sift_dv_write(Relation rel, const SiftDv *dv, uint32 part_id, SiftPartDesc *desc)
{
	uint32		len;
	char	   *bytes = sift_dv_serialize(dv, &len);
	const SiftDvHeaderData *h = (const SiftDvHeaderData *) bytes;
	uint32		npages = SIFT_PAYLOAD_PAGES(len);
	SiftExtent	ext = sift_fsm_alloc(rel, npages);

	for (uint32 p = 0; p < npages; p++)
	{
		SiftXlog	x = sift_xlog_start(rel);
		Page		pg = sift_xlog_register(&x, sift_blk_new(rel, ext.start + p), GENERIC_XLOG_FULL_IMAGE);
		uint32		from = SIFT_PAYLOAD_BYTES(p),
					n = Min(len - from, (uint32) SIFT_PAGE_PAYLOAD);

		sift_page_init_part(pg, SIFT_PT_DELVEC, dv->dv_gen, part_id);
		memcpy(SiftPageGetData(pg), bytes + from, n);
		SiftPageSetUsed(pg, n);
		sift_xlog_done(&x);
	}
	desc->dv_blk = ext.start;
	desc->dv_nblocks = ext.nblocks;
	desc->dv_gen = dv->dv_gen;
	desc->dv_max_xid = h->xid_max;
	desc->ndead = h->ndead_total;
	desc->flags = (desc->flags & ~SIFT_PART_FLAG_DV_SETTLED) | ((h->flags & SIFT_DV_FLAG_ALL_SETTLED) ? SIFT_PART_FLAG_DV_SETTLED : 0);
	return ext;
}

SiftDv *
sift_dv_read(Relation rel, const SiftPartDesc *desc, uint32 part_id, const uint16 *granule_rows)
{
	Buffer		buf = sift_blk_read(rel, desc->dv_blk, BUFFER_LOCK_SHARE);
	uint32		len = ((const SiftDvHeaderData *) SiftPageGetData(BufferGetPage(buf)))->stream_len;
	char	   *bytes;

	UnlockReleaseBuffer(buf);
	if (len < sizeof(SiftDvHeaderData) || len > SIFT_PAYLOAD_BYTES(desc->dv_nblocks))
		SIFT_SEG_CORRUPT(rel, desc->part_gen, "The deletion vector at block %u claims %u bytes in %u blocks.", desc->dv_blk, len, desc->dv_nblocks);
	bytes = palloc(len);
	for (uint32 from = 0, p = 0; from < len; from += SIFT_PAGE_PAYLOAD, p++)
	{
		buf = sift_blk_read(rel, desc->dv_blk + p, BUFFER_LOCK_SHARE);
		sift_page_expect(rel, BufferGetPage(buf), desc->dv_blk + p, SIFT_PT_DELVEC, desc->dv_gen, SIFT_PART_ID_ANY);
		memcpy(bytes + from, SiftPageGetData(BufferGetPage(buf)), Min(len - from, (uint32) SIFT_PAGE_PAYLOAD));
		UnlockReleaseBuffer(buf);
	}
	return sift_dv_deserialize(rel, bytes, len, desc->part_gen, desc->dv_gen, granule_rows);
}
