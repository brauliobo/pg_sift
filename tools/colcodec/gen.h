/*
 * gen.h - deterministic e-commerce shaped columns for the column codec harness (tools/colcodec).
 *
 * An orders table (ever-growing id, store id with a skewed popularity, channel, date, dictionary-text columns of low
 * cardinality, 2-decimal totals, coordinates, a full-entropy double, an update timestamp, nullable discounts, free text and
 * urls) and its flattened order-items table (order id repeated 0..8 times, small categorical codes, prices in 0.5 steps, the
 * store and date denormalized). Skews: store ~ u^2 over 40000 stores, channel ~ u^2 over 30, 12 locales, 60 countries,
 * 2 % cancelled. Two physical orders: heap (insertion order, ids adjacent) and clustered by (store_id, order_date).
 * Not natural language: the text columns are Zipf-sampled pseudo-words, so zstd/lz4 ratios on them are only indicative.
 */
#ifndef CC_GEN_H
#define CC_GEN_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sift_testrng.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;

typedef enum { K_INT, K_F32, K_F64, K_STR } Kind;

typedef struct Col
{
	const char *name;
	Kind		kind;
	int			width;			/* bytes of one value (INT, F32, F64) */
	int			sgn;
	void	   *d;				/* n values, or for K_STR the blob */
	u32		   *off;			/* K_STR: n + 1 offsets into d */
	u8		   *nul;			/* NULL or one byte per row, 1 = null */
	u32			n;
} Col;

typedef struct Table
{
	const char *name;
	u32			n;
	int			ncol;
	Col			col[40];
} Table;

static inline double
cc_u(u64 n, int k)
{
	return (sift_mix64(n * 1024 + (u64) k) % 1000003) / 1000003.0;
}

static const char *const g_locales[] = {"en", "de", "fr", "es", "it", "pt", "nl", "ru", "ja", "zh", "sv", "pl"};
static const char *const g_kinds[] = {"retail", "wholesale", "online", "corporate", "reseller", "partner"};
static const char *const g_cats[] = {"Electronics", "Apparel", "Home", "Garden", "Toys", "Books", "Sports", "Beauty", "Grocery",
									 "Pet", "Office", "Auto", "Tools", "Music", "Health"};
static const char *const g_states[] = {"pending", "shipped", "returned"};

#define NVOCAB 6000
static char g_vocab[NVOCAB][12];

static void
gen_vocab(void)
{
	for (int i = 0; i < NVOCAB; i++)
	{
		int			len = 3 + (int) (sift_mix64(i) % 8);

		for (int j = 0; j < len; j++)
			g_vocab[i][j] = 'a' + (int) (sift_mix64(i * 31 + j) % 26);
		g_vocab[i][len] = 0;
	}
}

static const char *
vocab_word(u64 h)
{
	double		u = (h % 1000003) / 1000003.0;

	return g_vocab[(int) (u * u * u * NVOCAB)];
}

typedef struct Order
{
	u32			id,
				store;
	u16			channel;
	i32			date;
	u8			locale,
				country,
				kind,
				cancelled,
				paid,
				nitems,
				discount_null;
	float		discount;
	double		total,
				lat,
				lon,
				noise;
	i64			updated;
} Order;

static int
order_cmp(const void *a, const void *b)
{
	const Order *x = a,
			   *y = b;

	if (x->store != y->store)
		return x->store < y->store ? -1 : 1;
	if (x->date != y->date)
		return x->date < y->date ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id;
}

typedef struct StrB
{
	char	   *d;
	u32		   *off;
	size_t		len,
				cap;
	u32			n;
} StrB;

static void
strb_add(StrB *s, const char *p, size_t l)
{
	if (s->len + l + 1 > s->cap)
	{
		s->cap = (s->len + l + 1) * 2 + 65536;
		s->d = realloc(s->d, s->cap);
	}
	if (!s->off)
		s->off = calloc(1, sizeof(u32));
	s->off = realloc(s->off, (s->n + 2) * sizeof(u32));
	memcpy(s->d + s->len, p, l);
	s->len += l;
	s->off[++s->n] = (u32) s->len;
}

static Col *
add_col(Table *t, const char *name, Kind k, int w, int sgn)
{
	Col		   *c = &t->col[t->ncol++];

	memset(c, 0, sizeof(*c));
	c->name = name;
	c->kind = k;
	c->width = w;
	c->sgn = sgn;
	c->n = t->n;
	if (k != K_STR)
		c->d = calloc(t->n + 64, w);
	return c;
}

#define SETI(c, i, v) do { \
	switch ((c)->width) { \
	case 1: ((u8 *) (c)->d)[i] = (u8) (v); break; \
	case 2: ((u16 *) (c)->d)[i] = (u16) (v); break; \
	case 4: ((u32 *) (c)->d)[i] = (u32) (v); break; \
	default: ((u64 *) (c)->d)[i] = (u64) (v); } \
} while (0)

static Col *
add_str(Table *t, const char *name, StrB *s)
{
	Col		   *c = &t->col[t->ncol++];

	memset(c, 0, sizeof(*c));
	c->name = name;
	c->kind = K_STR;
	c->d = s->d;
	c->off = s->off;
	c->n = s->n;
	return c;
}

/* the orders table and its flattened items table, norders orders, in heap order or clustered */
static void
gen_tables(Table *ot, Table *it, u32 norders, int clustered)
{
	Order	   *ords = calloc(norders, sizeof(Order));
	u32			nitems = 0;
	char		buf[1024];

	gen_vocab();
	for (u32 i = 0; i < norders; i++)
	{
		Order	   *o = &ords[i];
		u64			n = i;

		o->id = i + 1;
		o->store = 1 + (u32) floor(pow(cc_u(n, 1), 2) * 40000);
		o->channel = 1 + (u16) floor(pow(cc_u(n, 2), 2) * 30);
		o->date = 19000 + (i32) floor(1400 * sqrt(cc_u(n, 3)));
		o->locale = (u8) floor(pow(cc_u(n, 4), 2) * 12);
		o->country = (u8) floor(pow(cc_u(n, 5), 2) * 60);
		o->kind = (u8) floor(pow(cc_u(n, 10), 1.5) * 6);
		o->cancelled = cc_u(n, 6) < 0.02;
		o->paid = cc_u(n, 6) < 0.7;
		o->nitems = (u8) floor(pow(cc_u(n, 9), 1.5) * 9);
		o->discount_null = cc_u(n, 12) < 0.12;
		o->discount = (float) (floor((1 + 9 * pow(cc_u(n, 13), 0.5)) * 10 + 0.5) / 10);
		o->total = floor(exp(3 + 3.2 * cc_u(n, 14) + 0.5 * cc_u(n, 15)) * 100) / 100;
		o->lat = floor((cc_u(n, 16) * 140 - 70) * 1e6) / 1e6;
		o->lon = floor((cc_u(n, 17) * 360 - 180) * 1e6) / 1e6;
		o->noise = (double) (sift_mix64(n + 77) >> 11) * 0x1p-53;
		o->updated = INT64_C(1704067200000000) + (i64) n * 2300000 + (i64) (sift_mix64(n + 5) % 2000000);
		nitems += o->nitems;
	}
	if (clustered)
		qsort(ords, norders, sizeof(Order), order_cmp);

	ot->n = norders;
	it->n = nitems;
	{
		Col *id = add_col(ot, "id", K_INT, 4, 0), *store = add_col(ot, "store_id", K_INT, 4, 0),
			*chan = add_col(ot, "channel_id", K_INT, 2, 0), *date = add_col(ot, "order_date", K_INT, 4, 1),
			*loc = add_col(ot, "locale_code", K_INT, 1, 0), *cty = add_col(ot, "country_code", K_INT, 1, 0),
			*can = add_col(ot, "cancelled", K_INT, 1, 0), *paid = add_col(ot, "paid", K_INT, 1, 0),
			*nit = add_col(ot, "n_items", K_INT, 1, 0), *disc = add_col(ot, "discount", K_F32, 4, 0),
			*total = add_col(ot, "total", K_F64, 8, 0), *lat = add_col(ot, "lat", K_F64, 8, 0),
			*lon = add_col(ot, "lon", K_F64, 8, 0), *noise = add_col(ot, "noise", K_F64, 8, 0),
			*upd = add_col(ot, "updated_at", K_INT, 8, 1), *offs = add_col(ot, "item_offsets", K_INT, 4, 0);
		StrB		sl = {0}, sc = {0}, sk = {0}, sn = {0}, su = {0}, st = {0}, sx = {0};
		u32			cum = 0;

		disc->nul = calloc(norders, 1);
		for (u32 i = 0; i < norders; i++)
		{
			Order	   *o = &ords[i];

			SETI(id, i, o->id);
			SETI(store, i, o->store);
			SETI(chan, i, o->channel);
			SETI(date, i, o->date);
			SETI(loc, i, o->locale);
			SETI(cty, i, o->country);
			SETI(can, i, o->cancelled);
			SETI(paid, i, o->paid);
			SETI(nit, i, o->nitems);
			((float *) disc->d)[i] = o->discount;
			disc->nul[i] = o->discount_null;
			((double *) total->d)[i] = o->total;
			((double *) lat->d)[i] = o->lat;
			((double *) lon->d)[i] = o->lon;
			((double *) noise->d)[i] = o->noise;
			SETI(upd, i, o->updated);
			cum += o->nitems;
			SETI(offs, i, cum);
			strb_add(&sl, g_locales[o->locale], strlen(g_locales[o->locale]));
			snprintf(buf, sizeof buf, "%c%c", 65 + o->country / 8, 65 + o->country % 8);
			strb_add(&sc, buf, 2);
			strb_add(&sk, g_kinds[o->kind], strlen(g_kinds[o->kind]));
			snprintf(buf, sizeof buf, "%s %s %s", vocab_word(sift_mix64(o->store * 3)), vocab_word(sift_mix64(o->store * 3 + 1)),
					 vocab_word(sift_mix64(o->store * 3 + 2)));
			buf[0] -= 32;
			strb_add(&sn, buf, strlen(buf));
			snprintf(buf, sizeof buf, "https://shop.example/store/%u/order/%u", o->store, o->id);
			strb_add(&su, buf, strlen(buf));
			{
				int			nw = 3 + (int) (sift_mix64(o->id + 1) % 4),
							len = 0;

				for (int j = 0; j < nw; j++)
					len += snprintf(buf + len, sizeof buf - len, "%s%s", j ? " " : "", vocab_word(sift_mix64((u64) o->id * 977 + j)));
				strb_add(&st, buf, len);
			}
			{
				int			nw = 12 + (int) (sift_mix64(o->id + 2) % 30),
							len = 0;

				for (int j = 0; j < nw; j++)
					len += snprintf(buf + len, sizeof buf - len, "%s%s", j ? " " : "", vocab_word(sift_mix64((u64) o->id * 1013 + 100 + j)));
				strb_add(&sx, buf, len);
			}
		}
		add_str(ot, "locale", &sl);
		add_str(ot, "country", &sc);
		add_str(ot, "customer_kind", &sk);
		add_str(ot, "store_name", &sn);
		add_str(ot, "url", &su);
		add_str(ot, "note", &st);
		add_str(ot, "description", &sx);
	}
	{
		Col *oid = add_col(it, "order_id", K_INT, 4, 0), *idx = add_col(it, "item_idx", K_INT, 1, 0),
			*cat = add_col(it, "category_code", K_INT, 1, 0), *var = add_col(it, "variant_code", K_INT, 1, 0),
			*sta = add_col(it, "state_code", K_INT, 1, 0), *p4 = add_col(it, "price", K_F32, 4, 0),
			*p8 = add_col(it, "price8", K_F64, 8, 0), *store = add_col(it, "store_id", K_INT, 4, 0),
			*date = add_col(it, "order_date", K_INT, 4, 1), *px2 = add_col(it, "price_x2", K_INT, 2, 0);
		StrB		scat = {0}, sst = {0};
		u32			k = 0;

		for (u32 i = 0; i < norders; i++)
			for (int j = 0; j < ords[i].nitems; j++, k++)
			{
				u64			n = ords[i].id - 1;
				double		pr = floor((1 + 299 * pow(cc_u(n, 400 + j), 3)) * 2 + 0.5) / 2,
							us = cc_u(n, 500 + j);
				int			ci = (int) floor(pow(cc_u(n, 200 + j), 2) * 15),
							s = us < 0.1 ? 2 : us < 0.35 ? 0 : 1;

				SETI(oid, k, ords[i].id);
				SETI(idx, k, j);
				SETI(cat, k, ci);
				SETI(var, k, sift_mix64(n * 8 + 300 + j) % 8);
				SETI(sta, k, s);
				((float *) p4->d)[k] = (float) pr;
				((double *) p8->d)[k] = pr;
				SETI(store, k, ords[i].store);
				SETI(date, k, ords[i].date);
				SETI(px2, k, (int) (pr * 2));
				strb_add(&scat, g_cats[ci], strlen(g_cats[ci]));
				strb_add(&sst, g_states[s], strlen(g_states[s]));
			}
		add_str(it, "category", &scat);
		add_str(it, "state", &sst);
	}
	free(ords);
}

#endif
