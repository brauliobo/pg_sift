/*
 * agg/v6/v6_group_fast.c - packed keys, batched byte keys and the text minimum and maximum of the groups of SiftAggV6 (agg/v6/v6_group_fast.h).
 */
#include "postgres.h"

#include "agg/kern6.h"
#include "agg/v6/v6_group_fast.h"
#include "col/granule/granule.h"

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

static inline uint64
pack_hash(uint64 lo, uint64 hi)
{
	return sift_aggk6_hash1(lo ^ sift_aggk6_hash1(hi));
}

uint64
sift_pack_hash(const SiftInlineKey16 *k)
{
	return pack_hash(k->u64[0], k->u64[1]);
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
	return pack_hash(k->u64[0], k->u64[1]);
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
	wb->off = MemoryContextAlloc(cx, rows * sizeof(uint32));
	wb->len = MemoryContextAlloc(cx, rows * sizeof(uint32));
	wb->nulls = MemoryContextAlloc(cx, rows * sizeof(uint64));
	wb->hash = MemoryContextAlloc(cx, rows * sizeof(uint64));
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
		while (need > wb->cap)
			wb->cap *= 2;
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
	for (uint16 k = 0; k < n; k++)
		wb->hash[k] = sift_aggk6_hash1(sift_aggk6_hash_bytes(wb->buf + wb->off[k], wb->len[k]) ^ wb->nulls[k]);
}

void
sift_wide_batch_upsert(SiftSaltedHT *ht, const SiftWideBatch *wb, uint16 n, SiftAggSlot64 **slots)
{
	for (uint16 k = 0; k < n; k++)
	{
		bool		found;

		if (k + SIFT_HT_PREFETCH < n)
		{
			const uint32 g = (uint32) wb->hash[k + SIFT_HT_PREFETCH] & ht->mask;

			__builtin_prefetch(&ht->ctrl[g]);
			__builtin_prefetch(&ht->slots[g]);
		}
		slots[k] = sift_ht_upsert_wide(ht, wb->hash[k], wb->buf + wb->off[k], wb->len[k], wb->nulls[k], &found);
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

void
sift_strmm_offer(SiftStrSlab *sl, SiftStrMM *a, bool is_min, const uint8 *p, uint32 len)
{
	const uint64 pfx = be_prefix(p, len);
	bool		better;

	if (a->n == 0)
		better = true;
	else if (pfx != a->pfx)
		better = is_min ? pfx < a->pfx : pfx > a->pfx;
	else
	{
		const int	c = str_cmp(p, len, a->ptr, a->len);

		better = is_min ? c < 0 : c > 0;
	}
	a->n++;
	if (!better)
		return;
	if (len > a->cap)
	{
		a->ptr = slab_take(sl, len);
		a->cap = len;
	}
	if (len > 0)
		memcpy(a->ptr, p, len);
	a->len = len;
	a->pfx = pfx;
}

void
sift_strmm_merge(SiftStrMM *d, const SiftStrMM *s, bool is_min)
{
	bool		better;

	if (s->n == 0)
		return;
	if (d->n == 0)
		better = true;
	else if (s->pfx != d->pfx)
		better = is_min ? s->pfx < d->pfx : s->pfx > d->pfx;
	else
	{
		const int	c = str_cmp(s->ptr, s->len, d->ptr, d->len);

		better = is_min ? c < 0 : c > 0;
	}
	d->n += s->n;
	if (better)
	{
		d->ptr = s->ptr;
		d->len = s->len;
		d->pfx = s->pfx;
		d->cap = 0;
	}
}
