/*
 * col/f2/delvec_delta.c - the codec of the deletion vector, its palette of deleters and its in-place delta tail (col/f2/delvec_delta.h).
 */
#include "core/host.h"

#include "col/f2/delvec_delta.h"
#include "core/f2/meta.h"
#include "core/hash.h"
#include "core/xlog.h"

#ifndef SIFT_HREF_STANDALONE
#include "access/transam.h"
#include "storage/lmgr.h"

#include "col/f2/f2_cache.h"
#include "core/f2/fsm.h"
#include "core/f2/manifest.h"
#include "core/page_io.h"
#include "core/xid_cache.h"
#endif

#define DV_HDR(pg)	((SiftDvHeaderData *) SiftPageGetData(pg))

/* ---- serialize ---- */

static SiftDvKind
kind_of(const SiftDvGran *g)
{
	if (g->ndead == g->granule_rows)
		return SIFT_DV_ALL_DEAD;
	return g->ndead > SIFT_DV_ARRAY_MAX ? SIFT_DV_BITMAP : SIFT_DV_ARRAY16;
}

static void
put_bitmap(const SiftDvGran *g, uint8 *bitmap)
{
	memset(bitmap, 0, SIFT_DV_BITMAP_BYTES);
	for (int r = 0; r < g->ndead; r++)
		bitmap[g->rows[r] >> 3] |= (uint8) (1 << (g->rows[r] & 7));
}

static bool
same_deleter(const SiftDvXmaxEntry *a, const SiftDvXmaxEntry *b)
{
	return a->xmax == b->xmax && a->cmax == b->cmax;
}

/* The deleters of the granule once each, in the order they first appear; the count stops one past the palette */
static int
palette_of(const SiftDvGran *g, SiftDvXmaxEntry *uniq)
{
	int			u = 0;

	for (int r = 0; r < g->ndead; r++)
	{
		int			k = 0;

		while (k < u && !same_deleter(&uniq[k], &g->xmax[r]))
			k++;
		if (k == u)
		{
			if (u > SIFT_DVG_RLE_MAX_UNIQ)
				return u;
			uniq[u++] = g->xmax[r];
		}
	}
	return u;
}

static bool
all_valid(const SiftDvGran *g)
{
	for (int r = 0; r < g->ndead; r++)
		if (!TransactionIdIsValid(g->xmax[r].xmax))
			return false;
	return true;
}

static bool
any_valid(const SiftDvGran *g)
{
	for (int r = 0; r < g->ndead; r++)
		if (TransactionIdIsValid(g->xmax[r].xmax))
			return true;
	return false;
}

/* The bytes of the deleters of the granule in the form that is smallest: 0, one entry, a palette, or one entry for each row */
typedef enum XmaxForm
{
	XM_NONE,
	XM_SINGLE,
	XM_RLE,
	XM_ROWS
} XmaxForm;

static XmaxForm
form_of(const SiftDvGran *g, int *nuniq)
{
	SiftDvXmaxEntry uniq[SIFT_DVG_RLE_MAX_UNIQ + 2];
	int			u;

	*nuniq = 0;
	if (!any_valid(g))
		return XM_NONE;
	u = palette_of(g, uniq);
	if (u == 1 && all_valid(g))
		return XM_SINGLE;
	if (u >= 2 && u <= SIFT_DVG_RLE_MAX_UNIQ && 4 + (uint32) u * sizeof(SiftDvXmaxEntry) + g->ndead < (uint32) g->ndead * sizeof(SiftDvXmaxEntry))
	{
		*nuniq = u;
		return XM_RLE;
	}
	return XM_ROWS;
}

static uint32
xmax_bytes(XmaxForm f, const SiftDvGran *g, int nuniq)
{
	switch (f)
	{
		case XM_SINGLE:
			return sizeof(SiftDvXmaxEntry);
		case XM_RLE:
			return MAXALIGN(4 + nuniq * sizeof(SiftDvXmaxEntry) + g->ndead);
		case XM_ROWS:
			return g->ndead * sizeof(SiftDvXmaxEntry);
		default:
			return 0;
	}
}

static void
bounds(const SiftDvGran *g, const SiftDvOps *ops, FullTransactionId *lo, FullTransactionId *hi)
{
	*lo = *hi = InvalidFullTransactionId;
	for (int r = 0; r < g->ndead; r++)
		if (TransactionIdIsValid(g->xmax[r].xmax))
		{
			FullTransactionId f = ops->fxid(ops->ctx, g->xmax[r].xmax);

			if (!FullTransactionIdIsValid(*lo) || FullTransactionIdPrecedes(f, *lo))
				*lo = f;
			if (!FullTransactionIdIsValid(*hi) || FullTransactionIdFollows(f, *hi))
				*hi = f;
		}
}

static void
write_xmax(char *dst, XmaxForm f, const SiftDvGran *g, int nuniq)
{
	if (f == XM_SINGLE)
		memcpy(dst, g->xmax, sizeof(SiftDvXmaxEntry));
	else if (f == XM_ROWS)
		memcpy(dst, g->xmax, g->ndead * sizeof(SiftDvXmaxEntry));
	else if (f == XM_RLE)
	{
		SiftDvXmaxEntry uniq[SIFT_DVG_RLE_MAX_UNIQ + 2];
		uint16		n = nuniq;
		uint8	   *idx = (uint8 *) dst + 4 + nuniq * sizeof(SiftDvXmaxEntry);

		(void) palette_of(g, uniq);
		memcpy(dst, &n, sizeof(n));
		memcpy(dst + 4, uniq, nuniq * sizeof(SiftDvXmaxEntry));
		for (int r = 0; r < g->ndead; r++)
		{
			int			k = 0;

			while (!same_deleter(&uniq[k], &g->xmax[r]))
				k++;
			idx[r] = (uint8) k;
		}
	}
}

char *
sift_dvd_serialize(const SiftDv *dv, const SiftDvOps *ops, uint32 *len)
{
	uint32		off = sizeof(SiftDvHeaderData) + dv->ndirty * sizeof(SiftDvGranuleEntry);
	SiftDvGranuleEntry *ent = palloc0(Max(dv->ndirty, 1) * sizeof(SiftDvGranuleEntry));
	XmaxForm   *form = palloc(Max(dv->ndirty, 1) * sizeof(XmaxForm));
	int		   *nuniq = palloc(Max(dv->ndirty, 1) * sizeof(int));
	FullTransactionId xmin = InvalidFullTransactionId,
				xmax = InvalidFullTransactionId;
	uint64		ndead = 0;
	uint16		flags = SIFT_DV_FLAG_ALL_SETTLED;
	char	   *buf;
	SiftDvHeaderData *h;

	for (int i = 0; i < dv->ndirty; i++)
	{
		const SiftDvGran *g = &dv->gran[i];
		SiftDvKind	kind = kind_of(g);
		FullTransactionId lo;

		ent[i].granule_id = g->granule_id;
		ent[i].kind = kind;
		ent[i].ndead = g->ndead;
		ndead += g->ndead;
		if (kind == SIFT_DV_ARRAY16)
		{
			ent[i].payload_off = off;
			off += MAXALIGN(g->ndead * sizeof(uint16));
		}
		else if (kind == SIFT_DV_BITMAP)
		{
			ent[i].payload_off = off;
			off += SIFT_DV_BITMAP_BYTES;
		}
		bounds(g, ops, &lo, &ent[i].xid_max);
		form[i] = form_of(g, &nuniq[i]);
		if (form[i] == XM_NONE)
			continue;
		flags |= SIFT_DV_FLAG_HAS_XMAX;
		ent[i].xmax_off = off;
		if (form[i] == XM_SINGLE)
		{
			ent[i].kind |= SIFT_DVG_SINGLE_XMAX;
			flags |= SIFT_DV_FLAG_SINGLE_XMAX;
		}
		else if (form[i] == XM_RLE)
		{
			ent[i].kind |= SIFT_DVG_RLE_XMAX;
			flags |= SIFT_DV_FLAG_RLE_XMAX;
		}
		off += xmax_bytes(form[i], g, nuniq[i]);
		if (!FullTransactionIdIsValid(xmin) || FullTransactionIdPrecedes(lo, xmin))
			xmin = lo;
		if (!FullTransactionIdIsValid(xmax) || FullTransactionIdFollows(ent[i].xid_max, xmax))
			xmax = ent[i].xid_max;
		for (int r = 0; r < g->ndead; r++)
			if (TransactionIdIsValid(g->xmax[r].xmax) && !ops->committed(ops->ctx, g->xmax[r].xmax))
				flags &= ~SIFT_DV_FLAG_ALL_SETTLED;
	}
	buf = palloc0(off);
	h = (SiftDvHeaderData *) buf;
	h->magic = SIFT_DV_MAGIC;
	h->version = SIFT_DV_VERSION;
	h->flags = flags;
	h->ngranules = dv->ngranules;
	h->part_gen = dv->part_gen;
	h->dv_gen = dv->dv_gen;
	h->xid_min = xmin;
	h->xid_max = xmax;
	h->ndead_total = ndead;
	h->ndirty_granules = dv->ndirty;
	h->stream_len = off;
	h->nextents = 1;
	memcpy(buf + sizeof(SiftDvHeaderData), ent, dv->ndirty * sizeof(SiftDvGranuleEntry));
	for (int i = 0; i < dv->ndirty; i++)
	{
		const SiftDvGran *g = &dv->gran[i];
		uint16		kind = ent[i].kind & SIFT_DVG_KIND_MASK;

		if (kind == SIFT_DV_ARRAY16)
			memcpy(buf + ent[i].payload_off, g->rows, g->ndead * sizeof(uint16));
		else if (kind == SIFT_DV_BITMAP)
			put_bitmap(g, (uint8 *) buf + ent[i].payload_off);
		if (ent[i].xmax_off != 0)
			write_xmax(buf + ent[i].xmax_off, form[i], g, nuniq[i]);
	}
	h->crc32c = sift_crc32c(buf + 12, off - 12);
	*len = off;
	pfree(ent);
	pfree(form);
	pfree(nuniq);
	return buf;
}

/* ---- the checksum of a stream with a delta tail ---- */

uint32
sift_dvd_base_crc(const char *bytes, uint32 base_len)
{
	char	   *tmp = palloc(base_len);
	uint32		crc;

	memcpy(tmp, bytes, base_len);
	memset(tmp + offsetof(SiftDvHeaderData, xid_min), 0, 2 * sizeof(FullTransactionId));
	memset(tmp + offsetof(SiftDvHeaderData, ndead_total), 0, sizeof(uint64));
	memset(tmp + offsetof(SiftDvHeaderData, stream_len), 0, sizeof(uint32));
	memset(tmp + offsetof(SiftDvHeaderData, pad), 0, sizeof(uint32));
	crc = sift_crc32c(tmp + 12, base_len - 12);
	pfree(tmp);
	return crc;
}

/* ---- deserialize ---- */

/* The rows the chunks of the tail name for one granule, in the order the chunks come: a row named twice keeps the first deleter */
typedef struct DeltaRow
{
	uint16		row;
	uint32		seq;
	SiftDvXmaxEntry x;
} DeltaRow;

typedef struct DeltaBucket
{
	uint32		gid;
	uint16		grows;
	int			n;
	int			cap;
	DeltaRow   *rows;
	FullTransactionId xid_max;
} DeltaBucket;

typedef struct DeltaBuckets
{
	DeltaBucket *b;
	int			n;
	int			cap;
	int32	   *slots;			/* the place in b plus one, 0: empty; a power of two long, at most half full */
	uint32		mask;
	uint32		seq;
} DeltaBuckets;

static void
buckets_free(DeltaBuckets *bs)
{
	for (int i = 0; i < bs->n; i++)
		pfree(bs->b[i].rows);
	pfree(bs->b);
	pfree(bs->slots);
}

static void
buckets_init(DeltaBuckets *bs)
{
	*bs = (DeltaBuckets) {.cap = 16,.mask = 63};
	bs->b = palloc(bs->cap * sizeof(DeltaBucket));
	bs->slots = palloc0((bs->mask + 1) * sizeof(int32));
}

static void
buckets_rehash(DeltaBuckets *bs)
{
	pfree(bs->slots);
	bs->mask = bs->mask * 2 + 1;
	bs->slots = palloc0((bs->mask + 1) * sizeof(int32));
	for (int i = 0; i < bs->n; i++)
	{
		uint32		h = (uint32) (sift_fmix64(bs->b[i].gid) & bs->mask);

		while (bs->slots[h] != 0)
			h = (h + 1) & bs->mask;
		bs->slots[h] = i + 1;
	}
}

static DeltaBucket *
bucket_of(DeltaBuckets *bs, uint32 gid, uint16 grows)
{
	uint32		h = (uint32) (sift_fmix64(gid) & bs->mask);

	for (; bs->slots[h] != 0; h = (h + 1) & bs->mask)
		if (bs->b[bs->slots[h] - 1].gid == gid)
			return &bs->b[bs->slots[h] - 1];
	if (bs->n == bs->cap)
	{
		bs->cap *= 2;
		bs->b = repalloc(bs->b, bs->cap * sizeof(DeltaBucket));
	}
	bs->b[bs->n] = (DeltaBucket) {.gid = gid,.grows = grows,.cap = 16,.rows = palloc(16 * sizeof(DeltaRow))};
	bs->slots[h] = ++bs->n;
	if ((uint64) bs->n * 2 > (uint64) bs->mask + 1)
		buckets_rehash(bs);
	return &bs->b[bs->n - 1];
}

static void
bucket_add(DeltaBuckets *bs, uint32 gid, uint16 grows, const uint16 *rows, uint32 n, SiftDvXmaxEntry x, const SiftDvOps *ops)
{
	DeltaBucket *b = bucket_of(bs, gid, grows);

	if (b->n + (int) n > b->cap)
	{
		b->cap = Max(b->cap * 2, b->n + (int) n);
		/* check-memory: allow the rows of the chunks of one delta tail, which a fold of the tail bounds */
		b->rows = repalloc(b->rows, b->cap * sizeof(DeltaRow));
	}
	for (uint32 r = 0; r < n; r++)
		b->rows[b->n++] = (DeltaRow) {.row = rows[r],.seq = bs->seq,.x = x};
	bs->seq++;
	if (TransactionIdIsValid(x.xmax))
	{
		FullTransactionId f = ops->fxid(ops->ctx, x.xmax);

		if (!FullTransactionIdIsValid(b->xid_max) || FullTransactionIdFollows(f, b->xid_max))
			b->xid_max = f;
	}
}

static int
delta_row_cmp(const void *a, const void *b)
{
	const DeltaRow *x = a;
	const DeltaRow *y = b;

	if (x->row != y->row)
		return x->row < y->row ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int
bucket_cmp(const void *a, const void *b)
{
	const DeltaBucket *x = a;
	const DeltaBucket *y = b;

	return x->gid < y->gid ? -1 : x->gid > y->gid;
}

/* The rows of the bucket ascending, a row once (its first deleter) */
static void
bucket_settle(DeltaBucket *b)
{
	int			k = 0;

	qsort(b->rows, b->n, sizeof(DeltaRow), delta_row_cmp);
	for (int i = 0; i < b->n; i++)
		if (k == 0 || b->rows[k - 1].row != b->rows[i].row)
			b->rows[k++] = b->rows[i];
	b->n = k;
}

/* The granule of the vector with the rows of the bucket added (a row already dead keeps the deleter it has) */
static void
gran_merge(SiftDvGran *g, const DeltaBucket *b)
{
	uint16	   *nr = palloc((g->ndead + b->n) * sizeof(uint16));
	SiftDvXmaxEntry *nx = palloc((g->ndead + b->n) * sizeof(SiftDvXmaxEntry));
	int			a = 0,
				i = 0,
				k = 0;

	while (a < g->ndead || i < b->n)
	{
		if (i == b->n || (a < g->ndead && g->rows[a] <= b->rows[i].row))
		{
			i += i < b->n && g->rows[a] == b->rows[i].row;
			nr[k] = g->rows[a];
			nx[k++] = g->xmax[a++];
		}
		else
		{
			nr[k] = b->rows[i].row;
			nx[k++] = b->rows[i++].x;
		}
	}
	pfree(g->rows);
	pfree(g->xmax);
	g->rows = nr;
	g->xmax = nx;
	g->ndead = k;
	if (FullTransactionIdIsValid(b->xid_max) && (!FullTransactionIdIsValid(g->xid_max) || FullTransactionIdFollows(b->xid_max, g->xid_max)))
		g->xid_max = b->xid_max;
}

static SiftDvGran
gran_new(const DeltaBucket *b)
{
	SiftDvGran	g = {.granule_id = b->gid,.granule_rows = b->grows,.ndead = b->n,.xid_max = b->xid_max};

	g.rows = palloc(Max(b->n, 1) * sizeof(uint16));
	g.xmax = palloc(Max(b->n, 1) * sizeof(SiftDvXmaxEntry));
	for (int i = 0; i < b->n; i++)
	{
		g.rows[i] = b->rows[i].row;
		g.xmax[i] = b->rows[i].x;
	}
	return g;
}

/* The vector with the rows of every bucket: one merge of the granules of the vector with the buckets, both ascending by granule */
static void
buckets_apply(SiftDv *dv, DeltaBuckets *bs)
{
	SiftDvGran *out = palloc((dv->ndirty + bs->n) * sizeof(SiftDvGran));
	int			a = 0,
				k = 0;

	qsort(bs->b, bs->n, sizeof(DeltaBucket), bucket_cmp);
	for (int i = 0; i < bs->n; i++)
	{
		bucket_settle(&bs->b[i]);
		while (a < dv->ndirty && dv->gran[a].granule_id < bs->b[i].gid)
			out[k++] = dv->gran[a++];
		if (a < dv->ndirty && dv->gran[a].granule_id == bs->b[i].gid)
		{
			gran_merge(&dv->gran[a], &bs->b[i]);
			out[k++] = dv->gran[a++];
		}
		else
			out[k++] = gran_new(&bs->b[i]);
	}
	while (a < dv->ndirty)
		out[k++] = dv->gran[a++];
	pfree(dv->gran);
	dv->gran = out;
	dv->ndirty = k;
}

static const char *
read_granule(SiftDv *dv, const SiftDvGranuleEntry *e, const SiftDvHeaderData *h, const char *bytes, uint32 end, const uint16 *granule_rows)
{
	SiftDvGran *g = &dv->gran[dv->ndirty];
	uint32		nr;
	uint16		kind = e->kind & SIFT_DVG_KIND_MASK;
	bool		single = (e->kind & SIFT_DVG_SINGLE_XMAX) != 0,
				rle = (e->kind & SIFT_DVG_RLE_XMAX) != 0;

	if ((e->kind & ~(SIFT_DVG_KIND_MASK | SIFT_DVG_SINGLE_XMAX | SIFT_DVG_RLE_XMAX)) != 0 || (single && rle))
		return "a granule of the deletion vector has flags that no form of the vector has";
	if ((single && (!(h->flags & SIFT_DV_FLAG_SINGLE_XMAX) || e->xmax_off == 0)) || (rle && (!(h->flags & SIFT_DV_FLAG_RLE_XMAX) || e->xmax_off == 0)))
		return "a granule of the deletion vector keeps its deleters in a form the vector does not say it has";
	if (e->granule_id >= h->ngranules || (dv->ndirty > 0 && e->granule_id <= dv->gran[dv->ndirty - 1].granule_id) || e->ndead == 0)
		return "the deletion vector lists a granule out of order";
	g->granule_id = e->granule_id;
	g->granule_rows = nr = granule_rows[g->granule_id];
	g->ndead = e->ndead;
	g->xid_max = e->xid_max;
	g->rows = palloc(g->ndead * sizeof(uint16));
	g->xmax = palloc0(g->ndead * sizeof(SiftDvXmaxEntry));
	dv->ndirty++;
	if (kind == SIFT_DV_ALL_DEAD)
	{
		if ((uint32) g->ndead != nr)
			return "a granule is all dead with a count that is not its rows";
		for (uint32 r = 0; r < nr; r++)
			g->rows[r] = r;
	}
	else if (kind == SIFT_DV_ARRAY16 && (uint64) e->payload_off + g->ndead * sizeof(uint16) <= end)
		memcpy(g->rows, bytes + e->payload_off, g->ndead * sizeof(uint16));
	else if (kind == SIFT_DV_BITMAP && (uint64) e->payload_off + SIFT_DV_BITMAP_BYTES <= end)
	{
		const uint8 *bm = (const uint8 *) bytes + e->payload_off;
		int			k = 0;

		for (uint32 r = 0; r < nr && k < g->ndead; r++)
			if ((bm[r >> 3] >> (r & 7)) & 1)
				g->rows[k++] = r;
		if (k != g->ndead)
			return "the bitmap of a granule holds another count of dead rows than its entry";
	}
	else
		return "a granule of the deletion vector has a kind or a payload that is not in the vector";
	for (int r = 0; r < g->ndead; r++)
		if (g->rows[r] >= nr || (r > 0 && g->rows[r] <= g->rows[r - 1]))
			return "a granule lists a dead row out of order or out of range";
	if (e->xmax_off == 0)
		return NULL;
	if (rle)
	{
		uint16		nu;
		const uint8 *idx;
		SiftDvXmaxEntry uniq[SIFT_DVG_RLE_MAX_UNIQ];

		if ((uint64) e->xmax_off + 4 > end)
			return "the palette of deleters of a granule lies outside the vector";
		memcpy(&nu, bytes + e->xmax_off, sizeof(nu));
		if (nu < 2 || nu > SIFT_DVG_RLE_MAX_UNIQ || (uint64) e->xmax_off + 4 + nu * sizeof(SiftDvXmaxEntry) + g->ndead > end)
			return "the palette of deleters of a granule has a size that is not one or lies outside the vector";
		memcpy(uniq, bytes + e->xmax_off + 4, nu * sizeof(SiftDvXmaxEntry));
		idx = (const uint8 *) bytes + e->xmax_off + 4 + nu * sizeof(SiftDvXmaxEntry);
		for (int r = 0; r < g->ndead; r++)
		{
			if (idx[r] >= nu)
				return "a dead row names a deleter past the palette of its granule";
			g->xmax[r] = uniq[idx[r]];
		}
		return NULL;
	}
	if ((uint64) e->xmax_off + (single ? 1 : g->ndead) * sizeof(SiftDvXmaxEntry) > end)
		return "the deleters of a granule lie outside the vector";
	for (int r = 0; r < g->ndead; r++)
		memcpy(&g->xmax[r], bytes + e->xmax_off + (single ? 0 : r * sizeof(SiftDvXmaxEntry)), sizeof(SiftDvXmaxEntry));
	return NULL;
}

static const char *
read_tail(SiftDv *dv, const char *bytes, uint32 from, uint32 len, const uint16 *granule_rows, uint32 ngranules, const SiftDvOps *ops)
{
	DeltaBuckets bs;
	const char *why = NULL;

	buckets_init(&bs);
	for (uint32 pos = from; pos < len && why == NULL;)
	{
		SiftDvDeltaChunkHdr c = {0};
		uint32		need = 0;

		if (len - pos < sizeof(c))
			why = "a chunk of the delta tail is cut off";
		else
		{
			memcpy(&c, bytes + pos, sizeof(c));
			need = SIFT_DV_CHUNK_BYTES(c.nmarks);
		}
		if (why == NULL && (c.magic != SIFT_DV_DELTA_MAGIC || c.nmarks == 0 || c.nmarks > SIFT_DV_DELTA_MAX_MARKS || c.granule_id >= ngranules || (c.chunk_flags & ~SIFT_DV_DELTA_SETTLED) != 0))
			why = "a chunk of the delta tail has no magic, no rows, a granule that is not in the part or flags that are not its";
		if (why == NULL && (need > len - pos || ((c.chunk_flags & SIFT_DV_DELTA_SETTLED) != 0 && (c.xmax != InvalidTransactionId || c.cmax != 0))))
			why = "a chunk of the delta tail lies outside the vector or is settled and has a deleter";
		if (why == NULL)
		{
			uint16	   *rows = palloc(c.nmarks * sizeof(uint16));

			memcpy(rows, bytes + pos + sizeof(c), c.nmarks * sizeof(uint16));
			for (uint32 r = 0; r < c.nmarks && why == NULL; r++)
				if (rows[r] >= granule_rows[c.granule_id] || (r > 0 && rows[r] <= rows[r - 1]))
					why = "a chunk of the delta tail lists a row out of order or out of range";
			if (why == NULL)
				bucket_add(&bs, c.granule_id, granule_rows[c.granule_id], rows, c.nmarks, (SiftDvXmaxEntry) {c.xmax, c.cmax}, ops);
			pfree(rows);
			pos += need;
		}
	}
	if (why == NULL)
		buckets_apply(dv, &bs);
	buckets_free(&bs);
	return why;
}

SiftDv *
sift_dvd_deserialize(const char *bytes, uint32 len, uint64 part_gen, uint64 dv_gen, const uint16 *granule_rows, const SiftDvOps *ops, const char **why)
{
	const SiftDvHeaderData *h = (const SiftDvHeaderData *) bytes;
	const SiftDvGranuleEntry *ent = (const SiftDvGranuleEntry *) (bytes + sizeof(SiftDvHeaderData));
	SiftDv	   *dv;
	bool		tail;
	uint32		base_len;

	*why = NULL;
	if (len < sizeof(SiftDvHeaderData) || h->magic != SIFT_DV_MAGIC || h->version != SIFT_DV_VERSION || h->stream_len != len)
		*why = "the deletion vector has the wrong magic or size";
	else if ((h->flags & SIFT_DV_FLAG_DELTA_TAIL) != 0 && (h->pad < sizeof(SiftDvHeaderData) || h->pad > len || (h->pad & 3) != 0))
		*why = "the delta tail of the deletion vector starts outside it";
	if (*why != NULL)
		return NULL;
	tail = (h->flags & SIFT_DV_FLAG_DELTA_TAIL) != 0;
	base_len = tail ? h->pad : len;
	if (h->crc32c != (tail ? sift_dvd_base_crc(bytes, base_len) : sift_crc32c(bytes + 12, len - 12)))
		*why = "the deletion vector has the wrong checksum";
	else if (h->part_gen != part_gen || h->dv_gen != dv_gen || sizeof(SiftDvHeaderData) + (uint64) h->ndirty_granules * sizeof(SiftDvGranuleEntry) > base_len)
		*why = "the deletion vector belongs to another part or generation";
	if (*why != NULL)
		return NULL;
	dv = palloc0(sizeof(SiftDv));
	dv->part_gen = part_gen;
	dv->dv_gen = dv_gen;
	dv->ngranules = h->ngranules;
	dv->gran = palloc0(Max(h->ndirty_granules, 1) * sizeof(SiftDvGran));
	for (uint32 i = 0; i < h->ndirty_granules && *why == NULL; i++)
		*why = read_granule(dv, &ent[i], h, bytes, base_len, granule_rows);
	if (*why == NULL && tail)
		*why = read_tail(dv, bytes, base_len, len, granule_rows, h->ngranules, ops);
	if (*why != NULL)
	{
		for (int i = 0; i < dv->ndirty; i++)
		{
			pfree(dv->gran[i].rows);
			pfree(dv->gran[i].xmax);
		}
		pfree(dv->gran);
		pfree(dv);
		return NULL;
	}
	return dv;
}

/* ---- the delta tail of a page ---- */

uint32
sift_dvd_chunks_bytes(const SiftDvDeltaGran *g, int ng)
{
	uint32		n = 0;

	for (int i = 0; i < ng; i++)
		for (uint32 left = g[i].nrows; left > 0; left -= Min(left, SIFT_DV_DELTA_MAX_MARKS))
			n += SIFT_DV_CHUNK_BYTES(Min(left, SIFT_DV_DELTA_MAX_MARKS));
	return n;
}

bool
sift_dvd_page_takes(const char *pg, uint32 chunk_bytes)
{
	const SiftDvHeaderData *h = (const SiftDvHeaderData *) SiftPageGetData(pg);

	return sift_page_layout_ok(pg) && h->magic == SIFT_DV_MAGIC && h->stream_len == SiftPageGetUsed(pg) && h->ngranules <= UINT16_MAX &&
		(h->flags & SIFT_DV_FLAG_ALL_SETTLED) == 0 && h->stream_len % 4 == 0 && h->stream_len + chunk_bytes <= SIFT_PAGE_PAYLOAD;
}

void
sift_dvd_page_append(char *pg, const SiftDvDeltaGran *g, int ng, TransactionId xmax, CommandId cmax, FullTransactionId fxid)
{
	SiftDvHeaderData *h = DV_HDR(pg);
	bool		first = (h->flags & SIFT_DV_FLAG_DELTA_TAIL) == 0;
	uint32		base_len = h->stream_len,
				pos = h->stream_len;
	uint64		nmarks = 0;

	Assert(sift_dvd_page_takes(pg, sift_dvd_chunks_bytes(g, ng)));
	for (int i = 0; i < ng; i++)
	{
		Assert(g[i].nrows > 0);
		for (uint32 from = 0, n; from < g[i].nrows; from += n)
		{
			SiftDvDeltaChunkHdr c = {.magic = SIFT_DV_DELTA_MAGIC,.granule_id = g[i].granule_id,
				.chunk_flags = TransactionIdIsValid(xmax) ? 0 : SIFT_DV_DELTA_SETTLED,.xmax = xmax,.cmax = cmax};

			c.nmarks = n = Min(g[i].nrows - from, SIFT_DV_DELTA_MAX_MARKS);
			memcpy(SiftPageGetData(pg) + pos, &c, sizeof(c));
			memcpy(SiftPageGetData(pg) + pos + sizeof(c), g[i].rows + from, n * sizeof(uint16));
			pos += SIFT_DV_CHUNK_BYTES(n);
		}
		nmarks += g[i].nrows;
	}
	h->ndead_total += nmarks;
	if (TransactionIdIsValid(xmax))
	{
		if (!FullTransactionIdIsValid(h->xid_min) || FullTransactionIdPrecedes(fxid, h->xid_min))
			h->xid_min = fxid;
		if (!FullTransactionIdIsValid(h->xid_max) || FullTransactionIdFollows(fxid, h->xid_max))
			h->xid_max = fxid;
		h->flags |= SIFT_DV_FLAG_HAS_XMAX;
	}
	h->stream_len = pos;
	SiftPageSetUsed(pg, pos);
	if (first)
	{
		h->pad = base_len;
		h->flags |= SIFT_DV_FLAG_DELTA_TAIL;
		h->crc32c = sift_dvd_base_crc(SiftPageGetData(pg), base_len);
	}
}

#ifndef SIFT_HREF_STANDALONE

/* ---- the server side: buffers, records, the manifest ---- */

static bool
horizon_committed(void *ctx, TransactionId xmax)
{
	SiftHorizonStatus st = sift_xid_horizon_status(&((SiftDvHorizon *) ctx)->hc, xmax);

	return st == SIFT_H_COMMITTED_OLD || st == SIFT_H_COMMITTED_NEW;
}

static FullTransactionId
horizon_fxid(void *ctx, TransactionId xmax)
{
	return sift_xid_fxid_at(((SiftDvHorizon *) ctx)->hc.next, xmax);
}

void
sift_dvd_horizon_open(SiftDvHorizon *h)
{
	sift_xid_horizon_init(&h->hc, InvalidFullTransactionId);
	h->ops = (SiftDvOps) {.ctx = h,.committed = horizon_committed,.fxid = horizon_fxid};
}

void
sift_dv_append_delta_batch(Relation rel, uint32 part_id, const SiftDvDeltaPart *parts, int n, TransactionId xmax, CommandId cmax, bool *taken)
{
	FullTransactionId fxid = TransactionIdIsValid(xmax) ? sift_xid_fxid_at(ReadNextFullTransactionId(), xmax) : InvalidFullTransactionId;

	for (int i = 0; i < n;)
	{
		SiftXlog	x = sift_xlog_start(rel);
		int			in = 0,
					j;

		for (j = i; j < n && in < MAX_GENERIC_XLOG_PAGES; j++)
		{
			const SiftPartDesc *d = parts[j].desc;
			uint32		bytes = sift_dvd_chunks_bytes(parts[j].gran, parts[j].ngran);
			Buffer		buf;

			taken[j] = false;
			if (!sift_dvd_part_takes(d))
				continue;
			buf = sift_blk_read(rel, d->dv_blk, BUFFER_LOCK_EXCLUSIVE);
			sift_page_expect(rel, BufferGetPage(buf), d->dv_blk, SIFT_PT_DELVEC, d->dv_gen, SIFT_PART_ID_ANY);
			if (!sift_dvd_page_takes(BufferGetPage(buf), bytes))
			{
				UnlockReleaseBuffer(buf);
				continue;
			}
			sift_dvd_page_append(sift_xlog_register(&x, buf, 0), parts[j].gran, parts[j].ngran, xmax, cmax, fxid);
			taken[j] = true;
			in++;
		}
		if (in > 0)
			sift_xlog_done(&x);
		else
			sift_xlog_abort(&x);
		i = j;
	}
}

typedef struct QPage
{
	BlockNumber blk;
	const char *bytes;
	uint32		n;
	uint64		gen;
	uint32		part_id;
} QPage;

static void
write_queued(Relation rel, const QPage *q, int nq)
{
	SiftXlog	x = sift_xlog_start(rel);

	for (int i = 0; i < nq; i++)
	{
		Page		pg = sift_xlog_register(&x, sift_blk_new(rel, q[i].blk), GENERIC_XLOG_FULL_IMAGE);

		sift_page_init_part(pg, SIFT_PT_DELVEC, q[i].gen, q[i].part_id);
		memcpy(SiftPageGetData(pg), q[i].bytes, q[i].n);
		SiftPageSetUsed(pg, q[i].n);
	}
	sift_xlog_done(&x);
}

void
sift_dv_write_batch(Relation rel, uint32 part_id, const SiftDv *const *dvs, SiftPartDesc **descs, int n)
{
	SiftDvHorizon hz;
	QPage		q[MAX_GENERIC_XLOG_PAGES];
	char	  **streams = palloc(Max(n, 1) * sizeof(char *));
	int			nq = 0;

	sift_dvd_horizon_open(&hz);
	for (int i = 0; i < n; i++)
	{
		uint32		len;
		char	   *bytes = streams[i] = sift_dvd_serialize(dvs[i], &hz.ops, &len);
		const SiftDvHeaderData *h = (const SiftDvHeaderData *) bytes;
		uint32		npages = SIFT_PAYLOAD_PAGES(len);
		SiftExtent	ext = npages == 1 ? (SiftExtent) {sift_fsm_alloc_meta_page(rel), 1} : sift_fsm_alloc(rel, npages);

		for (uint32 p = 0; p < npages; p++)
		{
			uint32		from = SIFT_PAYLOAD_BYTES(p);

			if (nq == MAX_GENERIC_XLOG_PAGES)
			{
				write_queued(rel, q, nq);
				nq = 0;
			}
			q[nq++] = (QPage) {ext.start + p, bytes + from, Min(len - from, (uint32) SIFT_PAGE_PAYLOAD), dvs[i]->dv_gen, part_id};
		}
		descs[i]->dv_blk = ext.start;
		descs[i]->dv_nblocks = ext.nblocks;
		descs[i]->dv_gen = dvs[i]->dv_gen;
		descs[i]->dv_max_xid = h->xid_max;
		descs[i]->ndead = h->ndead_total;
		descs[i]->flags = (descs[i]->flags & ~SIFT_PART_FLAG_DV_SETTLED) | ((h->flags & SIFT_DV_FLAG_ALL_SETTLED) ? SIFT_PART_FLAG_DV_SETTLED : 0);
	}
	if (nq > 0)
		write_queued(rel, q, nq);
	for (int i = 0; i < n; i++)
		pfree(streams[i]);
	pfree(streams);
}

uint32
sift_dv_stream_len(Relation rel, BlockNumber dv_blk)
{
	Buffer		buf = sift_blk_read(rel, dv_blk, BUFFER_LOCK_SHARE);
	uint32		len = DV_HDR(BufferGetPage(buf))->stream_len;

	UnlockReleaseBuffer(buf);
	return len;
}

SiftChainStamp
sift_dv_stamp(Relation rel, BlockNumber dv_blk)
{
	Buffer		buf = sift_blk_read(rel, dv_blk, BUFFER_LOCK_SHARE);
	const SiftDvHeaderData *h = DV_HDR(BufferGetPage(buf));
	SiftChainStamp c = {.lsn = PageGetLSN(BufferGetPage(buf)),.count = h->ndead_total,.pages = h->stream_len,.max_xid = h->xid_max};

	UnlockReleaseBuffer(buf);
	return c;
}

uint64
sift_dv_part_ndead(Relation rel, const SiftPartDesc *d)
{
	return d->dv_blk != InvalidBlockNumber && d->dv_nblocks == 1 ? sift_dv_stamp(rel, d->dv_blk).count : d->ndead;
}

#define SIFT_DV_CONSOLIDATE_TAIL_BYTES	1024

static bool
has_tail(Relation rel, const SiftPartDesc *d)
{
	Buffer		buf;
	const SiftDvHeaderData *h;
	bool		tail;

	if (d->dv_blk == InvalidBlockNumber || d->dv_nblocks != 1)
		return false;
	buf = sift_blk_read(rel, d->dv_blk, BUFFER_LOCK_SHARE);
	h = DV_HDR(BufferGetPage(buf));
	/* STOR[STOR-F2-06]: consolidate a delvec once its delta tail holds 1024 B or the page is 3/4 full (no page rewrite for a 16 B delta) */
	tail = (h->flags & SIFT_DV_FLAG_DELTA_TAIL) != 0 && (h->stream_len - h->pad >= SIFT_DV_CONSOLIDATE_TAIL_BYTES || h->stream_len > SIFT_PAGE_PAYLOAD / 4 * 3);
	UnlockReleaseBuffer(buf);
	return tail;
}

int
sift_dv_consolidate(Relation rel, uint64 part_id, const uint8 *key_lo)
{
	SiftManifestRead mr;
	SiftPartDesc *parts,
			   *folded;
	SiftExtent *rel_ext;
	SiftDv	  **dvs;
	SiftPartDesc **descs;
	int			np,
				nf = 0;
	uint64		gen;
	SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo};

	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	parts = sift_manifest_read_parts(rel, &mr.entries[0], &np);
	folded = palloc(Max(np, 1) * sizeof(SiftPartDesc));
	rel_ext = palloc(Max(np, 1) * sizeof(SiftExtent));
	dvs = palloc(Max(np, 1) * sizeof(SiftDv *));
	descs = palloc(Max(np, 1) * sizeof(SiftPartDesc *));
	for (int p = 0; p < np; p++)
		if (has_tail(rel, &parts[p]))
		{
			SiftPart   *part = sift_part_open_desc(rel, &parts[p], (uint32) part_id);
			SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
			uint16	   *grows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));

			for (uint32 g = 0; g < part->hdr.ngranules; g++)
				grows[g] = dir[g].nrows;
			dvs[nf] = sift_dv_read(rel, &parts[p], (uint32) part_id, grows);
			folded[nf] = parts[p];
			rel_ext[nf] = (SiftExtent) {parts[p].dv_blk, parts[p].dv_nblocks};
			nf++;
		}
	if (nf > 0)
	{
		gen = sift_manifest_new_gen(rel, nf);
		for (int i = 0; i < nf; i++)
		{
			dvs[i]->dv_gen = gen + i;
			descs[i] = &folded[i];
		}
		sift_dv_write_batch(rel, (uint32) part_id, (const SiftDv *const *) dvs, descs, nf);
		e.update_parts = folded;
		e.nupdate = nf;
		e.release = rel_ext;
		e.nrelease = nf;
		sift_manifest_commit(rel, &e);
	}
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return nf;
}

#endif							/* !SIFT_HREF_STANDALONE */
