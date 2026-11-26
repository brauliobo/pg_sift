/*
 * agg/group/compact.c - the compact group table of SiftAggV6 (agg/group/compact.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "agg/kern/kern.h"
#include "agg/datum.h"
#include "agg/group/cmp.h"

#define CMP_AHEAD		16
#define CMP_FIRST_CAP	1024
#define CMP_ROWS_MAX	(UINT64CONST(1) << 31)	/* the rows at which a sum of 32-bit values, or a count of 32 bits, may pass its width */
#define CMP_MAX_KW		4

static void compact_flush(SiftV6Group *g, V6Set *set, bool spill);

static uint8
int_width(Oid t)
{
	return t == BOOLOID ? 1 : t == INT2OID ? 2 : (t == INT4OID || t == DATEOID) ? 4 : (t == INT8OID || t == TIMESTAMPOID || t == TIMESTAMPTZOID) ? 8 : 0;
}

static bool
agg_fits(const SiftV6Agg *a)
{
	const Oid	t = a->arg.typid;

	if (a->fclause != 0 || a->nxcols != 0)
		return false;
	switch (a->kind)
	{
		case SV6_COUNT_STAR:
			return true;
		case SV6_COUNT_DISTINCT:
			return sift_v6_class(t) != SV6C_F64 && sift_v6_class(t) != SV6C_TEXT && t != NUMERICOID;
		case SV6_COUNT:
			return t == BOOLOID || t == INT2OID || t == INT4OID || t == INT8OID;
		case SV6_SUM:
		case SV6_AVG:
			return t == INT2OID || t == INT4OID;
		default:
			return false;
	}
}

/* The numbers of the strings of the codes of the dictionary of a batch: kept while the batches are of the same part, else made again */
static void
codes_of_part(SiftCompact *c, const SiftV6Group *g, const SiftKeyDict *kd)
{
	if (kd->gen != 0 && kd->gen == c->map_gen && kd->d.n == c->map_n)
		return;
	if (kd->d.n > c->code_cap)
	{
		c->code_cap = Max(Max(1024, kd->d.n), c->code_cap * 2);
		if (c->code_id != NULL)
			pfree(c->code_id);
		if (c->code_ent != NULL)
			pfree(c->code_ent);
		c->code_id = c->strm ? NULL : MemoryContextAlloc(g->cx, (Size) c->code_cap * sizeof(uint32));
		c->code_ent = c->strm ? MemoryContextAlloc(g->cx, (Size) c->code_cap * sizeof(uint64)) : NULL;
	}
	if (c->strm)
		memset(c->code_ent, 0, (Size) kd->d.n * sizeof(uint64));
	else
		memset(c->code_id, 0xFF, (Size) kd->d.n * sizeof(uint32));
	c->map_gen = kd->gen;
	c->map_n = kd->d.n;
}

/* ---- the table ---- */

static uint64
cap_for(const SiftV6Group *g, int sw)
{
	const uint64 slot_bytes = (uint64) sw * sizeof(uint64);
	const uint64 room = g->budget / 4 / slot_bytes;
	const uint64 est = g->spec->groups_est;
	uint64		cap = CMP_FIRST_CAP;

	while (est >= 4 * SIFT_VEC_LEN && cap * 5 < Min(est, room) * 8)
		cap *= 2;
	return cap;
}

/* The layout of the key in the table: one integer, or the packed key of the set, or a word for each key column (the integers as they are, the text as its number) */
static bool
key_plan(const SiftV6Spec *spec, const V6Set *set, SiftCompact *c)
{
	c->strk = -1;
	if (set->packed)
	{
		c->pack = set->pack;
		c->kw = 2;
		return true;
	}
	if (!set->wide)
	{
		c->kw = 1;
		return int_width(spec->key[set->kidx[0]].typid) != 0;
	}
	if (set->nk == 1)
	{
		c->kw = 1;
		c->strk = 0;
		return true;
	}
	if (set->nk > CMP_MAX_KW)
		return false;
	for (int i = 0; i < set->nk; i++)
	{
		const Oid	t = spec->key[set->kidx[i]].typid;

		if (sift_v6_class(t) == SV6C_TEXT && c->strk < 0)
			c->strk = i;
		else if (int_width(t) == 0 && t != NUMERICOID)
			return false;
	}
	c->kw = set->nk;
	return c->strk >= 0;
}

/* Whether the set has a compact table, whatever the state of the scan: what the table would be, in c */
static bool
compact_plan(const SiftV6Group *g, const V6Set *set, SiftCompact *c)
{
	const SiftV6Spec *spec = g->spec;

	if (spec->nsets != 0 || g->nfilters != 0 || set->nk == 0 || set->ht.lock_at != 0 || g->gkeys != NULL)
		return false;
	for (int j = 0; j < spec->naggs; j++)
	{
		if (!agg_fits(&spec->agg[j]))
			return false;
		c->nsum += spec->agg[j].kind == SV6_SUM || spec->agg[j].kind == SV6_AVG;
	}
	c->has_gid = g->ndist > 0;
	if (!key_plan(spec, set, c) || !cmp_layout(c, set->packed))
		return false;
	return (uint64) spec->groups_est * c->sw * sizeof(uint64) * 12 / 5 <= g->budget - g->dist_budget;
}

bool
v6g_compact_eligible(const SiftV6Group *g, const V6Set *set)
{
	SiftCompact c = {0};

	return compact_plan(g, set, &c);
}

static SiftCompact *
compact_new(const SiftV6Group *g, const V6Set *set)
{
	const SiftV6Spec *spec = g->spec;
	SiftCompact *c;

	if (g->dist_put != NULL || g->spilled || set->ht.count != 0 || set->spill != NULL)
		return NULL;
	c = MemoryContextAllocZero(g->cx, sizeof(SiftCompact));
	c->cx = g->cx;
	if (!compact_plan(g, set, c))
	{
		pfree(c);
		return NULL;
	}
	c->word = MemoryContextAlloc(g->cx, Max(spec->naggs, 1) * sizeof(int8));
	for (int j = 0, w = 0; j < spec->naggs; j++)
		c->word[j] = spec->agg[j].kind == SV6_SUM || spec->agg[j].kind == SV6_AVG ? (int8) w++ : -1;
	if (c->strk >= 0 && c->kw >= 2)
		c->rowk = MemoryContextAlloc(g->cx, (Size) SIFT_VEC_LEN * c->kw * sizeof(uint64));
	cmp_init(c, cap_for(g, c->sw));
	return c;
}

/* whether the batch is one the table holds exactly: every argument of its aggregates has a value in every row, and no sum can pass 62 bits */
static bool
compact_takes(const SiftCompact *c, const SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *argvec)
{
	if ((c->nsum > 0 || c->cnt_hi) && c->rows + bs->n_raw >= CMP_ROWS_MAX)
		return false;
	for (int j = 0; j < g->spec->naggs; j++)
		if (g->spec->agg[j].kind != SV6_COUNT_STAR && g->spec->agg[j].kind != SV6_COUNT_DISTINCT && !argvec[j]->all_valid)
			return false;
	return true;
}

/* The number of the string of row r of a text key vector */
static uint32
text_id(SiftCompact *c, const SiftV6Group *g, const SiftVec *kv, uint16 r)
{
	if (kv->type == SVEC_DICT32)
	{
		const SiftGranuleDict *d = kv->dict;
		const uint32 code = ((const uint32 *) kv->data)[r];

		if (c->code_id[code] == CMP_NO_ID)
			c->code_id[code] = cmp_intern(c, d->bytes + d->off[code], d->off[code + 1] - d->off[code]);
		return c->code_id[code];
	}
	return cmp_intern(c, (const char *) sift_strview_bytes(&((const SiftStrView *) kv->data)[r]), ((const SiftStrView *) kv->data)[r].len);
}

/* The key words and hash of the rows of the batch (g->ikey, g->hash): false when a row has a NULL column and the table has no room for the flags */
static bool
key_rows(SiftCompact *c, SiftV6Group *g, const V6Set *set, SiftVec *const *keyvec, uint16 n)
{
	SiftVec    *kvs[SIFT_PACK_MAX_KEYS];

	if (c->strk >= 0 && keyvec[set->kidx[c->strk]]->type == SVEC_DICT32)
		codes_of_part(c, g, (const SiftKeyDict *) keyvec[set->kidx[c->strk]]->dict);
	if (c->kw == 1)
	{
		const SiftVec *kv = keyvec[set->kidx[0]];

		for (uint16 k = 0; k < n; k++)
		{
			const bool	valid = is_valid(kv, g->sel[k]);
			const int64 x = !valid ? 0 : c->strk == 0 ? (int64) text_id(c, g, kv, g->sel[k]) : int_at(kv, g->sel[k]);

			g->ikey[k].i64[0] = x;
			g->ikey[k].i64[1] = !valid;
			g->hash[k] = sift_aggk6_hash1((uint64) x);
		}
		return true;
	}
	for (int i = 0; i < set->nk; i++)
		kvs[i] = keyvec[set->kidx[i]];
	if (c->strk < 0)
	{
		uint64		nulls = 0;

		sift_pack_batch(&c->pack, kvs, g->sel, n, g->hash, g->ikey);
		for (uint16 k = 0; c->cnt_hi && k < n; k++)
			nulls |= g->ikey[k].u64[1] >> 32;
		return nulls == 0;
	}
	for (uint16 k = 0; k < n; k++)
	{
		uint64	   *w = c->rowk + (size_t) k * c->kw;
		uint64		nulls = 0;

		for (int i = 0; i < c->kw; i++)
		{
			const bool	valid = is_valid(kvs[i], g->sel[k]);

			nulls |= (uint64) !valid << i;
			w[i] = !valid ? 0 : i == c->strk ? text_id(c, g, kvs[i], g->sel[k]) : (uint64) int_at(kvs[i], g->sel[k]);
		}
		w[c->strk] |= nulls << 32;
		g->hash[k] = cmp_key_hash(c, w);
	}
	return true;
}

/* Whether the strings of a batch fit inside the compact table's budget (accounting for arena doubling) */
static bool
compact_str_fits(const SiftCompact *c, const SiftVec *kv, const uint16 *sel, uint16 n, uint64 budget)
{
	uint64		held,
				added = 0;

	if (budget == UINT64_MAX)
		return true;
	held = cmp_bytes(c);
	if (held >= budget)
		return false;
	for (uint16 k = 0; k < n; k++)
	{
		const uint16 r = sel[k];
		uint32		len;

		if (!is_valid(kv, r))
			continue;
		if (kv->type == SVEC_DICT32)
		{
			const SiftGranuleDict *d = kv->dict;
			const uint32 code = ((const uint32 *) kv->data)[r];

			len = d->off[code + 1] - d->off[code];
		}
		else
			len = ((const SiftStrView *) kv->data)[r].len;
		added += len;
		if (c->in.used + added > c->in.arena_cap)
		{
			const uint64 ncap = Max(Max((uint64) c->in.arena_cap * 2, (uint64) c->in.used + added), 4096);

			if (held - c->in.arena_cap + ncap > budget)
				return false;
		}
	}
	return true;
}

bool
v6g_compact_batch(SiftV6Group *g, V6Set *set, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	SiftCompact *c = set->cmp;
	const SiftVec *sum_vec[CMP_MAX_WORDS];
	uint64		budget;
	uint16		n;

	if (c == NULL)
	{
		if (set->cmp_off)
			return false;
		c = compact_new(g, set);
		set->cmp_off = c == NULL;
		set->cmp = c;
		if (c == NULL)
			return false;
	}
	n = sift_aggk6->mask_to_sel16(g->sel, bs->mask, bs->n_raw);
	if (!compact_takes(c, g, bs, argvec))
	{
		compact_flush(g, set, false);
		return false;
	}
	budget = g->budget == UINT64_MAX ? UINT64_MAX : g->budget - g->dist_budget;
	if (!cmp_reserve(c, n, budget) || (c->strk >= 0 && !compact_str_fits(c, keyvec[set->kidx[c->strk]], g->sel, n, budget)))
	{
		compact_flush(g, set, true);
		return false;
	}
	for (int j = 0; j < g->spec->naggs; j++)
		if (c->word[j] >= 0)
			sum_vec[c->word[j]] = argvec[j];
	if (c->strm)
	{
		if (keyvec[set->kidx[0]]->type == SVEC_DICT32)
			codes_of_part(c, g, (const SiftKeyDict *) keyvec[set->kidx[0]]->dict);
		sstr_batch(c, g, keyvec[set->kidx[0]], n);
		c->rows += n;
		return true;
	}
	if (!key_rows(c, g, set, keyvec, n))
	{
		compact_flush(g, set, false);
		return false;
	}
	for (uint16 k = 0; k < n; k++)
	{
		const uint64 *key = c->rowk != NULL ? c->rowk + (size_t) k * c->kw : g->ikey[k].u64;
		uint64	   *s;

		if (k + CMP_AHEAD < n)
			__builtin_prefetch(c->tab + (g->hash[k + CMP_AHEAD] & c->mask) * c->sw, 1);
		s = c->kw == 1 && g->ikey[k].i64[1] != 0 ? c->null_group : cmp_find(c, g->hash[k], key);
		if (c->has_gid)
		{
			if (cmp_count(c, s) == 0)
				s[c->gidw] = c->ngid++;
			g->gid[k] = (uint32) s[c->gidw];
		}
		cmp_count_up(c, s);
		for (int m = 0; m < c->nsum; m++)
			s[c->sum0 + m] += (uint64) int_at(sum_vec[m], g->sel[k]);
	}
	c->rows += n;
	if (c->has_gid)
	{
		g->gid_direct = true;
		for (int d = 0; d < g->ndist; d++)
			v6g_dist_group_rows(g, set, g->dagg[d], argvec, n);
		g->gid_direct = false;
		if (c->flush_after)
			compact_flush(g, set, false);
	}
	return true;
}

/* ---- the groups ---- */

uint64
v6g_compact_count(const V6Set *set, const uint64 *slot)
{
	return cmp_count(set->cmp, slot);
}

void
v6g_compact_each(const V6Set *set, V6CompactFn fn, void *arg)
{
	const SiftCompact *c = set->cmp;

	for (uint64 i = 0; i < c->cap; i++)
		if (cmp_count(c, c->tab + i * c->sw) != 0)
			fn(arg, c->tab + i * c->sw);
	if (c->kw == 1 && cmp_count(c, c->null_group) != 0)
		fn(arg, c->null_group);
}

void
v6g_compact_acc(const SiftV6Group *g, const V6Set *set, const uint64 *slot, V6Acc *acc)
{
	const SiftCompact *c = set->cmp;

	for (int j = 0; j < g->spec->naggs; j++)
	{
		acc[j] = (V6Acc) {.n = (int64) cmp_count(c, slot)};
		if (g->spec->agg[j].kind == SV6_COUNT_DISTINCT)
			acc[j] = (V6Acc) {.v0 = c->dv == NULL ? 0 : c->dv[(uint64) g->dslot[j] * c->ngid + slot[c->gidw]]};
		else if (c->word[j] >= 0)
			v6acc_set_i128(&acc[j], sift_i128_of_i64((int64) slot[c->sum0 + c->word[j]]));
	}
}

/* The value of key column i of the group of a slot: false when it is NULL */
static bool
slot_key(const SiftCompact *c, const uint64 *slot, int i, int64 *x)
{
	if (c->kw == 1)
	{
		*x = (int64) slot[0];
		return slot != c->null_group;
	}
	if (c->strk >= 0)
	{
		*x = i == c->strk ? (int64) (slot[i] & UINT32_MAX) : (int64) slot[i];
		return !((slot[c->strk] >> (32 + i)) & 1);
	}
	return sift_pack_get(&c->pack, &(SiftInlineKey16) {.u64 = {slot[0], c->cnt_hi ? (uint32) slot[1] : slot[1]}}, i, x);
}

/* The string of the group of a slot of a text key whose number is id (the slot is not the NULL group) */
static void
slot_text(const SiftCompact *c, const uint64 *slot, int64 id, const char **p, uint32 *len)
{
	if (c->strm)
		sstr_slot_str(c, slot, p, len);
	else
	{
		*p = c->in.arena + c->in.str[id].off;
		*len = c->in.str[id].len;
	}
}

void
v6g_compact_key_datums(const SiftV6Group *g, const V6Set *set, const uint64 *slot, Datum *values, bool *isnull, uint32 want)
{
	const SiftCompact *c = set->cmp;

	for (int i = 0; i < set->nk; i++)
	{
		const int	k = set->kidx[i];
		int64		x = 0;

		if (!((want >> k) & 1))
			continue;
		isnull[k] = !slot_key(c, slot, i, &x);
		if (isnull[k])
			values[k] = (Datum) 0;
		else if (i == c->strk)
		{
			const char *p;
			uint32		len;

			slot_text(c, slot, x, &p, &len);
			values[k] = PointerGetDatum(cstring_to_text_with_len(p, len));
		}
		else
			values[k] = v6g_int_datum(g->spec->key[k].typid, x);
	}
	if (g->gkeys != NULL)
		sift_v6_genkeys_resolve(g->gkeys, values, isnull, want);
}

void
v6g_compact_dist(V6Set *set, int dk, uint32 gid, uint64 count)
{
	SiftCompact *c = set->cmp;

	if (c->dv == NULL)
		c->dv = MemoryContextAllocZero(set->owner->cx, Max((uint64) c->ngid * set->owner->ndist, 1) * sizeof(uint64));
	c->dv[(uint64) dk * c->ngid + gid] += count;
}

void
v6g_compact_defer_flush(V6Set *set)
{
	set->cmp->flush_after = true;
}

/* The slots of the groups by number */
static void
by_gid_make(SiftCompact *c, const V6Set *set)
{
	if (c->by_n == c->ngid && c->by_gid != NULL)
		return;
	if (c->by_gid != NULL)
		pfree(c->by_gid);
	c->by_gid = MemoryContextAlloc(set->owner->cx, Max(c->ngid, 1) * sizeof(uint64 *));
	for (uint64 i = 0; i < c->cap; i++)
		if (cmp_count(c, c->tab + i * c->sw) != 0)
			c->by_gid[c->tab[i * c->sw + c->gidw]] = c->tab + i * c->sw;
	if (c->kw == 1 && cmp_count(c, c->null_group) != 0)
		c->by_gid[c->null_group[c->gidw]] = c->null_group;
	c->by_n = c->ngid;
}

static void
key_bytes(const SiftCompact *c, const V6Set *set, const uint64 *slot, StringInfo buf, uint64 *nulls)
{
	*nulls = 0;
	for (int i = 0; i < set->nk; i++)
	{
		int64		x;

		if (!slot_key(c, slot, i, &x))
			*nulls |= UINT64CONST(1) << i;
		else if (i == c->strk)
		{
			const char *p;
			uint32		len;

			slot_text(c, slot, x, &p, &len);
			appendBinaryStringInfo(buf, (const char *) &len, sizeof(uint32));
			appendBinaryStringInfo(buf, p, len);
		}
		else
			appendBinaryStringInfo(buf, (const char *) &x, sizeof(x));
	}
}

void
v6g_compact_rowkey(V6Set *set, uint32 gid, V6RowKey *rk)
{
	SiftCompact *c = set->cmp;
	const uint64 *slot;

	by_gid_make(c, set);
	slot = c->by_gid[gid];
	if (c->strk >= 0)
	{
		resetStringInfo(&set->owner->kbuf);
		key_bytes(c, set, slot, &set->owner->kbuf, &rk->nulls);
		rk->p = set->owner->kbuf.data;
		rk->len = set->owner->kbuf.len;
	}
	else if (c->kw == 1)
		*rk = (V6RowKey) {slot, sizeof(int64), slot == c->null_group};
	else
		*rk = (V6RowKey) {slot, sizeof(SiftInlineKey16), 0};
}

/* ---- the groups to the group table ---- */

/* The slot of a group in the group table, made with its key: an integer, a packed key, or the bytes of its columns */
static SiftAggSlot64 *
flush_upsert(SiftV6Group *g, V6Set *set, const uint64 *slot)
{
	const SiftCompact *c = set->cmp;
	SiftAggSlot64 *s;

	sift_ht_reserve(&set->ht, 1);
	if (c->strk < 0)
	{
		SiftInlineKey16 key;
		uint64		h;

		if (c->kw == 1)
		{
			key = (SiftInlineKey16) {.i64 = {slot == c->null_group ? 0 : (int64) slot[0], slot == c->null_group}};
			h = sift_aggk6_hash1((uint64) key.i64[0]);
		}
		else
		{
			key = (SiftInlineKey16) {.u64 = {slot[0], c->cnt_hi ? (uint32) slot[1] : slot[1]}};
			h = sift_pack_hash(&key);
		}
		sift_ht_upsert_batch(&set->ht, &h, &key, 1, &s);
		return s;
	}
	{
		uint64		nulls;
		bool		found;

		resetStringInfo(&g->kbuf);
		key_bytes(c, set, slot, &g->kbuf, &nulls);
		return sift_ht_upsert_wide(&set->ht, sift_aggk6_hash1(sift_aggk6_hash_bytes(g->kbuf.data, g->kbuf.len) ^ nulls), g->kbuf.data, g->kbuf.len, nulls, &found);
	}
}

typedef struct Flush
{
	V6Set	   *set;
	bool		spill;			/* the groups leave for the spill a run at a time, as the table is flushed because the memory is out */
	uint32		run;
	uint32		run_max;		/* the groups a quarter of the budget holds */
} Flush;

static void
flush_group(void *arg, const uint64 *slot)
{
	Flush	   *f = arg;
	V6Set	   *set = f->set;

	v6g_compact_acc(set->owner, set, slot, (V6Acc *) sift_ht_ext(&set->ht, flush_upsert(set->owner, set, slot)));
	if (f->spill && ++f->run == f->run_max)
	{
		f->run = 0;
		v6g_spill_sets(set->owner);
	}
}

static void
compact_flush(SiftV6Group *g, V6Set *set, bool spill)
{
	Flush		f = {set, spill && !set->cmp->has_gid, 0, 0};
	MemoryContext old;

	if (set->cmp == NULL)
		return;
	f.run_max = (uint32) Max(g->budget / 4 / (g->stride + sizeof(SiftAggSlot64) + 1), 1024);
	old = MemoryContextSwitchTo(g->cx);
	sift_ht_presize(&set->ht, (uint32) Min(set->cmp->count, f.spill ? f.run_max : UINT32_MAX));
	if (set->cmp->has_gid)
	{
		by_gid_make(set->cmp, set);
		for (uint32 gid = 0; gid < set->cmp->ngid; gid++)
			flush_group(&f, set->cmp->by_gid[gid]);
	}
	else
		v6g_compact_each(set, flush_group, &f);
	v6g_compact_free(set);
	set->cmp_off = true;
	MemoryContextSwitchTo(old);
}

void
v6g_compact_flush(SiftV6Group *g, V6Set *set)
{
	compact_flush(g, set, false);
}

uint64
v6g_compact_bytes(const V6Set *set)
{
	const SiftCompact *c = set->cmp;

	return c == NULL ? 0 : cmp_bytes(c);
}

void
v6g_compact_free(V6Set *set)
{
	SiftCompact *c = set->cmp;

	if (c == NULL)
		return;
	cmp_free_all(c);
	if (c->dv != NULL)
		pfree(c->dv);
	if (c->by_gid != NULL)
		pfree(c->by_gid);
	if (c->code_id != NULL)
		pfree(c->code_id);
	if (c->code_ent != NULL)
		pfree(c->code_ent);
	if (c->rowk != NULL)
		pfree(c->rowk);
	pfree(c->word);
	pfree(c);
	set->cmp = NULL;
}
