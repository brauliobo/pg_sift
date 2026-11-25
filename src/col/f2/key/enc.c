/*
 * col/f2/key/enc.c - the order preserving bytes of a sort key (col/f2/key/enc.h).
 */
#include "core/host.h"

#include "col/f2/key/enc.h"

typedef struct Emit
{
	uint8	   *out;
	uint32		cap;
	uint32		n;
	bool		trunc;
	uint8		xor;			/* 0xFF for a descending column: its value bytes are complemented */
} Emit;

static void
put(Emit *e, uint8 b)
{
	if (e->n < e->cap)
		e->out[e->n++] = b ^ e->xor;
	else
		e->trunc = true;
}

static void
put_be(Emit *e, uint64 v, int bytes)
{
	for (int i = bytes - 1; i >= 0; i--)
		put(e, (uint8) (v >> (8 * i)));
}

uint8
sift_key_null_tag(const SiftKeyCol *c)
{
	return (c->flags & SIFT_KEYCOL_NULLS_FIRST) ? 0x00 : 0x02;
}

bool
sift_key_spec_valid(const SiftKeySpec *s)
{
	if (s->ncols < 1 || s->ncols > SIFT_KEY_COLS_MAX || s->key_bytes < SIFT_KEY_BYTES_MIN || s->key_bytes > SIFT_KEY_BYTES_MAX || s->key_bytes % 8 != 0)
		return false;
	for (int i = 0; i < s->ncols; i++)
		if (s->cols[i].kind < SIFT_KK_BOOL || s->cols[i].kind > SIFT_KK_TEXT || (s->cols[i].flags & ~(SIFT_KEYCOL_DESC | SIFT_KEYCOL_NULLS_FIRST)))
			return false;
	return true;
}

/* The bits of a float whose unsigned order is the order of the floats: -0 is 0 and a NaN is the greatest */
static uint64
f8_key(double d)
{
	uint64		b;

	if (d != d)
		return UINT64CONST(0xFFF8000000000000);
	if (d == 0.0)
		d = 0.0;
	memcpy(&b, &d, 8);
	return (b >> 63) ? ~b : (b | (UINT64CONST(1) << 63));
}

static uint32
f4_key(double d)
{
	float		f = (float) d;
	uint32		b;

	if (f != f)
		return 0xFFC00000U;
	if (f == 0.0f)
		f = 0.0f;
	memcpy(&b, &f, 4);
	return (b >> 31) ? ~b : (b | 0x80000000U);
}

static void
enc_text(Emit *e, const uint8 *p, uint32 len)
{
	for (uint32 i = 0; i < len; i++)
	{
		put(e, p[i]);
		if (p[i] == 0)
			put(e, 0xFF);
	}
	put(e, 0x00);
	put(e, 0x01);
}

static bool
is_word(const uint8 *p, uint32 len, const char *w)
{
	uint32		n = (uint32) strlen(w);

	return len == n && memcmp(p, w, n) == 0;
}

/*
 * A decimal text as sign, exponent and digits: the value is 0.d1d2...dn * 10^e with d1 not 0, as the digits of the text with the zeros at both ends taken off. The sign tag orders
 * -Infinity < negative < zero < positive < Infinity < NaN; the bytes after the tag of a negative number are complemented, which reverses their order.
 */
static void
enc_numeric(Emit *e, const uint8 *p, uint32 len)
{
	uint32		i = 0,
				ip,
				ie,
				fp,
				fe;
	bool		neg = false;
	int32		exp10;
	uint8		saved = e->xor;

	if (is_word(p, len, "NaN") || is_word(p, len, "Infinity") || is_word(p, len, "+Infinity") || is_word(p, len, "-Infinity"))
	{
		put(e, is_word(p, len, "NaN") ? 0xF0 : p[0] == '-' ? 0x20 : 0xE0);
		return;
	}
	if (i < len && (p[i] == '-' || p[i] == '+'))
		neg = p[i++] == '-';
	ip = i;
	while (i < len && p[i] >= '0' && p[i] <= '9')
		i++;
	ie = i;
	fp = fe = i;
	if (i < len && p[i] == '.')
	{
		fp = ++i;
		while (i < len && p[i] >= '0' && p[i] <= '9')
			i++;
		fe = i;
	}
	SIFT_INVARIANT(i == len && (ie != ip || fe != fp), "malformed numeric key value");
	while (ip < ie && p[ip] == '0')
		ip++;
	while (fe > fp && p[fe - 1] == '0')
		fe--;
	if (ip == ie)
	{
		uint32		z = 0;

		while (fp + z < fe && p[fp + z] == '0')
			z++;
		fp += z;
		exp10 = -(int32) z;
	}
	else
		exp10 = (int32) (ie - ip);
	if (ip == ie && fp == fe)
	{
		put(e, 0x80);
		return;
	}
	put(e, neg ? 0x40 : 0xC0);
	if (neg)
		e->xor ^= 0xFF;
	put_be(e, (uint32) exp10 + 0x80000000U, 4);
	if (fe == fp)
		while (ie > ip && p[ie - 1] == '0')
			ie--;
	for (uint32 k = ip; k < ie; k++)
		put(e, (uint8) (p[k] - '0' + 1));
	for (uint32 k = fp; k < fe; k++)
		put(e, (uint8) (p[k] - '0' + 1));
	put(e, 0x00);
	e->xor = saved;
}

uint32
sift_key_enc_val(const SiftKeyCol *c, const SiftKeyVal *v, uint8 *out, uint32 cap, bool *truncated)
{
	Emit		e = {.out = out,.cap = cap};

	if (v->isnull)
		put(&e, sift_key_null_tag(c));
	else
	{
		put(&e, SIFT_KEY_VALUE_TAG);
		e.xor = (c->flags & SIFT_KEYCOL_DESC) ? 0xFF : 0x00;
		switch (c->kind)
		{
			case SIFT_KK_BOOL:
				put(&e, v->i != 0);
				break;
			case SIFT_KK_INT2:
				put_be(&e, (uint16) v->i ^ 0x8000U, 2);
				break;
			case SIFT_KK_INT4:
				put_be(&e, (uint32) v->i ^ 0x80000000U, 4);
				break;
			case SIFT_KK_INT8:
				put_be(&e, (uint64) v->i ^ (UINT64CONST(1) << 63), 8);
				break;
			case SIFT_KK_FLOAT4:
				put_be(&e, f4_key(v->f), 4);
				break;
			case SIFT_KK_FLOAT8:
				put_be(&e, f8_key(v->f), 8);
				break;
			case SIFT_KK_UUID:
				SIFT_INVARIANT(v->len == 16, "a uuid key value is 16 bytes, not %u", v->len);
				for (int i = 0; i < 16; i++)
					put(&e, v->p[i]);
				break;
			case SIFT_KK_NUMERIC:
				enc_numeric(&e, v->p, v->len);
				break;
			case SIFT_KK_TEXT:
				enc_text(&e, v->p, v->len);
				break;
			default:
				SIFT_INVARIANT(false, "key kind %d is none", c->kind);
		}
	}
	if (e.trunc && truncated != NULL)
		*truncated = true;
	return e.n;
}

uint32
sift_key_enc_tuple(const SiftKeySpec *s, const SiftKeyVal *vals, int n, uint8 *out, bool *truncated)
{
	uint32		len = 0;

	memset(out, 0, s->key_bytes);
	for (int i = 0; i < n && len < s->key_bytes; i++)
		len += sift_key_enc_val(&s->cols[i], &vals[i], out + len, s->key_bytes - len, truncated);
	return len;
}

static int
cmp_u64(uint64 a, uint64 b)
{
	return a < b ? -1 : a > b;
}

int
sift_key_val_cmp(const SiftKeyCol *c, const SiftKeyVal *a, const SiftKeyVal *b)
{
	SiftKeyCol	asc = {.kind = c->kind};
	uint8	   *x,
			   *y;
	uint32		nx,
				ny;
	int			r;

	switch (c->kind)
	{
		case SIFT_KK_BOOL:
		case SIFT_KK_INT2:
		case SIFT_KK_INT4:
		case SIFT_KK_INT8:
			return a->i < b->i ? -1 : a->i > b->i;
		case SIFT_KK_FLOAT4:
			return cmp_u64(f4_key(a->f), f4_key(b->f));
		case SIFT_KK_FLOAT8:
			return cmp_u64(f8_key(a->f), f8_key(b->f));
		case SIFT_KK_UUID:
			return memcmp(a->p, b->p, 16);
		default:
			break;
	}
	x = palloc(a->len + 16);
	y = palloc(b->len + 16);
	nx = sift_key_enc_val(&asc, a, x, a->len + 16, NULL);
	ny = sift_key_enc_val(&asc, b, y, b->len + 16, NULL);
	r = memcmp(x, y, Min(nx, ny));
	if (r == 0)
		r = nx < ny ? -1 : nx > ny;
	pfree(x);
	pfree(y);
	return r;
}
