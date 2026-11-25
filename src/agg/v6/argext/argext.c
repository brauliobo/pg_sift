/*
 * agg/v6/argext/argext.c - arg_max, arg_min and DISTINCT ON over order-preserving keys (v6_argext.h).
 */
#include "agg/v6/argext/argext.h"
#include "agg/v6/spill.h"

#define SIGN64		(UINT64_C(1) << 63)

/* ---- order-preserving keys ---- */

static void
ord_marker(SiftBytes *k, SiftOrdSpec s, bool is_null)
{
	sift_bytes_put1(k, is_null ? (s.nulls_first ? 0 : 2) : 1);
}

static void
ord_put_u64(SiftBytes *k, SiftOrdSpec s, uint64 u)
{
	uint8	   *p = sift_bytes_room(k, 8);

	if (s.desc)
		u = ~u;
	for (int i = 0; i < 8; i++)
		p[i] = (uint8) (u >> (56 - 8 * i));
	k->len += 8;
}

uint64
sift_ord_u64_of_i64(int64 v)
{
	return (uint64) v ^ SIGN64;
}

uint64
sift_ord_u64_of_f64(double v)
{
	uint64		bits;

	if (v != v)
		bits = UINT64_C(0x7FF8000000000000);
	else if (v == 0)
		bits = 0;
	else
		memcpy(&bits, &v, sizeof(bits));
	return (bits & SIGN64) ? ~bits : bits | SIGN64;
}

void
sift_ord_null(SiftBytes *k, SiftOrdSpec s)
{
	ord_marker(k, s, true);
}

void
sift_ord_i64(SiftBytes *k, SiftOrdSpec s, int64 v)
{
	ord_marker(k, s, false);
	ord_put_u64(k, s, sift_ord_u64_of_i64(v));
}

void
sift_ord_f64(SiftBytes *k, SiftOrdSpec s, double v)
{
	ord_marker(k, s, false);
	ord_put_u64(k, s, sift_ord_u64_of_f64(v));
}

/* 0x00 is escaped as 0x00 0xFF and the string ends with 0x00 0x01: no encoded string is a prefix of another, so a shorter string sorts first and the next column never mixes into it */
void
sift_ord_bytes(SiftBytes *k, SiftOrdSpec s, const void *p, uint32 len)
{
	const uint8 *in = p;
	uint32		start;

	ord_marker(k, s, false);
	start = k->len;
	for (uint32 i = 0; i < len; i++)
	{
		sift_bytes_put1(k, in[i]);
		if (in[i] == 0)
			sift_bytes_put1(k, 0xFF);
	}
	sift_bytes_put1(k, 0);
	sift_bytes_put1(k, 1);
	if (s.desc)
		for (uint32 i = start; i < k->len; i++)
			k->buf[i] = (uint8) ~k->buf[i];
}

static int
key_cmp(const uint8 *a, uint32 alen, const uint8 *b, uint32 blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	if (c != 0)
		return c;
	return alen == blen ? 0 : (alen < blen ? -1 : 1);
}

/* ---- arg_max, arg_min ---- */

typedef struct ArgSlot
{
	uint64		k64;
	int64		v64;
	uint8	   *kb;
	uint8	   *vb;
	uint32		klen;
	uint32		kcap;
	uint32		vlen;
	uint32		vcap;
	bool		has;
	bool		vnull;
} ArgSlot;

struct SiftArgExt
{
	MemoryContext cx;
	SiftArgDir	dir;
	SiftArgMode mode;
	ArgSlot    *slot;
	uint32		nslot;
	uint64		bytes;
};

SiftArgExt *
sift_argext_create(MemoryContext cx, SiftArgDir dir, SiftArgMode mode)
{
	SiftArgExt *a = MemoryContextAllocZero(cx, sizeof(SiftArgExt));

	a->cx = cx;
	a->dir = dir;
	a->mode = mode;
	return a;
}

static ArgSlot *
arg_slot(SiftArgExt *a, uint32 gid)
{
	if (gid >= a->nslot)
	{
		uint32		n = Max(Max(a->nslot * 2, gid + 1), 16);
		ArgSlot    *s = MemoryContextAllocZero(a->cx, sizeof(ArgSlot) * n);

		if (a->slot != NULL)
		{
			memcpy(s, a->slot, sizeof(ArgSlot) * a->nslot);
			pfree(a->slot);
		}
		a->bytes += sizeof(ArgSlot) * (uint64) (n - a->nslot);
		a->slot = s;
		a->nslot = n;
	}
	return &a->slot[gid];
}

static bool
arg_better(const SiftArgExt *a, bool greater)
{
	return a->dir == SIFT_ARG_MAX ? greater : !greater;
}

void
sift_argext_offer_i64(SiftArgExt *a, uint32 gid, uint64 cmp_key, int64 val, bool val_null)
{
	ArgSlot    *s = arg_slot(a, gid);

	SIFT_INVARIANT(a->mode == SIFT_ARG_I64, "arg_max: the one-word offer to a state of bytes");
	if (s->has && (cmp_key == s->k64 || !arg_better(a, cmp_key > s->k64)))
		return;
	s->has = true;
	s->k64 = cmp_key;
	s->v64 = val;
	s->vnull = val_null;
}

static uint8 *
arg_copy(SiftArgExt *a, uint8 *old, uint32 *cap, const void *p, uint32 len)
{
	if (len > *cap)
	{
		uint32		ncap = Max(len, 16);

		if (old != NULL)
			pfree(old);
		old = MemoryContextAlloc(a->cx, ncap);
		a->bytes += ncap - *cap;
		*cap = ncap;
	}
	if (len > 0)
		memcpy(old, p, len);
	return old;
}

void
sift_argext_offer(SiftArgExt *a, uint32 gid, const void *cmp_key, uint32 cmp_len, const void *val, uint32 val_len, bool val_null)
{
	ArgSlot    *s = arg_slot(a, gid);

	SIFT_INVARIANT(a->mode == SIFT_ARG_BYTES, "arg_max: the offer of bytes to a state of one word");
	if (s->has)
	{
		int			c = key_cmp(cmp_key, cmp_len, s->kb, s->klen);

		if (c == 0 || !arg_better(a, c > 0))
			return;
	}
	s->kb = arg_copy(a, s->kb, &s->kcap, cmp_key, cmp_len);
	s->klen = cmp_len;
	s->vnull = val_null;
	s->vb = arg_copy(a, s->vb, &s->vcap, val, val_null ? 0 : val_len);
	s->vlen = val_null ? 0 : val_len;
	s->has = true;
}

static inline int64
val_of(const SiftVec *v, uint32 r)
{
	if (v == NULL)
		return (int64) r;
	switch (v->type)
	{
		case SVEC_I32:
			return ((const int32 *) v->data)[r];
		default:
			return ((const int64 *) v->data)[r];
	}
}

void
sift_argext_update(SiftArgExt *a, const SiftBatchSel *bs, const uint32 *gid, const SiftVec *cmp, const SiftVec *val)
{
	SiftBatchIter it;
	uint32		r;

	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
	{
		if (!sift_vec_valid_at(cmp, r))
			continue;
		sift_argext_offer_i64(a, gid != NULL ? gid[r] : 0, sift_argext_cmp_key(cmp, r), val_of(val, r), val != NULL && !sift_vec_valid_at(val, r));
	}
}

void
sift_argext_merge(SiftArgExt *a, const SiftArgExt *src)
{
	for (uint32 g = 0; g < src->nslot; g++)
	{
		const ArgSlot *s = &src->slot[g];

		if (!s->has)
			continue;
		if (a->mode == SIFT_ARG_I64)
			sift_argext_offer_i64(a, g, s->k64, s->v64, s->vnull);
		else
			sift_argext_offer(a, g, s->kb, s->klen, s->vb, s->vlen, s->vnull);
	}
}

bool
sift_argext_has(const SiftArgExt *a, uint32 gid)
{
	return gid < a->nslot && a->slot[gid].has;
}

bool
sift_argext_val_i64(const SiftArgExt *a, uint32 gid, int64 *val)
{
	if (!sift_argext_has(a, gid) || a->slot[gid].vnull)
		return false;
	*val = a->slot[gid].v64;
	return true;
}

bool
sift_argext_val_bytes(const SiftArgExt *a, uint32 gid, const uint8 **val, uint32 *len)
{
	if (!sift_argext_has(a, gid) || a->slot[gid].vnull)
		return false;
	*val = a->slot[gid].vb;
	*len = a->slot[gid].vlen;
	return true;
}

uint32
sift_argext_ngroups(const SiftArgExt *a)
{
	return a->nslot;
}

uint64
sift_argext_bytes(const SiftArgExt *a)
{
	return a->bytes;
}

void
sift_argext_free(SiftArgExt *a)
{
	for (uint32 g = 0; g < a->nslot; g++)
	{
		if (a->slot[g].kb != NULL)
			pfree(a->slot[g].kb);
		if (a->slot[g].vb != NULL)
			pfree(a->slot[g].vb);
	}
	if (a->slot != NULL)
		pfree(a->slot);
	pfree(a);
}

/* ---- DISTINCT ON ---- */

#define DIS_MAX_LEVEL		6
#define DIS_MIN_CAP			256
#define DIS_MIN_BUDGET		(32 * 1024)

typedef struct DisGroup
{
	uint64		hash;
	uint64		tag;
	uint32		klen;
	uint32		olen;
	uint32		ocap;
	uint32		pad;
	uint8		data[];			/* key, then the order key (ocap bytes) */
} DisGroup;

struct SiftDisOn
{
	MemoryContext cx;
	uint32		level;
	uint64		budget;
	uint64		expect;
	DisGroup  **tab;
	uint32		cap;
	uint64		ngroups;
	uint64		gbytes;
	SiftSpillSet *spill;
	SiftBytes	rec;
	SiftDisOnStats st;
};

static uint64
dis_group_bytes(uint32 klen, uint32 ocap)
{
	return sizeof(DisGroup) + klen + ocap + 16;
}

static uint64
dis_bytes_of(const SiftDisOn *d)
{
	return d->gbytes + (uint64) d->cap * sizeof(DisGroup *);
}

uint64
sift_dison_bytes(const SiftDisOn *d)
{
	return dis_bytes_of(d);
}

static SiftDisOn *
dis_new(MemoryContext cx, uint32 level, uint64 budget, uint64 expect)
{
	SiftDisOn  *d = MemoryContextAllocZero(cx, sizeof(SiftDisOn));

	d->cx = cx;
	d->level = level;
	d->budget = Max(budget, DIS_MIN_BUDGET);
	d->expect = expect;
	sift_bytes_init(&d->rec, cx);
	return d;
}

SiftDisOn *
sift_dison_create(MemoryContext cx, uint64 budget_bytes, uint64 expect_bytes)
{
	return dis_new(cx, 0, budget_bytes, expect_bytes);
}

static void
dis_tab_insert(DisGroup **tab, uint32 cap, DisGroup *g)
{
	uint32		i = (uint32) g->hash & (cap - 1);

	while (tab[i] != NULL)
		i = (i + 1) & (cap - 1);
	tab[i] = g;
}

static void
dis_tab_grow(SiftDisOn *d)
{
	uint32		ncap = Max(d->cap * 2, DIS_MIN_CAP);
	DisGroup  **n = MemoryContextAllocZero(d->cx, sizeof(DisGroup *) * ncap);

	for (uint32 i = 0; i < d->cap; i++)
		if (d->tab[i] != NULL)
			dis_tab_insert(n, ncap, d->tab[i]);
	if (d->tab != NULL)
		pfree(d->tab);
	d->tab = n;
	d->cap = ncap;
}

static uint64
dis_hash(const SiftDisOn *d, const void *key, uint32 klen)
{
	uint64		h = hash_bytes_extended((const unsigned char *) key, (int) klen, d->level + 100);

	h *= UINT64_C(0xD6E8FEB86659FD93);
	return h ^ (h >> 32);
}

static void
dis_put_rec(SiftDisOn *d, uint32 part, uint64 tag, const void *key, uint32 klen, const void *ord, uint32 olen)
{
	sift_bytes_reset(&d->rec);
	sift_bytes_put(&d->rec, &klen, sizeof(klen));
	sift_bytes_put(&d->rec, &olen, sizeof(olen));
	sift_bytes_put(&d->rec, key, klen);
	sift_bytes_put(&d->rec, ord, olen);
	sift_spill_put(d->spill, part, tag, d->rec.buf, d->rec.len);
}

/* Over the budget: the best row of every group goes to its partition, and so does every row that follows */
static void
dis_evict_all(SiftDisOn *d)
{
	uint32		nparts = sift_spill_nparts(d->expect ? d->expect : d->budget * 4, d->budget);

	d->spill = sift_spill_create(d->cx, nparts);
	d->st.max_level = Max(d->st.max_level, d->level + 1);
	for (uint32 i = 0; i < d->cap; i++)
	{
		DisGroup   *g = d->tab[i];

		if (g == NULL)
			continue;
		dis_put_rec(d, sift_spill_part_of(g->hash, nparts), g->tag, g->data, g->klen, g->data + g->klen, g->olen);
		pfree(g);
	}
	pfree(d->tab);
	d->tab = NULL;
	d->cap = 0;
	d->ngroups = 0;
	d->gbytes = 0;
}

void
sift_dison_offer(SiftDisOn *d, const void *key, uint32 klen, const void *ord, uint32 olen, uint64 tag)
{
	uint64		h = dis_hash(d, key, klen);
	DisGroup   *g;
	uint32		i;

	d->st.rows++;
	if (d->spill != NULL)
	{
		dis_put_rec(d, sift_spill_part_of(h, sift_spill_nparts_of(d->spill)), tag, key, klen, ord, olen);
		return;
	}
	if (d->cap > 0)
	{
		for (i = (uint32) h & (d->cap - 1); (g = d->tab[i]) != NULL; i = (i + 1) & (d->cap - 1))
			if (g->hash == h && g->klen == klen && memcmp(g->data, key, klen) == 0)
				break;
		if (g != NULL)
		{
			if (key_cmp(ord, olen, g->data + klen, g->olen) >= 0)
				return;
			if (olen > g->ocap)
			{
				DisGroup   *n = MemoryContextAlloc(d->cx, sizeof(DisGroup) + klen + olen);

				d->gbytes += dis_group_bytes(klen, olen) - dis_group_bytes(klen, g->ocap);
				memcpy(n, g, sizeof(DisGroup) + klen);
				n->ocap = olen;
				pfree(g);
				d->tab[i] = g = n;
			}
			memcpy(g->data + klen, ord, olen);
			g->olen = olen;
			g->tag = tag;
			d->st.peak_bytes = Max(d->st.peak_bytes, dis_bytes_of(d));
			return;
		}
	}
	if (d->level < DIS_MAX_LEVEL && d->ngroups > 0 && dis_bytes_of(d) + dis_group_bytes(klen, olen) + (uint64) Max(d->cap, DIS_MIN_CAP) * sizeof(DisGroup *) > d->budget)
	{
		dis_evict_all(d);
		dis_put_rec(d, sift_spill_part_of(h, sift_spill_nparts_of(d->spill)), tag, key, klen, ord, olen);
		return;
	}
	if (d->cap == 0 || (d->ngroups + 1) * 10 > (uint64) d->cap * 7)
		dis_tab_grow(d);
	g = MemoryContextAlloc(d->cx, sizeof(DisGroup) + klen + olen);
	g->hash = h;
	g->tag = tag;
	g->klen = klen;
	g->olen = olen;
	g->ocap = olen;
	g->pad = 0;
	memcpy(g->data, key, klen);
	memcpy(g->data + klen, ord, olen);
	dis_tab_insert(d->tab, d->cap, g);
	d->ngroups++;
	d->gbytes += dis_group_bytes(klen, olen);
	d->st.peak_bytes = Max(d->st.peak_bytes, dis_bytes_of(d));
}

void
sift_dison_update_i64(SiftDisOn *d, const SiftBatchSel *bs, const SiftVec *key, const SiftVec *ord, SiftOrdSpec ord_spec, uint64 tag_base)
{
	SiftBatchIter it;
	uint32		r;
	SiftBytes	k, o;

	sift_bytes_init(&k, d->cx);
	sift_bytes_init(&o, d->cx);
	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
	{
		SiftOrdSpec asc = {false, false};

		sift_bytes_reset(&k);
		sift_bytes_reset(&o);
		if (sift_vec_valid_at(key, r))
			sift_ord_i64(&k, asc, key->type == SVEC_I32 ? ((const int32 *) key->data)[r] : ((const int64 *) key->data)[r]);
		else
			sift_ord_null(&k, asc);
		if (sift_vec_valid_at(ord, r))
			sift_ord_i64(&o, ord_spec, ord->type == SVEC_I32 ? ((const int32 *) ord->data)[r] : ((const int64 *) ord->data)[r]);
		else
			sift_ord_null(&o, ord_spec);
		sift_dison_offer(d, k.buf, k.len, o.buf, o.len, tag_base + r);
	}
	sift_bytes_free(&k);
	sift_bytes_free(&o);
}

static void
dis_free_groups(SiftDisOn *d)
{
	for (uint32 i = 0; i < d->cap; i++)
		if (d->tab[i] != NULL)
			pfree(d->tab[i]);
	if (d->tab != NULL)
		pfree(d->tab);
	d->tab = NULL;
	d->cap = 0;
	d->ngroups = 0;
	d->gbytes = 0;
}

static void
dis_merge_child(SiftDisOn *d, const SiftDisOn *c)
{
	d->st.passes += c->st.passes + 1;
	d->st.max_level = Max(d->st.max_level, c->st.max_level);
	d->st.spilled_records += c->st.spilled_records;
	d->st.spilled_bytes += c->st.spilled_bytes;
	d->st.peak_bytes = Max(d->st.peak_bytes, c->st.peak_bytes);
}

void
sift_dison_finish(SiftDisOn *d, SiftDisOnEmit emit, void *emit_arg)
{
	uint8	   *buf = NULL;
	uint32		cap = 0;

	if (d->spill == NULL)
	{
		for (uint32 i = 0; i < d->cap; i++)
			if (d->tab[i] != NULL)
				emit(emit_arg, d->tab[i]->tag, d->tab[i]->data, d->tab[i]->klen);
		dis_free_groups(d);
		return;
	}
	d->st.spilled_records = sift_spill_total_records(d->spill);
	d->st.spilled_bytes = sift_spill_total_bytes(d->spill);
	for (uint32 p = 0; p < sift_spill_nparts_of(d->spill); p++)
	{
		SiftDisOn  *c;
		uint64		tag;
		uint32		len;

		if (sift_spill_part_records(d->spill, p) == 0)
			continue;
		sift_spill_rewind(d->spill, p);
		c = dis_new(d->cx, d->level + 1, d->budget, 0);
		while (sift_spill_get(d->spill, p, &tag, &buf, &cap, &len))
		{
			uint32		klen, olen;

			memcpy(&klen, buf, sizeof(uint32));
			memcpy(&olen, buf + sizeof(uint32), sizeof(uint32));
			sift_dison_offer(c, buf + 2 * sizeof(uint32), klen, buf + 2 * sizeof(uint32) + klen, olen, tag);
		}
		sift_spill_drop(d->spill, p);
		sift_dison_finish(c, emit, emit_arg);
		dis_merge_child(d, c);
		sift_dison_free(c);
	}
	if (buf != NULL)
		pfree(buf);
	sift_spill_free(d->spill);
	d->spill = NULL;
}

const SiftDisOnStats *
sift_dison_stats(const SiftDisOn *d)
{
	return &d->st;
}

void
sift_dison_free(SiftDisOn *d)
{
	dis_free_groups(d);
	if (d->spill != NULL)
		sift_spill_free(d->spill);
	sift_bytes_free(&d->rec);
	pfree(d);
}
