/*
 * agg/v6/group/str.c - the compact group table of one text key (agg/v6/group/cmp.h): one table of 16-byte slots, four to a cache line, and an arena of the strings. A slot is word 0, the 32-bit
 * hash of the string and its place in the arena, and word 1, the length of the string and the count of its rows. A row costs one probe of the table, and one compare of the bytes when a slot's hash
 * and length are the string's. The rows of a dictionary-coded batch find the group of a code by the place of its string, with no compare of bytes.
 */
#include "postgres.h"

#include "agg/v6/group/cmp.h"

#define SSTR_AHEAD		16

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
				c->code_ent[code] = cmp_h32(sift_aggk6_hash1(sift_aggk6_hash_lenpfx(d->bytes + d->off[code], d->off[code + 1] - d->off[code])));
			g->hash[k] = (uint32) c->code_ent[code];
		}
		return;
	}
	sift_aggk6_hash_str_batch(g->hash, kv->data, valid, g->sel, n);
	for (uint16 k = 0; k < n; k++)
		g->hash[k] = valid != NULL && !((valid[g->sel[k] >> 6] >> (g->sel[k] & 63)) & 1) ? 0 : cmp_h32(g->hash[k]);
}

static inline uint64 *
row_group(SiftCompact *c, const SiftVec *kv, uint16 r, uint32 h32)
{
	const char *p;
	uint32		len;

	if (kv->type == SVEC_DICT32)
	{
		const SiftGranuleDict *d = kv->dict;
		const uint32 code = ((const uint32 *) kv->data)[r];
		uint64	   *s;

		if ((c->code_ent[code] >> 32) != 0)
			return sstr_find_off(c, h32, (uint32) (c->code_ent[code] >> 32) - 1);
		s = sstr_upsert(c, h32, d->bytes + d->off[code], d->off[code + 1] - d->off[code]);
		c->code_ent[code] = h32 | ((uint64) (sl_off(s) + 1) << 32);
		return s;
	}
	row_str(kv, r, &p, &len);
	return sstr_upsert(c, h32, p, len);
}

void
sstr_batch(SiftCompact *c, SiftV6Group *g, const SiftVec *kv, uint16 n)
{
	batch_hash(c, g, kv, n);
	for (uint16 k = 0; k < n; k++)
	{
		uint64	   *s;

		if (k + SSTR_AHEAD < n && g->hash[k + SSTR_AHEAD] != 0)
			__builtin_prefetch(sstr_slot(c, g->hash[k + SSTR_AHEAD] & c->mask), 1);
		s = g->hash[k] == 0 ? c->null_group : row_group(c, kv, g->sel[k], (uint32) g->hash[k]);
		cmp_count_up(c, s);
	}
}
