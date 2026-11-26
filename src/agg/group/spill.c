/*
 * agg/group/spill.c - the groups of SiftAggV6 that leave for the spill when the memory of the table is out, and come back one partition at a time.
 *
 * A group whose key is a value the table holds by a code (agg/datum.h) or whose MIN or MAX of text is a string in the arena of the groups stands for memory the record of its
 * slot does not carry. Its record is made of its values instead (own_flush): the keys as the serialized values of their codes (the hash of the group is that of the values, so that a
 * group meets its other records whatever the codes of the table that made each), the states, and the strings of the text winners. A partition is merged into a table whose codes and arena
 * are its own (own_begin gives the ones of the partition before it back), and the groups of a spill that is read back are returned from that table, one partition after another
 * (v6g_spill_open, v6g_spill_next): what the groups hold is the memory of the table and of one partition.
 */
#include "postgres.h"

#include <stdalign.h>

#include "miscadmin.h"
#include "utils/memutils.h"

#include "agg/gpu/scan.h"
#include "agg/ht/ht.h"
#include "agg/kern/kern.h"
#include "agg/group/int.h"
#include "agg/datum.h"
#include "agg/gen/gen.h"
#include "agg/group/rows.h"
#include "agg/pct.h"

#define OWN_REC		SIFT_REC_OWN
#define OWN_HDR		24			/* len and kind, the hash, the nulls of the keys */

#define SET_OF(sh)	((V6Set *) ((char *) (sh) - offsetof(V6Set, shape)))

/* The bytes the groups hold now, as the allocator has them: the tables whole, the text arena, the code slots, the values of the keys, and the table a batch would grow: old and new at once */
static uint64
group_bytes(const SiftV6Group *g)
{
	uint64		bytes = g->tx.cap + (uint64) g->code_cap * (sizeof(*g->code_slots) + sizeof(*g->code_epoch)) + (g->gkeys != NULL ? sift_v6_genkeys_bytes(g->gkeys) : 0);

	if (g->gen != NULL)
		bytes += v6_gen_bytes(g->gen);
	if (g->slab != NULL)
		bytes += sift_strslab_bytes(g->slab);
	for (int s = 0; s < g->nsets; s++)
	{
		const SiftSaltedHT *ht = &g->set[s].ht;

		if (g->set[s].nk == 0)
			continue;
		bytes += sift_ht_bytes(ht);
		if (ht->count + SIFT_VEC_LEN > ht->grow_at)
			bytes += 2 * (uint64) ht->capacity * (sizeof(SiftAggSlot64) + 1);
	}
	return bytes;
}

/* The keys of a slot as v6g_slot_of takes them (what sift_v6_group_fold_rec is given): the integer or code of each, the strings of the text keys, and bit i of nulls for the i-th key of the set */
static void
slot_keys(const SiftV6Group *g, const V6Set *set, const SiftSaltedHT *ht, const SiftAggSlot64 *s, int64 *key, SiftV6StrIn *str, uint64 *nulls)
{
	*nulls = 0;
	for (int i = 0; i < set->nk; i++)
		str[set->kidx[i]] = (SiftV6StrIn) {0};
	if (set->packed)
		for (int i = 0; i < set->nk; i++)
		{
			int64		x = 0;

			if (!sift_pack_get(&set->pack, &s->key, i, &x))
				*nulls |= UINT64CONST(1) << i;
			key[set->kidx[i]] = x;
		}
	else if (!set->wide)
	{
		key[set->kidx[0]] = s->key.i64[0];
		*nulls = s->key.i64[1] != 0;
	}
	else
	{
		uint32		len;
		const uint8 *p = sift_ht_wide_key(ht, s, &len);

		*nulls = s->key.wide.key_nulls;
		for (int i = 0; i < set->nk; i++)
		{
			const int	k = set->kidx[i];

			if ((*nulls >> i) & 1)
				continue;
			if (sift_v6_class(g->spec->key[k].typid) == SV6C_TEXT)
			{
				uint32		n;

				memcpy(&n, p, sizeof(n));
				str[k] = (SiftV6StrIn) {(const char *) p + sizeof(n), n};
				p += sizeof(n) + n;
			}
			else
			{
				memcpy(&key[k], p, sizeof(int64));
				p += sizeof(int64);
			}
		}
	}
}

SiftAggSlot64 *
v6g_slot_of(SiftV6Group *g, V6Set *set, SiftSaltedHT *ht, const int64 *key, const SiftV6StrIn *str, uint64 nulls)
{
	sift_ht_reserve(ht, 1);
	if (set->packed)
	{
		int64		x[SIFT_PACK_MAX_KEYS];

		for (int i = 0; i < set->nk; i++)
			x[i] = key[set->kidx[i]];
		g->hash[0] = sift_pack_key(&set->pack, x, nulls, &g->ikey[0]);
		sift_ht_upsert_batch(ht, g->hash, g->ikey, 1, g->slots);
		return g->slots[0];
	}
	if (set->wide)
	{
		bool		found;

		resetStringInfo(&g->kbuf);
		for (int i = 0; i < set->nk; i++)
			if (!((nulls >> i) & 1))
			{
				const SiftV6StrIn *s = str != NULL ? &str[set->kidx[i]] : NULL;

				if (s != NULL && s->p != NULL)
					v6g_append_key_str(g, s->p, s->len);
				else
					appendBinaryStringInfo(&g->kbuf, (const char *) &key[set->kidx[i]], sizeof(int64));
			}
		return sift_ht_upsert_wide(ht, sift_aggk6_hash1(sift_aggk6_hash_bytes(g->kbuf.data, g->kbuf.len) ^ nulls), g->kbuf.data, g->kbuf.len, nulls, &found);
	}
	{
		const int64 x = (nulls & 1) ? 0 : key[set->kidx[0]];

		g->ikey[0].i64[0] = x;
		g->ikey[0].i64[1] = nulls & 1;
		g->hash[0] = sift_aggk6_hash1((uint64) x);
		sift_ht_upsert_batch(ht, g->hash, g->ikey, 1, g->slots);
		return g->slots[0];
	}
}

static inline bool
is_generic(const SiftV6Group *g, int k)
{
	return g->gkeys != NULL && sift_v6_genkeys_is(g->gkeys, k);
}

/* the strings of the text winners of a state block: a winner that is not inline (a state of n values has its bytes in the arena of the groups) */
static inline bool
tx_winner(const SiftV6Agg *a, const V6Acc *acc)
{
	return v6g_is_text_mm(a) && acc->n > 0 && !(((const SiftTextSlot *) acc)->pad & SIFT_TSLOT_INLINE);
}

/* A record holds the bytes of a text winner once: the state of a winner equal to the one of an earlier aggregate (MIN and MAX of a group of one value) has that aggregate in its pad, from this bit, plus one */
#define TSLOT_SAME_SHIFT	1

/* the first aggregate before j whose text winner is the one of j, or -1 */
static int
same_winner(const SiftV6Group *g, const uint8 *ext, int j)
{
	const SiftV6Agg *agg = g->spec->agg;
	const SiftTextSlot *a = (const SiftTextSlot *) (ext + g->off[j]);

	if (!tx_winner(&agg[j], (const V6Acc *) a))
		return -1;
	for (int i = 0; i < j; i++)
	{
		const SiftTextSlot *b = (const SiftTextSlot *) (ext + g->off[i]);

		if (tx_winner(&agg[i], (const V6Acc *) b) && b->v1 == a->v1 && memcmp(g->tx.buf + b->v0, g->tx.buf + a->v0, (uint32) a->v1) == 0)
			return i;
	}
	return -1;
}

/* where the bytes of the text winner of aggregate i are in a record whose states start at states and whose winners at base */
static const char *
record_winner(const SiftV6Group *g, const uint8 *states, const char *base, int i)
{
	for (int j = 0; j < i; j++)
	{
		SiftTextSlot t;

		memcpy(&t, states + g->off[j], sizeof(t));
		if (tx_winner(&g->spec->agg[j], (const V6Acc *) &t) && (t.pad >> TSLOT_SAME_SHIFT) == 0)
			base += (uint32) t.v1;
	}
	return base;
}

/* The record of every group of ht: the values of its keys and of its text winners with its states, hashed by those values */
static void
own_flush(const SiftRadixShape *sh, const SiftSaltedHT *ht, SiftRecBuf *buf, SiftRecPut put, void *arg)
{
	V6Set	   *set = SET_OF(sh);
	SiftV6Group *g = set->owner;
	const SiftV6Spec *spec = g->spec;
	StringInfoData out;
	const SiftAggSlot64 *s;
	int64		key[SIFT_V6_MAX_KEYS];
	SiftV6StrIn str[SIFT_V6_MAX_KEYS];

	initStringInfo(&out);
	SIFT_HT_FOREACH(ht, s)
	{
		const uint8 *ext = sift_ht_ext(ht, s);
		uint64		nulls,
					hash;
		uint32		len;

		slot_keys(g, set, ht, s, key, str, &nulls);
		resetStringInfo(&out);
		enlargeStringInfo(&out, OWN_HDR + g->stride);
		out.len = OWN_HDR;
		memset(out.data, 0, OWN_HDR);
		appendBinaryStringInfo(&out, (const char *) ext, g->stride);
		hash = sift_aggk6_hash1(nulls);
		for (int i = 0; i < set->nk; i++)
		{
			const int	k = set->kidx[i];
			uint64		kh;

			if ((nulls >> i) & 1)
				continue;
			if (is_generic(g, k))
			{
				kh = sift_v6_genkeys_hash(g->gkeys, k, key[k]);
				sift_v6_genkeys_put(g->gkeys, k, key[k], &out);
			}
			else if (str[k].p != NULL)
			{
				kh = sift_aggk6_hash_bytes(str[k].p, str[k].len);
				appendBinaryStringInfo(&out, (const char *) &str[k].len, sizeof(uint32));
				appendBinaryStringInfo(&out, str[k].p, str[k].len);
			}
			else
			{
				kh = sift_aggk6_hash1((uint64) key[k]);
				appendBinaryStringInfo(&out, (const char *) &key[k], sizeof(int64));
			}
			hash = sift_aggk6_hash1(hash ^ kh);
		}
		for (int j = 0; j < spec->naggs; j++)
		{
			const V6Acc *acc = (const V6Acc *) (ext + g->off[j]);
			const int	same = same_winner(g, ext, j);

			if (!tx_winner(&spec->agg[j], acc))
				continue;
			((SiftTextSlot *) (out.data + OWN_HDR + g->off[j]))->pad = (int64) (same + 1) << TSLOT_SAME_SHIFT;
			if (same < 0)
				appendBinaryStringInfo(&out, g->tx.buf + ((const SiftTextSlot *) acc)->v0, (int) acc->v1);
		}
		for (int j = 0; j < spec->naggs; j++)
			if (spec->agg[j].kind == SV6_PCT_CONT)
				sift_v6_pct_put(&g->tx, (const V6Acc *) (ext + g->off[j]), &out);
			else if (g->gen != NULL && g->gen->agg[j] != NULL)
				v6_gen_state_put(g->gen, j, (const V6Acc *) (ext + g->off[j]), &out);
		while (out.len % 8 != 0)
			appendStringInfoChar(&out, '\0');
		len = (uint32) out.len;
		memcpy(out.data, &len, sizeof(len));
		len = OWN_REC;
		memcpy(out.data + sizeof(uint32), &len, sizeof(len));
		memcpy(out.data + 8, &hash, sizeof(hash));
		memcpy(out.data + 16, &nulls, sizeof(nulls));
		put(arg, hash, (const uint8 *) out.data, (uint32) out.len);
	}
	pfree(out.data);
}

/* A record of own_flush merged into the table of m: its keys are interned in the codes of the partition, its text winners are in the arena of the partition */
static void
own_merge(const SiftRadixShape *sh, SiftRecMerge *m, const uint8 *rec)
{
	V6Set	   *set = SET_OF(sh);
	SiftV6Group *g = set->owner;
	const SiftV6Spec *spec = g->spec;
	uint64		nulls;
	const char *at = (const char *) rec + OWN_HDR + g->stride;
	const char *first;
	int64		key[SIFT_V6_MAX_KEYS];
	SiftV6StrIn str[SIFT_V6_MAX_KEYS];
	V6Acc	   *src;
	uint8	   *dst;

	memcpy(&nulls, rec + 16, sizeof(nulls));
	if (m->ext_copy == NULL)
		m->ext_copy = MemoryContextAllocAligned(m->ht->cxt, g->stride, alignof(SiftI128), 0);
	src = (V6Acc *) m->ext_copy;
	memcpy(src, rec + OWN_HDR, g->stride);
	for (int i = 0; i < set->nk; i++)
	{
		const int	k = set->kidx[i];

		str[k] = (SiftV6StrIn) {0};
		key[k] = 0;
		if ((nulls >> i) & 1)
			continue;
		if (is_generic(g, k))
			key[k] = sift_v6_genkeys_get(g->gkeys, k, &at);
		else if (sift_v6_class(g->spec->key[k].typid) == SV6C_TEXT)
		{
			uint32		n;

			memcpy(&n, at, sizeof(n));
			str[k] = (SiftV6StrIn) {at + sizeof(n), n};
			at += sizeof(n) + n;
		}
		else
		{
			memcpy(&key[k], at, sizeof(int64));
			at += sizeof(int64);
		}
	}
	first = at;
	for (int j = 0; j < spec->naggs; j++)
		if (tx_winner(&spec->agg[j], &src[j]))
		{
			SiftTextSlot *t = (SiftTextSlot *) &src[j];
			const int64 n = t->n;
			const int64 same = t->pad >> TSLOT_SAME_SHIFT;
			const char *bytes = same > 0 ? record_winner(g, rec + OWN_HDR, first, (int) same - 1) : at;

			t->pad = 0;
			sift_tslot_set(&g->tx, t, bytes, (uint32) t->v1);
			t->n = n;
			if (same == 0)
				at += (uint32) t->v1;
		}
	dst = sift_ht_ext(m->ht, v6g_slot_of(g, set, m->ht, key, str, nulls));
	for (int j = 0; j < spec->naggs; j++)
		if (spec->agg[j].kind == SV6_PCT_CONT)
			at = sift_v6_pct_take(&g->tx, (V6Acc *) (dst + g->off[j]), &src[j], at);
		else if (g->gen != NULL && g->gen->agg[j] != NULL)
			at = v6_gen_state_take(g->gen, j, (V6Acc *) (dst + g->off[j]), &src[j], at);
		else
			v6g_merge_acc(g, &spec->agg[j], (V6Acc *) (dst + g->off[j]), &src[j]);
}

/* The winners of the text MIN and MAX of the sets of no keys outlive the arena they are in: they are copied out and set again in the one that is empty */
static void
tx_reset(SiftV6Group *g)
{
	const SiftV6Spec *spec = g->spec;
	MemoryContext tmp = AllocSetContextCreate(g->cx, "sift agg v6 text winners", ALLOCSET_SMALL_SIZES);
	char	  **keep = MemoryContextAllocZero(tmp, Max(g->nsets * spec->naggs, 1) * sizeof(char *));

	for (int s = 0; s < g->nsets; s++)
		for (int j = 0; j < spec->naggs && g->set[s].nk == 0; j++)
		{
			const V6Acc *acc = &g->set[s].plain[j];

			if (tx_winner(&spec->agg[j], acc))
				keep[s * spec->naggs + j] = memcpy(MemoryContextAlloc(tmp, (uint32) acc->v1), g->tx.buf + acc->v0, (uint32) acc->v1);
		}
	if (g->tx.buf != NULL)
		pfree(g->tx.buf);
	g->tx.buf = NULL;
	g->tx.cap = g->tx.used = 0;
	for (int s = 0; s < g->nsets; s++)
		for (int j = 0; j < spec->naggs && g->set[s].nk == 0; j++)
			if (keep[s * spec->naggs + j] != NULL)
			{
				SiftTextSlot *t = (SiftTextSlot *) &g->set[s].plain[j];
				const int64 n = t->n;

				t->pad = 0;
				sift_tslot_set(&g->tx, t, keep[s * spec->naggs + j], (uint32) t->v1);
				t->n = n;
			}
	MemoryContextDelete(tmp);
}

/* The values the groups of the tables point to are given back, when no table of the state holds a code or an offset into them (a compact table holds codes, a locked one does not spill) */
static void
values_reset(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].cmp != NULL || (g->set[s].nk > 0 && g->set[s].ht.count > 0))
			return;
	if (g->gkeys != NULL)
		sift_v6_genkeys_reset(g->gkeys);
	if (g->gen != NULL)
		v6_gen_release(g->gen);
	tx_reset(g);
}

static void
own_begin(const SiftRadixShape *sh)
{
	values_reset(SET_OF(sh)->owner);
}

static uint64
own_held(const SiftRadixShape *sh)
{
	const SiftV6Group *g = SET_OF(sh)->owner;

	return g->tx.cap + (g->gkeys != NULL ? sift_v6_genkeys_bytes(g->gkeys) : 0) + (g->gen != NULL ? v6_gen_bytes(g->gen) : 0);
}

/* every group is in a table that spills */
static bool
all_keyed(const SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].nk == 0 || g->set[s].ht.lock_at != 0)
			return false;
	return true;
}

static bool
pct_of(const SiftV6Group *g)
{
	for (int j = 0; j < g->spec->naggs; j++)
		if (g->spec->agg[j].kind == SV6_PCT_CONT)
			return true;
	return false;
}

/* The states of the generic aggregates leave with the groups when every group is in a table that spills, and every aggregate combines */
static bool
gen_spills(const SiftV6Group *g)
{
	return all_keyed(g) && v6_gen_can_spill(g->gen);
}

void
v6g_spill_own(SiftV6Group *g)
{
	const SiftV6Spec *spec = g->spec;
	bool		values = g->gkeys != NULL || g->gen != NULL;

	for (int j = 0; j < spec->naggs; j++)
		values |= v6g_is_text_mm(&spec->agg[j]) || spec->agg[j].kind == SV6_PCT_CONT;
	if (!values || g->ndist > 0 || (g->gen != NULL && !gen_spills(g)) || (pct_of(g) && !all_keyed(g)))
		return;
	g->own_recs = true;
	for (int s = 0; s < g->nsets; s++)
	{
		g->set[s].shape.flush = own_flush;
		g->set[s].shape.merge_rec = own_merge;
		g->set[s].shape.part_begin = own_begin;
		g->set[s].shape.held = own_held;
	}
}

void
v6g_spill_sets(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
	{
		V6Set	   *set = &g->set[s];

		if (set->nk == 0 || set->ht.count == 0 || set->ht.lock_at != 0)
			continue;
		if (!v6g_dist_groups_in_pairs(g, set))
		{
			if (set->spill == NULL)
			{
				MemoryContext old = MemoryContextSwitchTo(g->cx);

				set->spill = sift_gspill_new(&set->shape);
				MemoryContextSwitchTo(old);
			}
			sift_gspill_put_ht(set->spill, &set->ht);
		}
		v6g_dist_gflat_flush(g, set);
		sift_ht_clear(&set->ht);
		sift_ht_shrink_to(&set->ht, SIFT_SPILL_LOCAL_CAP);
	}
	g->spilled = true;
}

void
v6g_spill_all(SiftV6Group *g)
{
	v6g_spill_sets(g);
	if (g->own_recs)
		values_reset(g);
}

/* The tables a spill leaves (SIFT_SPILL_LOCAL_CAP slots each) have room for more groups: spilling them now would write a few groups at a time, whatever the budget */
static bool
in_local_tables(const SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
	{
		const SiftSaltedHT *ht = &g->set[s].ht;

		if (g->set[s].nk > 0 && (ht->capacity > SIFT_SPILL_LOCAL_CAP || ht->count + SIFT_VEC_LEN > ht->grow_at))
			return false;
	}
	return true;
}

/* MEM[MEM-AGG-02]: SiftV6Group spill budget check (group_bytes of allocated memory against budget - dist_budget; a spill leaves SIFT_SPILL_LOCAL_CAP slots per set; was count * 65B and the whole table kept) */
void
v6g_spill_if_over(SiftV6Group *g)
{
	if (g->frozen || group_bytes(g) <= g->budget - g->dist_budget || (g->spilled && in_local_tables(g)))
		return;
	if (g->gen != NULL && !gen_spills(g))
	{
		if (!v6g_can_freeze(g))
			v6_gen_no_spill(g->gen);
		v6g_freeze(g);
		return;
	}
	v6g_spill_all(g);
}

/* After a batch of rows: a table of slots that is over its memory sends the rows of the groups it has no room for away, in place of the records of its groups */
void
v6g_rows_if_over(SiftV6Group *g)
{
	if (!g->frozen && group_bytes(g) > g->budget - g->dist_budget && v6g_rows_cheaper(g))
		v6g_freeze(g);
	else
		v6g_spill_if_over(g);
}

void
v6g_spill_open(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].nk > 0)
		{
			sift_ht_destroy(&g->set[s].ht);
			if (g->set[s].spill != NULL)
				g->set[s].it = sift_gspill_iter_begin(g->set[s].spill, g->budget / 4 * 3, true);
		}
}

const SiftSaltedHT *
v6g_spill_next(V6Set *set)
{
	set->emit_at = 0;
	set->out = set->it != NULL ? sift_gspill_iter_next(set->it) : NULL;
	return set->out;
}
