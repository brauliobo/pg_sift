/*
 * agg/wide.c - the dictionary of a wide leaf (agg/wide.h). An entry is the datum as PostgreSQL holds it, display scale included
 * (1.00 stays 1.00): the varlena of the numeric, the 16 bytes of the Interval, each padded to 8 bytes so that an entry is the datum itself.
 * Two values that are equal but spelled differently (1.0 and 1.00, 1 month and 30 days) are two entries; the executor decides what
 * equal values mean with the type's own equality.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "agg/wide.h"
#include "col/am.h"
#include "col/wide.h"

static uint32
key_hash(const uint64 *k)
{
	uint64		h = (k[0] * UINT64CONST(0x9E3779B97F4A7C15)) ^ (k[1] * UINT64CONST(0xC2B2AE3D27D4EB4F)) ^ (k[2] * UINT64CONST(0x165667B19E3779F9));

	return (uint32) ((h ^ (h >> 31)) * UINT64CONST(0xD6E8FEB86659FD93) >> 32);
}

static void
rows_room(SiftAggWide *w, uint32 n)
{
	if (w->cap >= n)
		return;
	if (w->codes)
	{
		pfree(w->codes);
		pfree(w->isnull);
	}
	w->cap = Max(n, 1024);
	w->codes = MemoryContextAlloc(w->cxt, w->cap * sizeof(uint32));
	w->isnull = MemoryContextAlloc(w->cxt, w->cap);
}

static void
table_reset(SiftAggWide *w, uint32 n)
{
	uint32		nslot = 64;

	while (nslot < 2 * Max(n, 1))
		nslot *= 2;
	if (w->nslot < nslot)
	{
		if (w->slot)
			pfree(w->slot);
		w->nslot = nslot;
		w->slot = MemoryContextAlloc(w->cxt, nslot * sizeof(uint32));
	}
	memset(w->slot, 0, w->nslot * sizeof(uint32));
	w->nd = 0;
	w->blen = 0;
	if (w->off == NULL)
	{
		w->dcap = 256;
		w->off = MemoryContextAlloc(w->cxt, (w->dcap + 1) * sizeof(uint32));
		w->key = MemoryContextAlloc(w->cxt, w->dcap * 3 * sizeof(uint64));
		w->bcap = 4096;
		w->bytes = MemoryContextAlloc(w->cxt, w->bcap);
	}
	w->off[0] = 0;
}

/* the entry (1..) of the value whose words are k, 0 when it has none yet; *at is where it goes */
static uint32
find(const SiftAggWide *w, const uint64 *k, uint32 *at)
{
	uint32		mask = w->nslot - 1,
				h = key_hash(k) & mask,
				e;

	for (; (e = w->slot[h]) != 0; h = (h + 1) & mask)
		if (memcmp(&w->key[(size_t) (e - 1) * 3], k, 3 * sizeof(uint64)) == 0)
			return e;
	*at = h;
	return 0;
}

/* a new entry of the value whose words are k, at the slot find gave */
static uint32
add(SiftAggWide *w, const uint64 *k, uint32 at, const char *bytes, uint32 len)
{
	if (w->nd == w->dcap)
	{
		w->dcap *= 2;
		w->off = repalloc(w->off, (w->dcap + 1) * sizeof(uint32));
		w->key = repalloc(w->key, w->dcap * 3 * sizeof(uint64));
	}
	if (w->blen + len + 8 > w->bcap)
	{
		while (w->blen + len + 8 > w->bcap)
			w->bcap *= 2;
		w->bytes = repalloc(w->bytes, w->bcap);
	}
	memcpy(w->bytes + w->blen, bytes, len);
	memset(w->bytes + w->blen + len, 0, -len & 7);
	w->blen += TYPEALIGN(8, len);
	memcpy(&w->key[(size_t) w->nd * 3], k, 3 * sizeof(uint64));
	w->off[w->nd + 1] = (uint32) w->blen;
	w->slot[at] = ++w->nd;
	return w->nd;
}

/* rows ahead whose dictionary entry is prefetched */
#define WIDE_AHEAD	16

static const uint32 pow10_pad[4] = {1, 10, 100, 1000};

/* the numeric v / 10^scale in the short format numeric_in gives it (4 bytes of length, a 2 byte header, the base 10000 digits), 0 when it needs the long one */
static uint32
w128_numeric(sift_w128 v, int scale, uint8 *out)
{
	sift_wu128	u = v < 0 ? 0 - (sift_wu128) v : (sift_wu128) v;
	int			frac = (scale + 3) / 4,
				pad = frac * 4 - scale,
				n = 0,
				top,
				lo = 0,
				weight;
	uint16		dig[16],
				hdr;

	if (scale < 0 || scale > 63 || u > ~(sift_wu128) 0 / pow10_pad[pad])
		return 0;
	u *= pow10_pad[pad];
	do
	{
		dig[n++] = (uint16) (u % 10000);
		u /= 10000;
	} while (u != 0);
	while (n < frac)
		dig[n++] = 0;
	for (top = n; top > 0 && dig[top - 1] == 0; top--)
		;
	while (lo < top && dig[lo] == 0)
		lo++;
	weight = top == lo ? 0 : top - 1 - frac;
	if (weight < -64 || weight > 63)
		return 0;
	hdr = (uint16) (0x8000 | (v < 0 && top > lo ? 0x2000 : 0) | scale << 7 | (weight < 0 ? 0x0040 : 0) | (weight & 0x003F));
	SET_VARSIZE(out, VARHDRSZ + sizeof(uint16) + (top - lo) * sizeof(uint16));
	memcpy(out + VARHDRSZ, &hdr, sizeof(hdr));
	for (int i = top - 1, o = VARHDRSZ + sizeof(uint16); i >= lo; i--, o += sizeof(uint16))
		memcpy(out + o, &dig[i], sizeof(uint16));
	return VARSIZE(out);
}

/* the digits of a finite number an image holds that the short format below takes in; more are spelled through the text */
#define IMG_DIGITS_MAX	64

/*
 * The numeric of an image and its display scale in the short format numeric_in gives it, without spelling the number as text. 0 when the number
 * is NaN or an infinity, has more than IMG_DIGITS_MAX digits, more fraction digits than dscale, or a weight or scale the short format lacks.
 */
static uint32
image_numeric(const uint8 *img, uint32 len, int32 dscale, uint8 *out)
{
	uint8		dig[IMG_DIGITS_MAX];
	uint16		grp[IMG_DIGITS_MAX / 4 + 4];
	int32		exp10 = 0,
				p,
				weight,
				top,
				last;
	uint32		nd = 0,
				ng = 0;
	bool		neg = false;
	int			kind = sift_col_numimg_digits(img, len, &neg, &exp10, dig, IMG_DIGITS_MAX, &nd);
	uint16		hdr;

	if (kind == 0 || dscale < 0 || dscale > 63)
		return 0;
	if (kind == 1)
	{
		SET_VARSIZE(out, VARHDRSZ + sizeof(uint16));
		hdr = (uint16) (0x8000 | dscale << 7);
		memcpy(out + VARHDRSZ, &hdr, sizeof(hdr));
		return VARSIZE(out);
	}
	/* digit k sits at the power 10^(p - k); NBASE digit g spans the powers 10^(top - 4g) .. 10^(top - 4g - 3) */
	p = exp10 - 1;
	weight = p >= 0 ? p / 4 : -((-p + 3) / 4);
	top = 4 * weight + 3;
	last = p - (int32) nd + 1;
	if (last < -dscale || weight < -64 || weight > 63)
		return 0;
	ng = (uint32) ((top - last) / 4) + 1;
	memset(grp, 0, ng * sizeof(uint16));
	for (uint32 k = 0; k < nd; k++)
	{
		int32		at = top - (p - (int32) k);

		grp[at / 4] += dig[k] * (uint16) pow10_pad[3 - at % 4];
	}
	while (ng > 0 && grp[ng - 1] == 0)
		ng--;
	hdr = (uint16) (0x8000 | (neg ? 0x2000 : 0) | dscale << 7 | (weight < 0 ? 0x0040 : 0) | (weight & 0x003F));
	SET_VARSIZE(out, VARHDRSZ + sizeof(uint16) + ng * sizeof(uint16));
	memcpy(out + VARHDRSZ, &hdr, sizeof(hdr));
	memcpy(out + VARHDRSZ + sizeof(uint16), grp, ng * sizeof(uint16));
	return VARSIZE(out);
}

/* the varlena of the numeric of a decimal text */
static uint32
text_numeric(const char *txt, uint8 *out)
{
	Datum		d = DirectFunctionCall3(numeric_in, CStringGetDatum(txt), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	uint32		len = VARSIZE_ANY(DatumGetPointer(d));

	memcpy(out, DatumGetPointer(d), len);
	pfree(DatumGetPointer(d));
	return len;
}

void
sift_aggwide_build(SiftAggWide *w, const SiftColDef *def, const SiftColVec *lanes, uint32 n)
{
	bool		interval = def->type == SIFT_COLT_INTERVAL,
				image = def->store == SIFT_COLS_STR && def->type == SIFT_COLT_NUMERIC,
				opaque = def->store == SIFT_COLS_STR && !image;
	char		txt[SIFT_COL_W128_TEXT];

	if (w->cxt == NULL)
		w->cxt = CurrentMemoryContext;
	if (image && w->txt == NULL)
		w->txt = MemoryContextAlloc(w->cxt, SIFT_NIMG_TEXT_MAX);
	rows_room(w, n);
	table_reset(w, n);
	w->n = n;
	w->has_null = false;
	for (uint32 i = 0; i < n; i++)
	{
		uint64		k[3] = {0, 0, 0};
		bool		isnull = lanes[0].nulls && (lanes[0].nulls[i >> 6] >> (i & 63) & 1),
					nan = isnull && !opaque && lanes[0].ovf && (lanes[0].ovf[i >> 6] >> (i & 63) & 1);
		uint32		at = 0,
					e;

		/* the strings of a dictionary of millions of entries are cache misses (the images of a segment of 10,000,000 almost unique numbers: 975 cycles a row, 486 in segments of a tenth of that): the offset of a row 2 steps ahead is fetched, then its string */
		if ((image || opaque) && i + 2 * WIDE_AHEAD < n)
		{
			const SiftColDict *d = lanes[0].dict;
			uint32		far = lanes[0].codes[i + 2 * WIDE_AHEAD],
						near = lanes[0].codes[i + WIDE_AHEAD];

			__builtin_prefetch(&d->off[far]);
			if (near < d->n)
				__builtin_prefetch(d->bytes + d->off[near]);
		}
		w->isnull[i] = isnull && !nan;
		if (w->isnull[i])
		{
			w->codes[i] = 0;
			w->has_null = true;
			continue;
		}
		if (nan)
			k[2] = 1;
		else if (opaque)
			k[0] = lanes[0].codes[i];
		else if (image)
		{
			k[0] = lanes[0].codes[i];
			k[1] = (uint64) lanes[1].i[i];
		}
		else
		{
			k[0] = (uint64) lanes[0].i[i];
			k[1] = (uint64) lanes[1].i[i];
			if (interval)
				k[2] = (uint64) lanes[2].i[i];
		}
		e = find(w, k, &at);
		if (e == 0)
		{
			uint8		b[SIFT_NIMG_TEXT_MAX + 64] __attribute__((aligned(8)));
			uint32		len;

			if (opaque)
			{
				e = add(w, k, at, SIFT_COL_DICT_STR(lanes[0].dict, lanes[0].codes[i]), SIFT_COL_DICT_LEN(lanes[0].dict, lanes[0].codes[i]));
				w->codes[i] = e - 1;
				continue;
			}
			if (nan)
			{
				SET_VARSIZE(b, VARHDRSZ + sizeof(uint16));
				memcpy(b + VARHDRSZ, &(uint16) {0xC000}, sizeof(uint16));
				len = VARSIZE(b);
			}
			else if (interval)
			{
				Interval   *p = sift_col_lanes_interval(lanes[0].i[i], lanes[1].i[i], lanes[2].i[i]);

				memcpy(b, p, sizeof(Interval));
				pfree(p);
				len = sizeof(Interval);
			}
			else if (image)
			{
				const char *img = SIFT_COL_DICT_STR(lanes[0].dict, lanes[0].codes[i]);

				uint32		ilen = SIFT_COL_DICT_LEN(lanes[0].dict, lanes[0].codes[i]);

				if ((len = image_numeric((const uint8 *) img, ilen, (int32) lanes[1].i[i], b)) == 0)
				{
					if (sift_col_numimg_to_text((const uint8 *) img, ilen, (int32) lanes[1].i[i], w->txt, SIFT_NIMG_TEXT_MAX) == 0)
						ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("sift_col: a numeric image is damaged")));
					len = text_numeric(w->txt, b);
				}
			}
			else if ((len = w128_numeric(sift_col_wide_join(lanes[0].i[i], lanes[1].i[i]), def->scale, b)) == 0)
			{
				sift_col_w128_to_decimal(sift_col_wide_join(lanes[0].i[i], lanes[1].i[i]), def->scale, txt);
				len = text_numeric(txt, b);
			}
			e = add(w, k, at, (const char *) b, len);
		}
		w->codes[i] = e - 1;
	}
}

Datum
sift_aggwide_datum(Oid typid, const char *bytes, uint32 len)
{
	return PointerGetDatum(bytes);
}
