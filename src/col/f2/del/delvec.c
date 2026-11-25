/*
 * col/delvec.c - the immutable deletion vector of a part (col/delvec.h).
 */
#include "core/host.h"

#include "access/transam.h"
#include "storage/procarray.h"

#include "col/f2/del/delvec.h"
#include "col/f2/del/delvec_delta.h"
#include "col/f2/part/format.h"
#include "col/f2/part/vis.h"
#include "core/f2/fsm/fsm.h"
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

static Tup *
marks_sorted(const SiftDvMark *marks, int n, uint32 ngranules, const uint16 *granule_rows)
{
	Tup		   *t = palloc((n + 1) * sizeof(Tup));

	for (int k = 0; k < n; k++)
	{
		if (marks[k].granule >= ngranules || marks[k].row >= granule_rows[marks[k].granule])
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: row %u of granule %u is not in the part", marks[k].row, marks[k].granule)));
		t[k] = (Tup) {marks[k].granule, marks[k].row, {marks[k].xmax, marks[k].cmax}, k};
	}
	qsort(t, n, sizeof(Tup), tup_cmp);
	return t;
}

/* The marks of granule gid that start at t[*i]: one for each row (the first), moved to the start (*run); *i is past all of them, the count of the ones kept is returned */
static int
marks_run(Tup *t, int *i, int n, uint32 gid, Tup **run)
{
	int			j = *i,
				m = 0;

	*run = t + *i;
	while (j < n && t[j].granule == gid)
	{
		if (j == *i || t[j].row != t[j - 1].row)
			t[*i + m++] = t[j];
		j++;
	}
	*i = j;
	return m;
}

/* The marks are sorted by granule and row alone (a mark of a row already dead keeps the first); the granules of base that no mark names are copied, and the others merged row by row */
SiftDv *
sift_dv_merge(const SiftDv *base, const SiftDvMark *marks, int n, uint64 part_gen, uint64 dv_gen, uint32 ngranules, const uint16 *granule_rows)
{
	int			nbase = base != NULL ? base->ndirty : 0,
				b = 0,
				i = 0;
	Tup		   *t = marks_sorted(marks, n, ngranules, granule_rows);
	SiftDv	   *dv = palloc0(sizeof(SiftDv));
	FullTransactionId next = ReadNextFullTransactionId(),
				lo;

	dv->part_gen = part_gen;
	dv->dv_gen = dv_gen;
	dv->ngranules = ngranules;
	dv->gran = palloc0((ngranules + 1) * sizeof(SiftDvGran));
	while (b < nbase || i < n)
	{
		uint32		gid = b == nbase ? t[i].granule : i == n ? base->gran[b].granule_id : Min(base->gran[b].granule_id, t[i].granule);
		const SiftDvGran *bg = b < nbase && base->gran[b].granule_id == gid ? &base->gran[b++] : NULL;
		SiftDvGran *g = &dv->gran[dv->ndirty++];
		Tup		   *run;
		int			m = marks_run(t, &i, n, gid, &run);

		g->granule_id = gid;
		g->granule_rows = granule_rows[gid];
		merge_rows(g, bg, run, m);
		xmax_bounds(g, next, &lo, &g->xid_max);
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

static char *
read_stream(Relation rel, const SiftPartDesc *desc, uint32 *len)
{
	Buffer		buf = sift_blk_read(rel, desc->dv_blk, BUFFER_LOCK_SHARE);
	char	   *bytes;

	*len = ((const SiftDvHeaderData *) SiftPageGetData(BufferGetPage(buf)))->stream_len;
	if (*len < sizeof(SiftDvHeaderData) || *len > SIFT_PAYLOAD_BYTES(desc->dv_nblocks))
		SIFT_SEG_CORRUPT(rel, desc->part_gen, "The deletion vector at block %u claims %u bytes in %u blocks.", desc->dv_blk, *len, desc->dv_nblocks);
	bytes = palloc(*len);
	for (uint32 from = 0, p = 0; from < *len; from += SIFT_PAGE_PAYLOAD, p++)
	{
		buf = p == 0 ? buf : sift_blk_read(rel, desc->dv_blk + p, BUFFER_LOCK_SHARE);
		sift_page_expect(rel, BufferGetPage(buf), desc->dv_blk + p, SIFT_PT_DELVEC, desc->dv_gen, SIFT_PART_ID_ANY);
		memcpy(bytes + from, SiftPageGetData(BufferGetPage(buf)), Min(*len - from, (uint32) SIFT_PAGE_PAYLOAD));
		UnlockReleaseBuffer(buf);
	}
	return bytes;
}

SiftDv *
sift_dv_read_stream(Relation rel, const SiftPartDesc *desc, uint32 part_id, const uint16 *granule_rows, uint32 *stream_len)
{
	char	   *bytes = read_stream(rel, desc, stream_len);

	return sift_dv_deserialize(rel, bytes, *stream_len, desc->part_gen, desc->dv_gen, granule_rows);
}

SiftDv *
sift_dv_read(Relation rel, const SiftPartDesc *desc, uint32 part_id, const uint16 *granule_rows)
{
	uint32		len;

	return sift_dv_read_stream(rel, desc, part_id, granule_rows, &len);
}

/* ---- the vector as the stream it was read from ---- */

/* A granule of the stream that the stream's own entry tells, or (ent NULL) one it does not: with a delta tail merged, or the rows of an aborted delete dropped */
typedef struct ImageGran
{
	uint32		granule_id;
	const SiftDvGranuleEntry *ent;
	SiftDvGran	patch;
} ImageGran;

struct SiftDvImage
{
	const char *bytes;
	uint32		base_len;
	uint32		n;
	ImageGran  *g;				/* the granules that have dead rows, ascending */
};

/* The rows of g whose deleter aborted are alive again: dropped; whether there were any */
static bool
gran_unabort(SiftDvGran *g, SiftXidHorizonCache *hc)
{
	int			k = 0;
	FullTransactionId lo;

	for (int r = 0; r < g->ndead; r++)
	{
		if (TransactionIdIsNormal(g->xmax[r].xmax) && sift_xid_horizon_status(hc, g->xmax[r].xmax) == SIFT_H_ABORTED)
			continue;
		g->rows[k] = g->rows[r];
		g->xmax[k++] = g->xmax[r];
	}
	if (k == g->ndead)
		return false;
	g->ndead = k;
	xmax_bounds(g, hc->next, &lo, &g->xid_max);
	return true;
}

static void
gran_free(SiftDvGran *g)
{
	pfree(g->rows);
	pfree(g->xmax);
}

/* The granule, one at a time, so that what is held is the stream, a few bytes for a granule that is all dead or has one deleter, and not the rows it stands for: the granules that the delta tail names
 * or an aborted delete leaves rows of are decoded and kept (the tail is the one page of its vector) */
SiftDvImage *
sift_dv_image_read(Relation rel, const SiftPartDesc *desc, const uint16 *granule_rows, bool *unaborted)
{
	SiftDvImage *img = palloc0(sizeof(SiftDvImage));
	SiftDv	   *tail = palloc0(sizeof(SiftDv));
	SiftDvHorizon hz;
	SiftXidHorizonCache hc;
	uint32		len,
				e = 0;
	int			t = 0;
	const char *why;

	img->bytes = read_stream(rel, desc, &len);
	why = sift_dvd_check_stream(img->bytes, len, desc->part_gen, desc->dv_gen, &img->base_len);
	if (why == NULL)
	{
		const SiftDvHeaderData *h = (const SiftDvHeaderData *) img->bytes;
		const SiftDvGranuleEntry *ent = (const SiftDvGranuleEntry *) (img->bytes + sizeof(*h));

		sift_dvd_horizon_open(&hz);
		sift_xid_horizon_init(&hc, InvalidFullTransactionId);
		tail->gran = palloc0(sizeof(SiftDvGran));
		if (img->base_len < len)
			why = sift_dvd_read_tail(tail, img->bytes, img->base_len, len, granule_rows, h->ngranules, &hz.ops);
		img->g = palloc((h->ndirty_granules + tail->ndirty + 1) * sizeof(ImageGran));
		*unaborted = false;
		while (why == NULL && (e < h->ndirty_granules || t < tail->ndirty))
		{
			const uint32 gid = e == h->ndirty_granules ? tail->gran[t].granule_id : t == tail->ndirty ? ent[e].granule_id : Min(ent[e].granule_id, tail->gran[t].granule_id);
			const SiftDvGranuleEntry *en = e < h->ndirty_granules && ent[e].granule_id == gid ? &ent[e++] : NULL;
			const SiftDvGran *tg = t < tail->ndirty && tail->gran[t].granule_id == gid ? &tail->gran[t++] : NULL;
			SiftDvGran	g = {.granule_id = gid,.granule_rows = granule_rows[gid]};
			bool		dropped,
						whole;

			if (en != NULL)
			{
				why = sift_dvd_read_granule(&g, en, en > ent ? en - 1 : NULL, h, img->bytes, img->base_len, granule_rows);
				if (why != NULL)
					break;
			}
			else
			{
				g.rows = palloc(1);
				g.xmax = palloc(1);
			}
			if (tg != NULL)
				sift_dvd_gran_union(&g, tg);
			dropped = gran_unabort(&g, &hc);
			whole = tg == NULL && !dropped;
			*unaborted |= dropped;
			if (g.ndead > 0)
				img->g[img->n++] = whole ? (ImageGran) {gid, en, {0}} : (ImageGran) {gid, NULL, g};
			if (g.ndead == 0 || whole)
				gran_free(&g);
		}
	}
	if (why != NULL)
		SIFT_SEG_CORRUPT(rel, desc->part_gen, "The deletion vector %llu is damaged: %s.", (unsigned long long) desc->dv_gen, why);
	for (int i = 0; i < tail->ndirty; i++)
		gran_free(&tail->gran[i]);
	pfree(tail->gran);
	pfree(tail);
	return img;
}

static bool
raw_dead(const SiftDvGranuleEntry *e, const char *bytes, uint16 row)
{
	const char *p = bytes + e->payload_off;
	int			lo = 0,
				hi = (int) e->ndead - 1;

	switch (e->kind & SIFT_DVG_KIND_MASK)
	{
		case SIFT_DV_ALL_DEAD:
			return true;
		case SIFT_DV_BITMAP:
			return (((const uint8 *) p)[row >> 3] >> (row & 7)) & 1;
		default:
			while (lo <= hi)
			{
				int			mid = (lo + hi) / 2;
				uint16		r;

				memcpy(&r, p + mid * sizeof(uint16), sizeof(r));
				if (r == row)
					return true;
				if (r < row)
					lo = mid + 1;
				else
					hi = mid - 1;
			}
			return false;
	}
}

bool
sift_dv_image_dead(const SiftDvImage *img, uint32 granule, uint16 row)
{
	int			lo = 0,
				hi = (int) img->n - 1;

	while (lo <= hi)
	{
		const ImageGran *s = &img->g[(lo + hi) / 2];

		if (s->granule_id == granule)
		{
			bool		dead;

			if (s->ent != NULL)
				return raw_dead(s->ent, img->bytes, row);
			(void) sift_dv_find(&s->patch, row, &dead);
			return dead;
		}
		if (s->granule_id < granule)
			lo = (lo + hi) / 2 + 1;
		else
			hi = (lo + hi) / 2 - 1;
	}
	return false;
}

/* The granule that the marks run (m of them, ascending, one for a row) add rows to, and the rows of the stream's granule s (NULL: none) it has; the builder takes the merged granule */
static void
merge_granule(SiftDvBuilder *b, const SiftDvImage *img, const ImageGran *s, const Tup *run, int m, uint32 gid, const uint16 *granule_rows)
{
	SiftDvGran	base = {0},
				g = {.granule_id = gid,.granule_rows = granule_rows[gid]};
	const SiftDvGran *bg = s != NULL ? &s->patch : NULL;

	if (s != NULL && s->ent != NULL)
	{
		const char *why = sift_dvd_read_granule(&base, s->ent, NULL, (const SiftDvHeaderData *) img->bytes, img->bytes, img->base_len, granule_rows);

		SIFT_INVARIANT(why == NULL, "the granule of a deletion vector read before does not read: %s", why);
		bg = &base;
	}
	merge_rows(&g, bg, run, m);
	sift_dvb_gran(b, &g);
	gran_free(&g);
	if (base.rows != NULL)
		gran_free(&base);
}

char *
sift_dv_image_merge(const SiftDvImage *img, const SiftDvMark *marks, int n, uint64 part_gen, uint64 dv_gen, uint32 ngranules, const uint16 *granule_rows, uint32 *len)
{
	const uint32 nimg = img != NULL ? img->n : 0;
	Tup		   *t = marks_sorted(marks, n, ngranules, granule_rows);
	SiftDvHorizon hz;
	SiftDvBuilder *b;
	uint32		a = 0;
	int			i = 0;

	sift_dvd_horizon_open(&hz);
	b = sift_dvb_new(&hz.ops, ngranules, part_gen, dv_gen);
	while (a < nimg || i < n)
	{
		const uint32 gid = a == nimg ? t[i].granule : i == n ? img->g[a].granule_id : Min(img->g[a].granule_id, t[i].granule);
		const ImageGran *s = a < nimg && img->g[a].granule_id == gid ? &img->g[a++] : NULL;
		Tup		   *run;
		int			m = marks_run(t, &i, n, gid, &run);

		if (m > 0)
			merge_granule(b, img, s, run, m, gid, granule_rows);
		else if (s->ent != NULL)
			sift_dvb_raw(b, s->ent, img->bytes);
		else
			sift_dvb_gran(b, &s->patch);
	}
	pfree(t);
	return sift_dvb_finish(b, len);
}
