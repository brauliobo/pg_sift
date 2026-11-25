/*
 * col/f2/del/delvec_write.c - the serialized form of a deletion vector (col/f2/del/delvec_delta.h), made one granule at a time: from the rows of a granule in memory, or from the bytes of the
 * granule in another stream, which a merge that does not touch the granule copies as they are.
 */
#include "core/host.h"

#include "col/f2/del/delvec_delta.h"
#include "core/f2/meta.h"
#include "core/hash.h"

#define PAY_LEAD	sizeof(uint64)

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
xmax_bytes(XmaxForm f, int ndead, int nuniq)
{
	switch (f)
	{
		case XM_SINGLE:
			return sizeof(SiftDvXmaxEntry);
		case XM_RLE:
			return MAXALIGN(4 + nuniq * sizeof(SiftDvXmaxEntry) + ndead);
		case XM_ROWS:
			return ndead * sizeof(SiftDvXmaxEntry);
		default:
			return 0;
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

struct SiftDvBuilder
{
	const SiftDvOps *ops;
	uint32		ngranules;
	uint64		part_gen;
	uint64		dv_gen;
	uint64		ndead;
	SiftDvGranuleEntry *ent;
	uint32		nent;
	uint32		capent;
	char	   *pay;			/* what the entries point to, after the entries: it starts with a word that no offset names, so that an offset of 0 is none */
	uint32		pay_len;
	uint32		pay_cap;
	FullTransactionId xmin;
	FullTransactionId xmax;
	uint16		flags;
};

SiftDvBuilder *
sift_dvb_new(const SiftDvOps *ops, uint32 ngranules, uint64 part_gen, uint64 dv_gen)
{
	SiftDvBuilder *b = palloc0(sizeof(*b));

	b->ops = ops;
	b->ngranules = ngranules;
	b->part_gen = part_gen;
	b->dv_gen = dv_gen;
	b->flags = SIFT_DV_FLAG_ALL_SETTLED;
	b->pay_cap = 1024;
	b->pay = palloc0(b->pay_cap);
	b->pay_len = PAY_LEAD;
	return b;
}

static uint32
pay_take(SiftDvBuilder *b, uint32 n)
{
	uint32		at = b->pay_len;

	if (at + n > b->pay_cap)
	{
		uint32		cap = Max(b->pay_cap * 2, at + n);

		b->pay = repalloc(b->pay, cap);
		memset(b->pay + b->pay_cap, 0, cap - b->pay_cap);
		b->pay_cap = cap;
	}
	b->pay_len += n;
	return at;
}

static SiftDvGranuleEntry *
ent_take(SiftDvBuilder *b, uint32 granule_id, uint16 kind, uint16 ndead)
{
	if (b->nent == b->capent)
	{
		b->capent = b->capent == 0 ? 16 : b->capent * 2;
		b->ent = b->ent == NULL ? palloc(b->capent * sizeof(*b->ent)) : repalloc(b->ent, b->capent * sizeof(*b->ent));
	}
	b->ent[b->nent] = (SiftDvGranuleEntry) {.granule_id = granule_id,.kind = kind,.ndead = ndead};
	b->ndead += ndead;
	return &b->ent[b->nent++];
}

/* The granule keeps the deleters at recs (n of them): its greatest is its xid_max, and with the least it bounds the vector; one that is not committed leaves the vector unsettled */
static void
note_deleters(SiftDvBuilder *b, SiftDvGranuleEntry *e, const char *recs, uint32 n)
{
	FullTransactionId lo = InvalidFullTransactionId;

	for (uint32 i = 0; i < n; i++)
	{
		SiftDvXmaxEntry x;
		FullTransactionId f;

		memcpy(&x, recs + i * sizeof(x), sizeof(x));
		if (!TransactionIdIsValid(x.xmax))
			continue;
		f = b->ops->fxid(b->ops->ctx, x.xmax);
		if (!FullTransactionIdIsValid(lo) || FullTransactionIdPrecedes(f, lo))
			lo = f;
		if (!FullTransactionIdIsValid(e->xid_max) || FullTransactionIdFollows(f, e->xid_max))
			e->xid_max = f;
		if (!b->ops->committed(b->ops->ctx, x.xmax))
			b->flags &= ~SIFT_DV_FLAG_ALL_SETTLED;
	}
	b->flags |= SIFT_DV_FLAG_HAS_XMAX | ((e->kind & SIFT_DVG_SINGLE_XMAX) ? SIFT_DV_FLAG_SINGLE_XMAX : 0) | ((e->kind & SIFT_DVG_RLE_XMAX) ? SIFT_DV_FLAG_RLE_XMAX : 0);
	if (!FullTransactionIdIsValid(b->xmin) || FullTransactionIdPrecedes(lo, b->xmin))
		b->xmin = lo;
	if (!FullTransactionIdIsValid(b->xmax) || FullTransactionIdFollows(e->xid_max, b->xmax))
		b->xmax = e->xid_max;
}

void
sift_dvb_gran(SiftDvBuilder *b, const SiftDvGran *g)
{
	SiftDvKind	kind = kind_of(g);
	int			nuniq;
	XmaxForm	form = form_of(g, &nuniq);
	SiftDvGranuleEntry *e = ent_take(b, g->granule_id, kind, g->ndead);

	if (kind == SIFT_DV_ARRAY16)
	{
		e->payload_off = pay_take(b, MAXALIGN(g->ndead * sizeof(uint16)));
		memcpy(b->pay + e->payload_off, g->rows, g->ndead * sizeof(uint16));
	}
	else if (kind == SIFT_DV_BITMAP)
	{
		e->payload_off = pay_take(b, SIFT_DV_BITMAP_BYTES);
		put_bitmap(g, (uint8 *) b->pay + e->payload_off);
	}
	if (form == XM_NONE)
		return;
	e->xmax_off = pay_take(b, xmax_bytes(form, g->ndead, nuniq));
	write_xmax(b->pay + e->xmax_off, form, g, nuniq);
	e->kind |= form == XM_SINGLE ? SIFT_DVG_SINGLE_XMAX : form == XM_RLE ? SIFT_DVG_RLE_XMAX : 0;
	note_deleters(b, e, (const char *) g->xmax, g->ndead);
}

void
sift_dvb_raw(SiftDvBuilder *b, const SiftDvGranuleEntry *src, const char *bytes)
{
	SiftDvGranuleEntry *e = ent_take(b, src->granule_id, src->kind, src->ndead);
	uint16		kind = src->kind & SIFT_DVG_KIND_MASK;
	uint32		npay = kind == SIFT_DV_ARRAY16 ? MAXALIGN(src->ndead * sizeof(uint16)) : kind == SIFT_DV_BITMAP ? SIFT_DV_BITMAP_BYTES : 0;
	const char *x = bytes + src->xmax_off;
	uint16		nu = 0;
	uint32		nrec = src->ndead,
				nbytes;
	XmaxForm	form = (src->kind & SIFT_DVG_SINGLE_XMAX) ? XM_SINGLE : (src->kind & SIFT_DVG_RLE_XMAX) ? XM_RLE : XM_ROWS;

	if (npay > 0)
	{
		e->payload_off = pay_take(b, npay);
		memcpy(b->pay + e->payload_off, bytes + src->payload_off, npay);
	}
	if (src->xmax_off == 0)
		return;
	if (form == XM_RLE)
		memcpy(&nu, x, sizeof(nu));
	nbytes = xmax_bytes(form, src->ndead, nu);
	e->xmax_off = pay_take(b, nbytes);
	memcpy(b->pay + e->xmax_off, x, nbytes);
	if (form == XM_SINGLE)
		nrec = 1;
	else if (form == XM_RLE)
	{
		nrec = nu;
		x += 4;
	}
	note_deleters(b, e, x, nrec);
}

char *
sift_dvb_finish(SiftDvBuilder *b, uint32 *len)
{
	const uint32 base = sizeof(SiftDvHeaderData) + b->nent * sizeof(SiftDvGranuleEntry),
				total = base + b->pay_len - PAY_LEAD;
	char	   *buf = palloc0(total);
	SiftDvHeaderData *h = (SiftDvHeaderData *) buf;

	for (uint32 i = 0; i < b->nent; i++)
	{
		b->ent[i].payload_off += b->ent[i].payload_off != 0 ? base - PAY_LEAD : 0;
		b->ent[i].xmax_off += b->ent[i].xmax_off != 0 ? base - PAY_LEAD : 0;
	}
	h->magic = SIFT_DV_MAGIC;
	h->version = SIFT_DV_VERSION;
	h->flags = b->flags;
	h->ngranules = b->ngranules;
	h->part_gen = b->part_gen;
	h->dv_gen = b->dv_gen;
	h->xid_min = b->xmin;
	h->xid_max = b->xmax;
	h->ndead_total = b->ndead;
	h->ndirty_granules = b->nent;
	h->stream_len = total;
	h->nextents = 1;
	if (b->nent > 0)
		memcpy(buf + sizeof(*h), b->ent, b->nent * sizeof(SiftDvGranuleEntry));
	memcpy(buf + base, b->pay + PAY_LEAD, b->pay_len - PAY_LEAD);
	h->crc32c = sift_crc32c(buf + 12, total - 12);
	*len = total;
	if (b->ent != NULL)
		pfree(b->ent);
	pfree(b->pay);
	pfree(b);
	return buf;
}

char *
sift_dvd_serialize(const SiftDv *dv, const SiftDvOps *ops, uint32 *len)
{
	SiftDvBuilder *b = sift_dvb_new(ops, dv->ngranules, dv->part_gen, dv->dv_gen);

	for (int i = 0; i < dv->ndirty; i++)
		sift_dvb_gran(b, &dv->gran[i]);
	return sift_dvb_finish(b, len);
}
