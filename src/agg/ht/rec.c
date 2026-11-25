/*
 * agg/ht/rec.c - the records of agg/ht/rec.h.
 */
#include "core/host.h"

#include <stdalign.h>

#include "agg/ht/rec.h"
#include "agg/vec.h"

enum
{
	REC_STATE = 1,
	REC_DISTINCT = 2,
	REC_DISTINCT_BYTES = 3
};

StaticAssertDecl(REC_DISTINCT_BYTES < SIFT_REC_OWN, "the kinds of a shape's own records follow those of this file");

typedef struct RecHdr
{
	uint32		len;
	uint32		kind;
} RecHdr;

#define REC_DIST_FIXED		(sizeof(RecHdr) + 8 + sizeof(SiftInlineKey16) + 8)
#define REC_STATE_FIXED		(sizeof(RecHdr) + sizeof(SiftAggSlot64))

/* The key of the slot as the bytes a record carries after its fixed part: the wide key, or nothing */
static const uint8 *
slot_key_bytes(const SiftSaltedHT *ht, const SiftAggSlot64 *s, uint32 *len)
{
	if (!ht->key_is_wide)
	{
		*len = 0;
		return NULL;
	}
	return sift_ht_wide_key(ht, s, len);
}

static void
buf_reserve(SiftRecBuf *b, uint32 len)
{
	if (len > b->cap)
	{
		b->cap = Max(len, Max(b->cap * 2, 1024));
		b->p = b->p ? repalloc(b->p, b->cap) : palloc(b->cap);
	}
}

static uint32
build_state(SiftRecBuf *b, const SiftSaltedHT *ht, const SiftRadixShape *sh, const SiftAggSlot64 *s)
{
	uint32		klen;
	const uint8 *kb = slot_key_bytes(ht, s, &klen);
	const uint32 len = TYPEALIGN(8, REC_STATE_FIXED + ht->ext_stride + klen);
	const RecHdr h = {len, REC_STATE};
	uint8	   *at;

	buf_reserve(b, len);
	memset(b->p, 0, len);
	memcpy(b->p, &h, sizeof(h));
	memcpy(b->p + sizeof(h), s, sizeof(*s));
	at = b->p + REC_STATE_FIXED;
	if (ht->ext_stride)
	{
		memcpy(at, sift_ht_ext(ht, s), ht->ext_stride);
		if (sh->ext_dist_off != SIFT_HT_NO_EXT)
			memset(at + sh->ext_dist_off, 0, sizeof(SiftDistSet *));
		at += ht->ext_stride;
	}
	if (klen)
		memcpy(at, kb, klen);
	return len;
}

/* A value record: the hash and key of the group, the value (an 8 byte image, or for bytes the length of the value), the key bytes, then the bytes of a wide value */
static uint32
build_distinct(SiftRecBuf *b, const SiftSaltedHT *ht, const SiftAggSlot64 *s, uint32 kind, uint64 v, const uint8 *vbytes, uint32 vlen)
{
	uint32		klen;
	const uint8 *kb = slot_key_bytes(ht, s, &klen);
	const uint32 len = TYPEALIGN(8, REC_DIST_FIXED + klen + vlen);
	const RecHdr h = {len, kind};
	uint8	   *at;

	buf_reserve(b, len);
	memset(b->p, 0, len);
	at = b->p;
	memcpy(at, &h, sizeof(h));
	at += sizeof(h);
	memcpy(at, &s->hash64, 8);
	at += 8;
	memcpy(at, &s->key, sizeof(SiftInlineKey16));
	at += sizeof(SiftInlineKey16);
	memcpy(at, &v, 8);
	at += 8;
	if (klen)
		memcpy(at, kb, klen);
	if (vlen)
		memcpy(at + klen, vbytes, vlen);
	return len;
}

void
sift_rec_flush_ht(const SiftSaltedHT *ht, const SiftRadixShape *sh, SiftRecBuf *buf, SiftRecPut put, void *arg)
{
	SiftAggSlot64 *s;

	if (sh->flush != NULL)
	{
		sh->flush(sh, ht, buf, put, arg);
		return;
	}
	SIFT_HT_FOREACH(ht, s)
	{
		uint32		len = build_state(buf, ht, sh, s);

		put(arg, s->hash64, buf->p, len);
		if (sh->ext_dist_off != SIFT_HT_NO_EXT)
		{
			const SiftDistSet *set = *(SiftDistSet **) (sift_ht_ext(ht, s) + sh->ext_dist_off);
			uint32		pos = 0,
						vlen;
			const uint8 *vbytes;
			uint64		v;

			if (set && set->wide)
				while (sift_dist_next_bytes(set, &pos, &vbytes, &vlen))
				{
					len = build_distinct(buf, ht, s, REC_DISTINCT_BYTES, vlen, vbytes, vlen);
					put(arg, s->hash64, buf->p, len);
				}
			else
				while (set && sift_dist_next(set, &pos, &v))
				{
					len = build_distinct(buf, ht, s, REC_DISTINCT, v, NULL, 0);
					put(arg, s->hash64, buf->p, len);
				}
		}
	}
}

static SiftAggSlot64 *
merge_group(SiftRecMerge *m, uint64 hash, const SiftInlineKey16 *key, const uint8 *kbytes, uint32 klen)
{
	SiftAggSlot64 *d;
	bool		found;

	d = m->shape->wide_key ? sift_ht_upsert_wide(m->ht, hash, kbytes, klen, key->wide.key_nulls, &found)
		: sift_ht_upsert(m->ht, hash, key, &found);
	if (!found && m->shape->init)
		m->shape->init(m->shape, d, m->ht->ext_stride ? sift_ht_ext(m->ht, d) : NULL);
	return d;
}

static const uint8 *
aligned_ext(SiftRecMerge *m, const uint8 *ext)
{
	if (m->ht->ext_stride == 0)
		return NULL;
	if (m->ext_copy == NULL)
		m->ext_copy = MemoryContextAllocAligned(m->ht->cxt, m->ht->ext_stride, alignof(SiftI128), 0);
	return memcpy(m->ext_copy, ext, m->ht->ext_stride);
}

void
sift_rec_merge(SiftRecMerge *m, const uint8 *rec)
{
	const SiftRadixShape *sh = m->shape;
	RecHdr		h;

	memcpy(&h, rec, sizeof(h));
	if (h.kind >= SIFT_REC_OWN)
		sh->merge_rec(sh, m, rec);
	else if (h.kind == REC_STATE)
	{
		SiftAggSlot64 src;
		const uint8 *ext = rec + REC_STATE_FIXED;
		SiftAggSlot64 *d;

		memcpy(&src, rec + sizeof(h), sizeof(src));
		d = merge_group(m, src.hash64, &src.key, ext + m->ht->ext_stride, src.key.wide.key_len);
		if (sh->merge)
			sh->merge(sh, d, m->ht->ext_stride ? sift_ht_ext(m->ht, d) : NULL, &src, aligned_ext(m, ext));
	}
	else
	{
		SiftInlineKey16 key;
		uint64		hash,
					v;
		const uint8 *at = rec + sizeof(h);
		SiftAggSlot64 *d;
		SiftDistSet **set;
		uint64		before;
		uint32		klen;

		Assert(h.kind == REC_DISTINCT || h.kind == REC_DISTINCT_BYTES);
		memcpy(&hash, at, 8);
		memcpy(&key, at + 8, sizeof(key));
		memcpy(&v, at + 8 + sizeof(key), 8);
		klen = sh->wide_key ? key.wide.key_len : 0;
		d = merge_group(m, hash, &key, at + 16 + sizeof(key), klen);
		set = (SiftDistSet **) (sift_ht_ext(m->ht, d) + sh->ext_dist_off);
		before = *set ? sift_dist_bytes(*set) : 0;
		if (*set == NULL)
		{
			MemoryContext old = MemoryContextSwitchTo(m->ht->cxt);

			*set = sift_dist_new();
			MemoryContextSwitchTo(old);
		}
		if (h.kind == REC_DISTINCT_BYTES)
			sift_dist_add_bytes(*set, at + 16 + sizeof(key) + klen, (uint32) v);
		else
			sift_dist_add(*set, v);
		m->dist_bytes += sift_dist_bytes(*set) - before;
	}
}
