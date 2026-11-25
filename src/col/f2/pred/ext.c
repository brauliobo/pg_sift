/*
 * col/f2/pred/ext.c - lower(col) LIKE 'prefix%' and lower(col) = 'lit' on a string, && and @> on the stored image of an array (col/f2/pred/ext.h).
 *
 * The array reader walks the ArrayType of PostgreSQL as it is stored: a 4 byte uncompressed varlena header, ndim, dataoffset, the element type, the dimensions and the lower bounds, the
 * bitmap of the non-NULL elements when dataoffset is not 0, then the elements (a NULL one takes no room). Offsets are relative to the start of the image, so the image may sit at any
 * address; every field is read with memcpy.
 *
 * check-memory: allow-allocs every array here follows the elements of one array constant of the statement
 */
#include "core/host.h"

#include "col/f2/pred/ext.h"

#define ARR_HDR			16				/* sizeof(ArrayType): vl_len_, ndim, dataoffset, elemtype */
#define ARR_MAXDIM		6
#define ARR_OID_TEXT	25
#define ARR_OID_VARCHAR	1043
#define ARR_OID_INT4	23
#define ARR_OID_INT8	20
#define ARR_ALIGN4(x)	(((x) + 3) & ~(uint64) 3)
#define ARR_ALIGN8(x)	(((x) + 7) & ~(uint64) 7)

/* ---- lower(col) LIKE 'prefix%' ---- */

bool
sift_f2_iprefix_has_ik(const uint8 *pat, uint32 plen)
{
	for (uint32 i = 0; i < plen; i++)
		if (pat[i] == 'i' || pat[i] == 'k')
			return true;
	return false;
}

SiftLikeVerdict
sift_f2_iprefix_test(const char *s, uint32 len, const uint8 *pat, uint32 plen, bool exact_eq, bool has_ik)
{
	const uint8 *b = (const uint8 *) s;

	if (len < plen)
		return SIFT_LIKE_NO;
	if (exact_eq && len > plen)
		return has_ik && sift_like_has_high(b, len) ? SIFT_LIKE_MAYBE : SIFT_LIKE_NO;
	for (uint32 i = 0; i < plen; i++)
	{
		uint8		c = b[i] >= 'A' && b[i] <= 'Z' ? b[i] | 0x20 : b[i];

		if (c != pat[i])
			return has_ik && sift_like_has_high(b, Min(len, plen * 3)) ? SIFT_LIKE_MAYBE : SIFT_LIKE_NO;
	}
	return SIFT_LIKE_YES;
}

uint32
sift_f2_iprefix_dict(uint32 n, const uint32 *off, const uint8 *bytes, const uint8 *pat, uint32 plen, bool exact_eq, uint64 *yes, uint64 *maybe)
{
	bool		has_ik = sift_f2_iprefix_has_ik(pat, plen);
	uint32		nw = (n + 63) / 64,
				found = 0;

	memset(yes, 0, nw * sizeof(uint64));
	memset(maybe, 0, nw * sizeof(uint64));
	for (uint32 c = 0; c < n; c++)
	{
		SiftLikeVerdict v = sift_f2_iprefix_test((const char *) bytes + off[c], off[c + 1] - off[c], pat, plen, exact_eq, has_ik);

		if (v != SIFT_LIKE_NO)
		{
			(v == SIFT_LIKE_YES ? yes : maybe)[c >> 6] |= UINT64CONST(1) << (c & 63);
			found++;
		}
	}
	return found;
}

/* ---- the right hand side of && and @> ---- */

static uint64
elem_hash(const uint8 *p, uint32 len)
{
	uint64		h = UINT64CONST(0xcbf29ce484222325);

	for (uint32 i = 0; i < len; i++)
		h = (h ^ p[i]) * UINT64CONST(0x100000001b3);
	return h;
}

static uint64
int_hash(int64 v)
{
	return ((uint64) v ^ ((uint64) v >> 32)) * UINT64CONST(0x9E3779B97F4A7C15);
}

static bool
elem_equal(const SiftArrQuery *q, uint32 i, const uint8 *p, uint32 len, int64 v)
{
	return q->kind == SIFT_ARR_TEXT ? q->len[i] == len && memcmp(q->ptr[i], p, len) == 0 : q->ival[i] == v;
}

static uint64
query_hash(const SiftArrQuery *q, uint32 i)
{
	return q->kind == SIFT_ARR_TEXT ? elem_hash(q->ptr[i], q->len[i]) : int_hash(q->ival[i]);
}

/* The index of the element equal to the probe, -1 for none */
static int
query_find(const SiftArrQuery *q, const uint8 *p, uint32 len, int64 v)
{
	if (q->nslots == 0)
	{
		for (uint32 i = 0; i < q->n; i++)
			if (elem_equal(q, i, p, len, v))
				return (int) i;
		return -1;
	}
	for (uint32 s = (uint32) (q->kind == SIFT_ARR_TEXT ? elem_hash(p, len) : int_hash(v)) & (q->nslots - 1); q->slot[s] != 0; s = (s + 1) & (q->nslots - 1))
		if (elem_equal(q, (uint32) q->slot[s] - 1, p, len, v))
			return q->slot[s] - 1;
	return -1;
}

static void
query_add(SiftArrQuery *q, const uint8 *p, uint32 len, int64 v)
{
	if (query_find(q, p, len, v) >= 0)
		return;
	if (q->kind == SIFT_ARR_TEXT)
	{
		q->ptr[q->n] = p;
		q->len[q->n] = len;
	}
	else
		q->ival[q->n] = v;
	q->n++;
	if (q->nslots != 0)
	{
		uint32		s = (uint32) query_hash(q, q->n - 1) & (q->nslots - 1);

		while (q->slot[s] != 0)
			s = (s + 1) & (q->nslots - 1);
		q->slot[s] = (int32) q->n;
	}
}

static SiftArrQuery *
query_new(SiftArrKind kind, int n)
{
	SiftArrQuery *q = palloc0(sizeof(SiftArrQuery));

	q->kind = kind;
	if (kind == SIFT_ARR_TEXT)
	{
		q->ptr = palloc(Max(n, 1) * sizeof(uint8 *));
		q->len = palloc(Max(n, 1) * sizeof(uint32));
	}
	else
		q->ival = palloc(Max(n, 1) * sizeof(int64));
	q->seen = palloc0(Max(n, 1) * sizeof(uint32));
	if (n > SIFT_ARRQ_LINEAR)
	{
		q->nslots = 32;
		while (q->nslots < 2 * (uint32) n)
			q->nslots *= 2;
		q->slot = palloc0(q->nslots * sizeof(int32));
	}
	return q;
}

SiftArrQuery *
sift_arr_query_text(const uint8 *const *elem, const uint32 *len, const bool *isnull, int n)
{
	SiftArrQuery *q = query_new(SIFT_ARR_TEXT, n);

	for (int i = 0; i < n; i++)
		if (isnull != NULL && isnull[i])
			q->has_null = true;
		else
			query_add(q, elem[i], len[i], 0);
	return q;
}

SiftArrQuery *
sift_arr_query_int(SiftArrKind kind, const int64 *val, const bool *isnull, int n)
{
	SiftArrQuery *q = query_new(kind, n);

	for (int i = 0; i < n; i++)
		if (isnull != NULL && isnull[i])
			q->has_null = true;
		else
			query_add(q, NULL, 0, val[i]);
	return q;
}

void
sift_arr_query_free(SiftArrQuery *q)
{
	if (q->ptr)
		pfree(q->ptr);
	if (q->len)
		pfree(q->len);
	if (q->ival)
		pfree(q->ival);
	if (q->slot)
		pfree(q->slot);
	pfree(q->seen);
	pfree(q);
}

/* ---- the image of an array ---- */

typedef struct ArrImg
{
	const uint8 *p;
	uint64		len;
	uint64		pos;			/* the next element */
	uint32		nitems;
	const uint8 *nullmap;		/* NULL when no element is */
} ArrImg;

static uint32
rd32(const uint8 *p)
{
	uint32		v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static bool
elem_type_ok(SiftArrKind kind, uint32 oid)
{
	return kind == SIFT_ARR_TEXT ? (oid == ARR_OID_TEXT || oid == ARR_OID_VARCHAR) : oid == (kind == SIFT_ARR_INT4 ? ARR_OID_INT4 : ARR_OID_INT8);
}

/* Reads the header; false for an image the walk does not take (the heap decides), *empty for an array of no element */
static bool
arr_open(ArrImg *a, SiftArrKind kind, const uint8 *img, uint32 img_len, bool *empty)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return false;
#else
	uint32		vl;
	int32		ndim,
				dataoff;
	uint64		nitems = 1,
				hdr;

	if (img_len < ARR_HDR || (img[0] & 3) != 0)
		return false;
	vl = rd32(img) >> 2;
	if (vl < ARR_HDR || vl > img_len)
		return false;
	ndim = (int32) rd32(img + 4);
	dataoff = (int32) rd32(img + 8);
	if (ndim < 0 || ndim > ARR_MAXDIM || !elem_type_ok(kind, rd32(img + 12)))
		return false;
	*a = (ArrImg) {.p = img,.len = vl};
	*empty = ndim == 0;
	if (ndim == 0)
		return true;
	hdr = ARR_HDR + 8 * (uint64) ndim;
	if (hdr > vl)
		return false;
	for (int d = 0; d < ndim; d++)
	{
		int32		dim = (int32) rd32(img + ARR_HDR + 4 * d);

		if (dim < 0 || (nitems *= (uint64) dim) > INT32_MAX)
			return false;
	}
	a->nitems = (uint32) nitems;
	*empty = nitems == 0;
	if (dataoff != 0)
	{
		if (dataoff < 0 || (uint64) dataoff < hdr + (nitems + 7) / 8 || (uint64) dataoff > vl)
			return false;
		a->nullmap = img + hdr;
		a->pos = (uint64) dataoff;
	}
	else
		a->pos = ARR_ALIGN8(hdr);
	return true;
#endif
}

/* The next element that is not NULL: false at the end of the array, *bad when the image does not hold it */
static bool
arr_next(ArrImg *a, uint32 *idx, SiftArrKind kind, const uint8 **p, uint32 *len, int64 *v, bool *bad)
{
	for (; *idx < a->nitems; (*idx)++)
	{
		uint64		total;

		if (a->nullmap != NULL && !((a->nullmap[*idx >> 3] >> (*idx & 7)) & 1))
			continue;
		if (kind != SIFT_ARR_TEXT)
		{
			uint64		w = kind == SIFT_ARR_INT4 ? 4 : 8;

			if (a->pos + w > a->len)
				return *bad = true, false;
			if (kind == SIFT_ARR_INT4)
			{
				int32		x;

				memcpy(&x, a->p + a->pos, sizeof(x));
				*v = x;
			}
			else
				memcpy(v, a->p + a->pos, sizeof(*v));
			a->pos += w;
			(*idx)++;
			return true;
		}
		if (a->pos >= a->len)
			return *bad = true, false;
		if (a->p[a->pos] == 0)
			a->pos = ARR_ALIGN4(a->pos);
		if (a->pos >= a->len)
			return *bad = true, false;
		if (a->p[a->pos] & 1)
		{
			total = a->p[a->pos] >> 1;
			if (total < 1)
				return *bad = true, false;
			*p = a->p + a->pos + 1;
			*len = (uint32) total - 1;
		}
		else
		{
			uint32		hdr;

			if (a->pos + 4 > a->len || ((hdr = rd32(a->p + a->pos)) & 3) != 0 || (total = hdr >> 2) < 4)
				return *bad = true, false;
			*p = a->p + a->pos + 4;
			*len = (uint32) total - 4;
		}
		if (a->pos + total > a->len)
			return *bad = true, false;
		a->pos += total;
		(*idx)++;
		return true;
	}
	return false;
}

SiftLikeVerdict
sift_f2_arr_match(SiftArrQuery *q, const uint8 *img, uint32 img_len, bool contains)
{
	ArrImg		a;
	bool		empty = false,
				bad = false;
	uint32		idx = 0,
				hits = 0,
				elen = 0;
	const uint8 *ep = NULL;
	int64		ev = 0;

	if (!arr_open(&a, q->kind, img, img_len, &empty) || (contains && q->has_null))
		return SIFT_LIKE_MAYBE;
	if (contains && q->n == 0)
		return SIFT_LIKE_YES;
	if (empty || q->n == 0)
		return SIFT_LIKE_NO;
	if (++q->gen == 0)
	{
		memset(q->seen, 0, q->n * sizeof(uint32));
		q->gen = 1;
	}
	while (arr_next(&a, &idx, q->kind, &ep, &elen, &ev, &bad))
	{
		int			hit = query_find(q, ep, elen, ev);

		if (hit < 0)
			continue;
		if (!contains)
			return SIFT_LIKE_YES;
		if (q->seen[hit] != q->gen)
		{
			q->seen[hit] = q->gen;
			if (++hits == q->n)
				return SIFT_LIKE_YES;
		}
	}
	return bad ? SIFT_LIKE_MAYBE : SIFT_LIKE_NO;
}
