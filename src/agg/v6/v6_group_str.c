/*
 * agg/v6/v6_group_str.c - the compact group table of one text key (agg/v6/v6_group_cmp.h): one table of 16-byte slots, four to a cache line, and an arena of the strings. A slot is word 0, the 32-bit
 * hash of the string and its place in the arena, and word 1, the length of the string and the count of its rows. A row costs one probe of the table, and one compare of the bytes when a slot's hash
 * and length are the string's. The rows of a dictionary-coded batch find the group of a code by the place of its string, with no compare of bytes.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/kern6.h"
#include "agg/v6/v6_group_cmp.h"

#define SSTR_AHEAD		16

static inline uint32
sl_h32(const uint64 *s)
{
	return (uint32) s[0];
}

static inline uint32
sl_off(const uint64 *s)
{
	return (uint32) (s[0] >> 32);
}

static inline uint32
sl_len(const uint64 *s)
{
	return (uint32) s[1];
}

/* a hash is never 0: the batch marks the rows with a NULL key by 0 */
static inline uint32
h32_of(uint64 h)
{
	return (uint32) h != 0 ? (uint32) h : 1;
}

void
sstr_slot_str(const SiftCompact *c, const uint64 *slot, const char **p, uint32 *len)
{
	*p = c->in.arena + sl_off(slot);
	*len = sl_len(slot);
}

static inline uint64 *
slot_at(const SiftCompact *c, uint64 i)
{
	return c->tab + i * 2;
}

static void
arena_put(SiftCompact *c, MemoryContext cx, const char *p, uint32 len)
{
	Intern	   *in = &c->in;

	if (in->used + len > in->arena_cap)
	{
		in->arena_cap = Max(Max(in->arena_cap * 2, in->used + len), 4096);
		in->arena = in->arena == NULL ? MemoryContextAlloc(cx, in->arena_cap) : repalloc(in->arena, in->arena_cap);
	}
	memcpy(in->arena + in->used, p, len);
	in->used += len;
}

/* The slot of the string, empty when the string is new */
static inline uint64 *
find(const SiftCompact *c, uint32 h32, const char *p, uint32 len)
{
	for (uint64 i = h32 & c->mask;; i = (i + 1) & c->mask)
	{
		uint64	   *s = slot_at(c, i);

		if (cmp_count(c, s) == 0 || (sl_h32(s) == h32 && sl_len(s) == len && memcmp(c->in.arena + sl_off(s), p, len) == 0))
			return s;
	}
}

/* The slot of the string of a code, whose place in the arena is known */
static inline uint64 *
find_off(const SiftCompact *c, uint32 h32, uint32 off)
{
	for (uint64 i = h32 & c->mask;; i = (i + 1) & c->mask)
	{
		uint64	   *s = slot_at(c, i);

		if (sl_h32(s) == h32 && sl_off(s) == off && cmp_count(c, s) != 0)
			return s;
	}
}

/* The group of the string, made when it is new */
static inline uint64 *
upsert(SiftCompact *c, MemoryContext cx, uint32 h32, const char *p, uint32 len)
{
	uint64	   *s = find(c, h32, p, len);

	if (cmp_count(c, s) == 0)
	{
		s[0] = h32 | ((uint64) c->in.used << 32);
		s[1] = len;
		arena_put(c, cx, p, len);
		c->count++;
	}
	return s;
}

void
sstr_move(SiftCompact *c, const uint64 *old, uint64 ocap)
{
	for (uint64 i = 0; i < ocap; i++)
	{
		const uint64 *s = old + i * 2;
		uint64		j;

		if (cmp_count(c, s) == 0)
			continue;
		for (j = sl_h32(s) & c->mask; cmp_count(c, slot_at(c, j)) != 0; j = (j + 1) & c->mask)
			;
		memcpy(slot_at(c, j), s, 2 * sizeof(uint64));
		c->count++;
	}
}

static inline void
row_str(const SiftVec *kv, uint16 r, const char **p, uint32 *len)
{
	const SiftStrView *v = &((const SiftStrView *) kv->data)[r];

	*p = (const char *) sift_strview_bytes(v);
	*len = v->len;
}

/* The hash of each row of the batch (g->hash), 0 for a NULL key; of a dictionary-coded batch the hash of a code is made once, with its entry */
static void
batch_hash(SiftCompact *c, SiftV6Group *g, const SiftVec *kv, uint16 n)
{
	const uint64 *valid = kv->all_valid ? NULL : kv->valid;

	if (kv->type == SVEC_DICT32)
	{
		const SiftGranuleDict *d = kv->dict;

		for (uint16 k = 0; k < n; k++)
		{
			uint32		code;

			g->hash[k] = 0;
			if (!is_valid(kv, g->sel[k]))
				continue;
			code = ((const uint32 *) kv->data)[g->sel[k]];
			if (c->code_ent[code] == 0)
				c->code_ent[code] = h32_of(sift_aggk6_hash1(sift_aggk6_hash_lenpfx(d->bytes + d->off[code], d->off[code + 1] - d->off[code])));
			g->hash[k] = (uint32) c->code_ent[code];
		}
		return;
	}
	sift_aggk6_hash_str_batch(g->hash, kv->data, valid, g->sel, n);
	for (uint16 k = 0; k < n; k++)
		g->hash[k] = valid != NULL && !((valid[g->sel[k] >> 6] >> (g->sel[k] & 63)) & 1) ? 0 : h32_of(g->hash[k]);
}

static inline uint64 *
row_group(SiftCompact *c, MemoryContext cx, const SiftVec *kv, uint16 r, uint32 h32)
{
	const char *p;
	uint32		len;

	if (kv->type == SVEC_DICT32)
	{
		const SiftGranuleDict *d = kv->dict;
		const uint32 code = ((const uint32 *) kv->data)[r];
		uint64	   *s;

		if ((c->code_ent[code] >> 32) != 0)
			return find_off(c, h32, (uint32) (c->code_ent[code] >> 32) - 1);
		s = upsert(c, cx, h32, d->bytes + d->off[code], d->off[code + 1] - d->off[code]);
		c->code_ent[code] = h32 | ((uint64) (sl_off(s) + 1) << 32);
		return s;
	}
	row_str(kv, r, &p, &len);
	return upsert(c, cx, h32, p, len);
}

void
sstr_batch(SiftCompact *c, SiftV6Group *g, const SiftVec *kv, uint16 n)
{
	batch_hash(c, g, kv, n);
	for (uint16 k = 0; k < n; k++)
	{
		uint64	   *s;

		if (k + SSTR_AHEAD < n && g->hash[k + SSTR_AHEAD] != 0)
			__builtin_prefetch(slot_at(c, g->hash[k + SSTR_AHEAD] & c->mask), 1);
		s = g->hash[k] == 0 ? c->null_group : row_group(c, g->cx, kv, g->sel[k], (uint32) g->hash[k]);
		cmp_count_up(c, s);
	}
}
