/*
 * agg/group/fast.c - packed keys, batched byte keys and the text minimum and maximum of the groups of SiftAggV6 (agg/group/fast.h).
 */
/* check-memory: allow-allocs the rows of the wide batch are one batch of at most SIFT_VEC_LEN rows */
#include "postgres.h"

#include "agg/kern/kern.h"
#include "agg/group/fast.h"
#include "core/memtrack.h"

/* ---- packed keys ---- */

bool
sift_pack_plan(const uint8 *width, const bool *sign, int nk, SiftPackSpec *ps)
{
	int			at = 0;

	*ps = (SiftPackSpec) {.nk = nk};
	if (nk < 2 || nk > SIFT_PACK_MAX_KEYS)
		return false;
	for (int i = 0; i < nk; i++)
	{
		if (width[i] != 1 && width[i] != 2 && width[i] != 4 && width[i] != 8)
			return false;
		ps->width[i] = width[i];
		ps->sign[i] = sign[i];
		ps->shift[i] = (uint8) (at * 8);
		at += width[i];
	}
	return at <= SIFT_PACK_MAX_BYTES;
}

static inline int64
vec_int(const SiftVec *v, uint16 r)
{
	switch (v->type)
	{
		case SVEC_I32:
			return ((const int32 *) v->data)[r];
		case SVEC_BOOL:
			return ((const uint8 *) v->data)[r];
		default:
			return ((const int64 *) v->data)[r];
	}
}

uint64
sift_pack_key(const SiftPackSpec *ps, const int64 *x, uint64 nulls, SiftInlineKey16 *k)
{
	unsigned __int128 v = 0;

	for (int i = 0; i < ps->nk; i++)
		if (!((nulls >> i) & 1))
		{
			const uint64 m = ps->width[i] == 8 ? UINT64_MAX : ((UINT64CONST(1) << (ps->width[i] * 8)) - 1);

			v |= (unsigned __int128) ((uint64) x[i] & m) << ps->shift[i];
		}
	k->u64[0] = (uint64) v;
	k->u64[1] = (uint64) (uint32) (v >> 64) | (nulls << 32);
	return sift_pack_hash(k);
}

void
sift_pack_batch(const SiftPackSpec *ps, SiftVec *const *kv, const uint16 *sel, uint16 n, uint64 *hash, SiftInlineKey16 *keys)
{
	for (uint16 k = 0; k < n; k++)
	{
		int64		x[SIFT_PACK_MAX_KEYS];
		uint64		nulls = 0;
		const uint16 r = sel[k];

		for (int i = 0; i < ps->nk; i++)
		{
			const SiftVec *v = kv[i];
			const bool	valid = v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1);

			x[i] = valid ? vec_int(v, r) : 0;
			nulls |= (uint64) !valid << i;
		}
		hash[k] = sift_pack_key(ps, x, nulls, &keys[k]);
	}
}

bool
sift_pack_get(const SiftPackSpec *ps, const SiftInlineKey16 *k, int i, int64 *x)
{
	const unsigned __int128 v = ((unsigned __int128) (uint32) k->u64[1] << 64) | k->u64[0];
	const int	bits = ps->width[i] * 8;
	uint64		u;

	if ((k->u64[1] >> (32 + i)) & 1)
		return false;
	u = (uint64) (v >> ps->shift[i]);
	if (bits < 64)
	{
		u &= (UINT64CONST(1) << bits) - 1;
		if (ps->sign[i] && ((u >> (bits - 1)) & 1))
			u |= ~((UINT64CONST(1) << bits) - 1);
	}
	*x = (int64) u;
	return true;
}

/* ---- keys as bytes ---- */

void
sift_wide_batch_init(SiftWideBatch *wb, MemoryContext cx, uint16 rows)
{
	*wb = (SiftWideBatch) {.cap = 64 * 1024,.cap_rows = rows};
	wb->buf = MemoryContextAlloc(cx, wb->cap);
	wb->off = MemoryContextAlloc(cx, rows * sizeof(uint32)); /* check-memory: allow rows is a uint16 */
	wb->len = MemoryContextAlloc(cx, rows * sizeof(uint32)); /* check-memory: allow rows is a uint16 */
	wb->nulls = MemoryContextAlloc(cx, rows * sizeof(uint64)); /* check-memory: allow rows is a uint16 */
	wb->hash = MemoryContextAlloc(cx, rows * sizeof(uint64)); /* check-memory: allow rows is a uint16 */
}

void
sift_wide_batch_free(SiftWideBatch *wb)
{
	pfree(wb->buf);
	pfree(wb->off);
	pfree(wb->len);
	pfree(wb->nulls);
	pfree(wb->hash);
}

static inline void
batch_room(SiftWideBatch *wb, size_t need)
{
	if (need > wb->cap)
	{
		wb->cap = sift_alloc_grow(wb->cap, need, "the keys of a batch of groups");
		wb->buf = repalloc(wb->buf, wb->cap);
	}
}

void
sift_wide_batch_build(SiftWideBatch *wb, const int *kidx, int nk, SiftVec *const *keyvec, const uint16 *sel, uint16 n)
{
	size_t		at = 0;

	for (uint16 k = 0; k < n; k++)
	{
		const uint16 r = sel[k];
		uint64		nulls = 0;

		wb->off[k] = (uint32) at;
		for (int i = 0; i < nk; i++)
		{
			const SiftVec *kv = keyvec[kidx[i]];

			if (!(kv->all_valid || ((kv->valid[r >> 6] >> (r & 63)) & 1)))
				nulls |= UINT64CONST(1) << i;
			else if (kv->type == SVEC_DICT32)
			{
				const SiftGranuleDict *d = kv->dict;
				const uint32 c = ((const uint32 *) kv->data)[r];
				const uint32 len = d->off[c + 1] - d->off[c];

				batch_room(wb, at + sizeof(uint32) + len);
				memcpy(wb->buf + at, &len, sizeof(uint32));
				memcpy(wb->buf + at + sizeof(uint32), d->bytes + d->off[c], len);
				at += sizeof(uint32) + len;
			}
			else if (kv->type == SVEC_STRVIEW)
			{
				const SiftStrView *s = &((const SiftStrView *) kv->data)[r];

				batch_room(wb, at + sizeof(uint32) + s->len);
				memcpy(wb->buf + at, &s->len, sizeof(uint32));
				memcpy(wb->buf + at + sizeof(uint32), sift_strview_bytes(s), s->len);
				at += sizeof(uint32) + s->len;
			}
			else
			{
				const int64 x = vec_int(kv, r);

				batch_room(wb, at + sizeof(int64));
				memcpy(wb->buf + at, &x, sizeof(x));
				at += sizeof(x);
			}
		}
		wb->len[k] = (uint32) (at - wb->off[k]);
		wb->nulls[k] = nulls;
	}
	sift_aggk6_hash_wide_batch(wb->hash, wb->buf, wb->off, wb->len, wb->nulls, n);
}

void
sift_wide_batch_upsert(SiftSaltedHT *ht, const SiftWideBatch *wb, uint16 n, SiftAggSlot64 **slots)
{
	uint32		gen = ht->gen;

	for (uint16 k = 0; k < n; k++)
	{
		bool		found;

		if (k + SIFT_HT_PREFETCH < n)
			sift_ht_prefetch_group(ht, wb->hash[k + SIFT_HT_PREFETCH]);
		if (k + SIFT_HT_PREFETCH_SLOT < n)
			sift_ht_prefetch_slot(ht, wb->hash[k + SIFT_HT_PREFETCH_SLOT]);
		if (k + SIFT_HT_PREFETCH_KEY < n)
			sift_ht_prefetch_key(ht, wb->hash[k + SIFT_HT_PREFETCH_KEY]);
		slots[k] = sift_ht_upsert_wide_lazy(ht, wb->hash[k], wb->buf + wb->off[k], wb->len[k], wb->nulls[k], &found);
		if (ht->gen != gen)
		{
			for (uint16 j = 0; j < k; j++)
				slots[j] = sift_ht_find_wide(ht, wb->hash[j], wb->buf + wb->off[j], wb->len[j], wb->nulls[j]);
			gen = ht->gen;
		}
	}
}

/* The group of the null key: what sift_wide_batch_build makes of a NULL (no bytes, the null flag) */
static inline uint64
null_hash(void)
{
	return sift_aggk6_hash1(sift_aggk6_hash_bytes("", 0) ^ 1);
}

void
sift_str_batch_upsert(SiftSaltedHT *ht, const SiftVec *kv, const uint16 *sel, uint16 n, uint64 *hash, SiftAggSlot64 **slots)
{
	const SiftStrView *v = kv->data;
	const uint64 *valid = kv->all_valid ? NULL : kv->valid;
	uint32		gen = ht->gen;

	sift_aggk6_hash_str_batch(hash, v, valid, sel, n);
	for (uint16 k = 0; k < n; k++)
	{
		const bool	null = valid != NULL && !((valid[sel[k] >> 6] >> (sel[k] & 63)) & 1);
		bool		found;

		if (null)
			hash[k] = null_hash();
		if (k + SIFT_HT_PREFETCH < n)
			sift_ht_prefetch_group(ht, hash[k + SIFT_HT_PREFETCH]);
		if (k + SIFT_HT_PREFETCH_SLOT < n)
			sift_ht_prefetch_slot(ht, hash[k + SIFT_HT_PREFETCH_SLOT]);
		if (k + SIFT_HT_PREFETCH_KEY < n)
			sift_ht_prefetch_key(ht, hash[k + SIFT_HT_PREFETCH_KEY]);
		slots[k] = null ? sift_ht_upsert_wide_lazy(ht, hash[k], NULL, 0, 1, &found) :
			sift_ht_upsert_lenpfx_lazy(ht, hash[k], sift_strview_bytes(&v[sel[k]]), v[sel[k]].len, &found);
		if (ht->gen != gen)
		{
			for (uint16 j = 0; j < k; j++)
				slots[j] = slots[j] == NULL ? NULL : (valid != NULL && !((valid[sel[j] >> 6] >> (sel[j] & 63)) & 1)) ? sift_ht_find_wide(ht, hash[j], NULL, 0, 1) :
					sift_ht_find_lenpfx(ht, hash[j], sift_strview_bytes(&v[sel[j]]), v[sel[j]].len);
			gen = ht->gen;
		}
	}
}

/* ---- the minimum and maximum of a text column ---- */

#define SLAB_CHUNK		(256 * 1024)

typedef struct SlabChunk
{
	struct SlabChunk *next;
	size_t		used;
	size_t		cap;
	uint8		data[FLEXIBLE_ARRAY_MEMBER];
} SlabChunk;

struct SiftStrSlab
{
	MemoryContext cx;
	SlabChunk  *head;
	uint64		bytes;
};

SiftStrSlab *
sift_strslab_new(MemoryContext cx)
{
	SiftStrSlab *sl = MemoryContextAllocZero(cx, sizeof(SiftStrSlab));

	sl->cx = cx;
	return sl;
}

uint64
sift_strslab_bytes(const SiftStrSlab *sl)
{
	return sl->bytes;
}

void
sift_strslab_free(SiftStrSlab *sl)
{
	for (SlabChunk *c = sl->head, *next; c != NULL; c = next)
	{
		next = c->next;
		pfree(c);
	}
	pfree(sl);
}

static uint8 *
slab_take(SiftStrSlab *sl, uint32 len)
{
	uint8	   *p;

	if (sl->head == NULL || sl->head->cap - sl->head->used < len)
	{
		const size_t cap = Max((size_t) SLAB_CHUNK, (size_t) len);
		SlabChunk  *c = MemoryContextAlloc(sl->cx, offsetof(SlabChunk, data) + cap);

		*c = (SlabChunk) {.next = sl->head,.cap = cap};
		sl->head = c;
		sl->bytes += cap;
	}
	p = sl->head->data + sl->head->used;
	sl->head->used += len;
	return p;
}

static inline uint64
be_prefix(const uint8 *p, uint32 len)
{
	uint64		w = 0;

	memcpy(&w, p, Min(len, 8));
	return __builtin_bswap64(w);
}

/* the order of the bytes of two strings, the shorter first when one begins the other */
static inline int
str_cmp(const uint8 *a, uint32 alen, const uint8 *b, uint32 blen)
{
	const int	c = memcmp(a, b, Min(alen, blen));

	return c != 0 ? c : (alen < blen ? -1 : alen > blen);
}

/* the string p of len bytes as the state's best: inline when it fits and the state owns no block, else in its block, made twice as large as the longest string it has had */
static void
strmm_put(SiftStrSlab *sl, SiftStrMM *a, uint64 pfx, const uint8 *p, uint32 len)
{
	const bool	owns = a->n != 0 && a->cap != 0 && a->cap != SIFT_STRMM_INLINE_CAP;

	if (!owns && len <= SIFT_STRMM_INLINE_MAX)
	{
		memset(a->u.tail, 0, sizeof(a->u.tail));
		if (len > 8)
			memcpy(a->u.tail, p + 8, len - 8);
		a->cap = SIFT_STRMM_INLINE_CAP;
	}
	else
	{
		if (!owns || len > a->cap)
		{
			const uint32 cap = len > (1u << 30) ? len : Max(32u, pg_nextpower2_32(len));

			a->u.ptr = slab_take(sl, cap);
			a->cap = cap;
		}
		if (len > 0)
			memcpy(a->u.ptr, p, len);
	}
	a->len = len;
	a->pfx = pfx;
}

/* whether the string p of len bytes (prefix pfx) comes before (is_min) or after the best of a */
static inline bool
strmm_better(const SiftStrMM *a, bool is_min, uint64 pfx, const uint8 *p, uint32 len)
{
	uint8		buf[16];
	int			c;

	if (a->n == 0)
		return true;
	if (pfx != a->pfx)
		return is_min ? pfx < a->pfx : pfx > a->pfx;
	c = str_cmp(p, len, sift_strmm_bytes(a, buf), a->len);
	return is_min ? c < 0 : c > 0;
}

void
sift_strmm_offer(SiftStrSlab *sl, SiftStrMM *a, bool is_min, const uint8 *p, uint32 len)
{
	const uint64 pfx = be_prefix(p, len);

	if (strmm_better(a, is_min, pfx, p, len))
		strmm_put(sl, a, pfx, p, len);
	a->n++;
}

void
sift_strmm_merge(SiftStrMM *d, const SiftStrMM *s, bool is_min)
{
	uint8		buf[16];

	if (s->n == 0)
		return;
	if (strmm_better(d, is_min, s->pfx, sift_strmm_bytes(s, buf), s->len))
	{
		if (s->cap == SIFT_STRMM_INLINE_CAP)
		{
			d->pfx = s->pfx;
			memcpy(d->u.tail, s->u.tail, sizeof(d->u.tail));
			d->cap = SIFT_STRMM_INLINE_CAP;
		}
		else
		{
			d->pfx = s->pfx;
			d->u.ptr = s->u.ptr;
			d->cap = 0;
		}
		d->len = s->len;
	}
	d->n += s->n;
}
