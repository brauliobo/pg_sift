/*
 * agg/v6/group.c - the groups of SiftAggV6: the keys and aggregates of a batch of up to 2048 rows folded into a table of groups, and the groups as the Datums of the plan.
 *
 * A set of keys with none: one set of accumulators folded by the vector kernels. A key of one column of 64 bits or less is the key of a slot of agg/group/ht.h; any other key is
 * the bytes of its columns (8 for a number, 4 and the string for a text) in the arena of a wide table. The state of an aggregate is a V6Acc in the state block of the
 * slot, zero until its first value (n == 0). A plain GROUP BY is one set of every key; GROUPING SETS, ROLLUP and CUBE are one set each, all folded from the same batches.
 */
#include "postgres.h"

#include <math.h>

#include "executor/executor.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/tuplestore.h"

#include "agg/gpu_v6.h"
#include "agg/group/ht.h"
#include "agg/group/spill.h"
#include "agg/group/state.h"
#include "agg/v6/group_int.h"
#include "agg/v6/group_nan.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_argext.h"
#include "core/memtrack.h"

extern PGDLLIMPORT bool sift_agg_exact_float;

/* sift.arg_max, sift.arg_min: n the rows with a cmp, v0 the order key of the best cmp, v1 its val, aux 1 when that val is NULL (v6_argext.h) */

static Datum
nan_datum(void)
{
	return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

static void
fold_row(SiftV6Group *g, const SiftV6Agg *a, V6Acc *acc, const SiftVec *v, const SiftVec *v2, uint16 r)
{
	if (a->kind == SV6_COUNT_STAR)
	{
		acc->n++;
		return;
	}
	if (a->kind == SV6_ARG_MAX || a->kind == SV6_ARG_MIN)
	{
		if (is_valid(v2, r))
			sift_argacc_offer(&acc->n, &acc->v0, &acc->v1, &acc->aux, a->kind == SV6_ARG_MAX ? SIFT_ARG_MAX : SIFT_ARG_MIN, sift_argext_cmp_key(v2, r), int_at(v, r), !is_valid(v, r));
		return;
	}
	if (a->kind == SV6_GROUPING)
		return;
	if (!is_valid(v, r))
	{
		acc->aux += sift_v6_agg_takes_nan(a) && v6acc_nan_row(v, r);
		return;
	}
	if (v6g_is_ext(a))
	{
		v6g_ext_fold_row(g, a, acc, v, r);
		return;
	}
	switch (a->kind)
	{
		case SV6_COUNT:
			acc->n++;
			break;
		case SV6_SUM:
		case SV6_AVG:
			if (v->type == SVEC_F64)
				v6acc_set_f64(acc, v6acc_f64(acc) + ((const double *) v->data)[r]);
			else
			{
				SiftI128	s = v6acc_i128(acc);

				sift_i128_add(&s, sift_i128_of_i64(int_at(v, r)));
				v6acc_set_i128(acc, s);
			}
			acc->n++;
			break;
		case SV6_MIN:
		case SV6_MAX:
			{
				const bool	mn = a->kind == SV6_MIN;

				if (v->type == SVEC_STRVIEW)
				{
					const SiftStrView *sv = &((const SiftStrView *) v->data)[r];

					sift_strmm_offer(g->slab, (SiftStrMM *) acc, mn, sift_strview_bytes(sv), sv->len);
					break;
				}
				if (v->type == SVEC_F64)
				{
					double		x = ((const double *) v->data)[r];

					if (acc->n == 0 || (mn ? v6acc_f8_cmp(x, v6acc_f64(acc)) < 0 : v6acc_f8_cmp(x, v6acc_f64(acc)) > 0))
						v6acc_set_f64(acc, x);
				}
				else
				{
					int64		x = int_at(v, r);

					if (acc->n == 0 || (mn ? x < (int64) acc->v0 : x > (int64) acc->v0))
						acc->v0 = (uint64) x;
				}
				acc->n++;
			}
			break;
		default:
			break;
	}
}

static void
plain_batch(SiftV6Group *g, V6Set *set, const SiftBatchSel *bs, SiftVec *const *argvec)
{
	for (int j = 0; j < g->spec->naggs; j++)
	{
		const SiftV6Agg *a = &g->spec->agg[j];
		const SiftVec *v = argvec[j];
		V6Acc	   *acc = &set->plain[j];
		uint64		m[SIFT_VEC_WORDS];
		uint16		cnt;
		const uint64 *mk = v6g_filter_take(g, a, bs, &cnt);

		if (a->kind == SV6_COUNT_STAR)
		{
			acc->n += cnt;
			continue;
		}
		if (a->kind == SV6_GROUPING)
			continue;
		if (a->kind == SV6_ARG_MAX || a->kind == SV6_ARG_MIN)
		{
			for (uint16 r = 0; r < bs->n_raw; r++)
				if ((mk[r >> 6] >> (r & 63)) & 1)
					fold_row(g, a, acc, v, argvec[g->spec->naggs + j], r);
			continue;
		}
		if (v->nan != NULL && sift_v6_agg_takes_nan(a))
			acc->aux += v6acc_nan_rows(v, mk, bs->n_raw);
		if (a->kind == SV6_COUNT_DISTINCT)
		{
			v6g_dist_plain(g, set, j, bs->n_raw, mk, argvec);
			continue;
		}
		if (v6g_is_ext(a))
		{
			v6g_ext_plain(g, a, acc, v, mk, bs->n_raw);
			continue;
		}
		if (v->type == SVEC_STRVIEW)
		{
			for (uint16 r = 0; r < bs->n_raw; r++)
				if (((mk[r >> 6] >> (r & 63)) & 1) && is_valid(v, r))
					fold_row(g, a, acc, v, NULL, r);
			continue;
		}
		if (!v->all_valid)
		{
			cnt = sift_aggk6->mask_and(m, mk, v->valid, bs->n_raw);
			mk = m;
		}
		if (cnt == 0)
			continue;
		switch (a->kind)
		{
			case SV6_COUNT:
				acc->n += cnt;
				break;
			case SV6_SUM:
			case SV6_AVG:
				if (v->type == SVEC_F64 && sift_agg_exact_float)
				{
					for (uint16 r = 0; r < bs->n_raw; r++)
						if ((mk[r >> 6] >> (r & 63)) & 1)
							v6acc_set_f64(acc, v6acc_f64(acc) + ((const double *) v->data)[r]);
				}
				else if (v->type == SVEC_F64)
					v6acc_set_f64(acc, sift_aggk6->sum_f64_fast(v->data, mk, bs->n_raw, v6acc_f64(acc)));
				else if (v->type == SVEC_I32)
				{
					SiftI128	s = v6acc_i128(acc);

					sift_i128_add(&s, sift_i128_of_i64(sift_aggk6->sum_i32(v->data, mk, bs->n_raw)));
					v6acc_set_i128(acc, s);
				}
				else
					v6acc_set_i128(acc, sift_aggk6->sum_i64_to_i128(v->data, mk, bs->n_raw, v6acc_i128(acc)));
				acc->n += cnt;
				break;
			case SV6_MIN:
			case SV6_MAX:
				{
					const bool	mn = a->kind == SV6_MIN;
					bool		got;
					int64		lo,
								hi;
					int32		lo32,
								hi32;
					double		flo,
								fhi;

					if (v->type == SVEC_F64)
					{
						got = sift_aggk6->minmax_f64(&flo, &fhi, v->data, mk, bs->n_raw);
						if (got && (acc->n == 0 || (mn ? v6acc_f8_cmp(flo, v6acc_f64(acc)) < 0 : v6acc_f8_cmp(fhi, v6acc_f64(acc)) > 0)))
							v6acc_set_f64(acc, mn ? flo : fhi);
					}
					else
					{
						if (v->type == SVEC_I32)
						{
							got = sift_aggk6->minmax_i32(&lo32, &hi32, v->data, mk, bs->n_raw);
							lo = lo32;
							hi = hi32;
						}
						else
							got = sift_aggk6->minmax_i64(&lo, &hi, v->data, mk, bs->n_raw);
						if (got && (acc->n == 0 || (mn ? lo < (int64) acc->v0 : hi > (int64) acc->v0)))
							acc->v0 = (uint64) (mn ? lo : hi);
					}
					acc->n += cnt;
				}
				break;
			default:
				break;
		}
	}
}

uint64
v6g_group_hash(const V6Set *set, const void *key, uint32 klen, uint64 nulls)
{
	int64		x;

	if (set->wide)
		return sift_aggk6_hash1(sift_aggk6_hash_bytes(key, klen) ^ nulls);
	memcpy(&x, key, sizeof(x));
	return sift_aggk6_hash1((uint64) x);
}

static void
append_key_str(SiftV6Group *g, const char *p, uint32 len)
{
	appendBinaryStringInfo(&g->kbuf, (const char *) &len, sizeof(len));
	appendBinaryStringInfo(&g->kbuf, p, len);
}

/* The group of each distinct code of a dictionary, found once for the batch and not for each row: set->ht holds a slot of every group the batch makes, with the bytes sift_wide_batch_build gives a string */
static SiftAggSlot64 *
code_slot(SiftV6Group *g, V6Set *set, const SiftGranuleDict *d, uint32 code)
{
	const bool	null = code == d->n;
	bool		found;

	resetStringInfo(&g->kbuf);
	if (!null)
		append_key_str(g, d->bytes + d->off[code], d->off[code + 1] - d->off[code]);
	return sift_ht_upsert_wide(&set->ht, sift_aggk6_hash1(sift_aggk6_hash_bytes(g->kbuf.data, g->kbuf.len) ^ null), g->kbuf.data, g->kbuf.len, null, &found);
}

static void
code_slots(SiftV6Group *g, V6Set *set, const SiftVec *kv, uint16 n)
{
	const SiftGranuleDict *d = kv->dict;
	const uint32 *code = kv->data;

	if (g->ncode_slots < d->n + 1)
	{
		g->code_slots = g->code_slots ? repalloc(g->code_slots, (d->n + 1) * sizeof(*g->code_slots)) : palloc((d->n + 1) * sizeof(*g->code_slots));
		g->ncode_slots = d->n + 1;
	}
	memset(g->code_slots, 0, (d->n + 1) * sizeof(*g->code_slots));
	for (uint16 k = 0; k < n; k++)
	{
		const uint32 c = is_valid(kv, g->sel[k]) ? code[g->sel[k]] : d->n;

		if (g->code_slots[c] == NULL)
			g->code_slots[c] = code_slot(g, set, d, c);
		g->slots[k] = g->code_slots[c];
	}
}

static void
grouped_batch(SiftV6Group *g, V6Set *set, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	const uint16 n = sift_aggk6->mask_to_sel16(g->sel, bs->mask, bs->n_raw);
	const bool	coded = set->wide && set->nk == 1 && g->ndist == 0 && keyvec[set->kidx[0]]->type == SVEC_DICT32;

	g->frows = g->nfilters > 0 ? sift_fmasks_rows(g->fmasks) : NULL;
	sift_ht_reserve(&set->ht, n);
	if (coded)
		code_slots(g, set, keyvec[set->kidx[0]], n);
	else if (set->packed)
	{
		SiftVec    *kvs[SIFT_PACK_MAX_KEYS];

		for (int i = 0; i < set->nk; i++)
			kvs[i] = keyvec[set->kidx[i]];
		sift_pack_batch(&set->pack, kvs, g->sel, n, g->hash, g->ikey);
		sift_ht_upsert_batch(&set->ht, g->hash, g->ikey, n, g->slots);
		for (int d = 0; d < g->ndist; d++)
			for (uint16 k = 0; k < n; k++)
				if (v6g_row_takes(g, &g->spec->agg[g->dagg[d]], g->sel[k]))
					v6g_dist_group_row(g, set, g->dagg[d], argvec, g->sel[k], &g->ikey[k], sizeof(SiftInlineKey16), 0);
	}
	else if (set->wide)
	{
		sift_wide_batch_build(&g->wb, set->kidx, set->nk, keyvec, g->sel, n);
		sift_wide_batch_upsert(&set->ht, &g->wb, n, g->slots);
		for (int d = 0; d < g->ndist; d++)
			for (uint16 k = 0; k < n; k++)
				if (v6g_row_takes(g, &g->spec->agg[g->dagg[d]], g->sel[k]))
					v6g_dist_group_row(g, set, g->dagg[d], argvec, g->sel[k], g->wb.buf + g->wb.off[k], g->wb.len[k], g->wb.nulls[k]);
	}
	else
	{
		const SiftVec *kv = keyvec[set->kidx[0]];

		for (uint16 k = 0; k < n; k++)
		{
			const uint16 r = g->sel[k];
			const bool	valid = is_valid(kv, r);
			const int64 x = valid ? int_at(kv, r) : 0;

			g->ikey[k].i64[0] = x;
			g->ikey[k].i64[1] = !valid;
			g->hash[k] = sift_aggk6_hash1((uint64) x);
		}
		sift_ht_upsert_batch(&set->ht, g->hash, g->ikey, n, g->slots);
		for (int d = 0; d < g->ndist; d++)
			for (uint16 k = 0; k < n; k++)
				if (v6g_row_takes(g, &g->spec->agg[g->dagg[d]], g->sel[k]))
					v6g_dist_group_row(g, set, g->dagg[d], argvec, g->sel[k], &g->ikey[k].i64[0], sizeof(int64), (uint64) g->ikey[k].i64[1]);
	}
	for (int j = 0; j < g->spec->naggs; j++)
	{
		if (g->spec->agg[j].kind == SV6_COUNT_STAR && g->spec->agg[j].fclause == 0)
		{
			for (uint16 k = 0; k < n; k++)
				((V6Acc *) (sift_ht_ext(&set->ht, g->slots[k]) + g->off[j]))->n++;
			continue;
		}
		for (uint16 k = 0; k < n; k++)
			if (v6g_row_takes(g, &g->spec->agg[j], g->sel[k]))
				fold_row(g, &g->spec->agg[j], (V6Acc *) (sift_ht_ext(&set->ht, g->slots[k]) + g->off[j]), argvec[j], argvec[g->spec->naggs + j], g->sel[k]);
	}
}

bool
sift_v6_spec_count_only(const SiftV6Spec *spec)
{
	bool		only = spec->nkeys == 0 && spec->nsets == 0 && spec->nbins == 0 && spec->nfilters == 0;

	for (int j = 0; only && j < spec->naggs; j++)
		only = spec->agg[j].kind == SV6_COUNT_STAR && spec->agg[j].fclause == 0;
	return only;
}

void
sift_v6_group_add_rows(SiftV6Group *g, uint64 n)
{
	for (int j = 0; j < g->spec->naggs; j++)
		g->set[0].plain[j].n += n;
}

void
sift_v6_group_update(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	MemoryContext old;

	if (bs->n_active == 0)
		return;
	old = MemoryContextSwitchTo(g->cx);
	v6g_filter_eval(g, bs, argvec);
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].nk == 0)
			plain_batch(g, &g->set[s], bs, argvec);
		else
			grouped_batch(g, &g->set[s], bs, keyvec, argvec);
	v6g_spill_if_over(g);
	MemoryContextSwitchTo(old);
}

static void
merge_acc(SiftV6Group *g, const SiftV6Agg *a, V6Acc *d, const V6Acc *s)
{
	if (v6g_is_ext(a))
		v6g_ext_merge(g, a, d, s);
	else if ((a->kind == SV6_MIN || a->kind == SV6_MAX) && sift_v6_class(a->arg.typid) == SV6C_TEXT)
	{
		if (s->n != 0)
			sift_strmm_merge((SiftStrMM *) d, (const SiftStrMM *) s, a->kind == SV6_MIN);
	}
	else
		sift_v6_acc_combine(v6g_acc_op(a), d, s);
}

static void
shape_init(const SiftRadixShape *sh, SiftAggSlot64 *slot, uint8 *ext)
{
}

static void
shape_merge(const SiftRadixShape *sh, SiftAggSlot64 *dst, uint8 *dst_ext, const SiftAggSlot64 *src, const uint8 *src_ext)
{
	SiftV6Group *g = (SiftV6Group *) sh->ctx;
	const SiftV6Spec *spec = g->spec;

	for (int j = 0; j < spec->naggs; j++)
		merge_acc(g, &spec->agg[j], (V6Acc *) dst_ext + j, (const V6Acc *) src_ext + j);
}

/* the keys of the set as fixed-width integers of at most 12 bytes in all: one inline key (v6_group_fast.h) */
static bool
pack_plan(const SiftV6Spec *spec, V6Set *set)
{
	uint8		width[SIFT_V6_MAX_KEYS];
	bool		sign[SIFT_V6_MAX_KEYS];

	for (int i = 0; i < set->nk; i++)
	{
		const Oid	t = spec->key[set->kidx[i]].typid;

		width[i] = t == BOOLOID ? 1 : t == INT2OID ? 2 : (t == INT4OID || t == DATEOID) ? 4 : (t == INT8OID || t == TIMESTAMPOID || t == TIMESTAMPTZOID) ? 8 : 0;
		sign[i] = t != BOOLOID;
	}
	return set->nk <= SIFT_PACK_MAX_KEYS && sift_pack_plan(width, sign, set->nk, &set->pack);
}

static void
set_init(SiftV6Group *g, V6Set *set, uint32 mask)
{
	const SiftV6Spec *spec = g->spec;

	set->owner = g;
	set->mask = mask;
	for (int i = 0; i < spec->nkeys; i++)
		if ((mask >> i) & 1)
			set->kidx[set->nk++] = i;
	set->packed = pack_plan(spec, set);
	set->wide = !set->packed && (set->nk > 1 || (set->nk == 1 && sift_v6_class(spec->key[set->kidx[0]].typid) == SV6C_TEXT));
	set->plain_row = true;
	if (set->nk == 0)
		set->plain = palloc0(Max(spec->naggs, 1) * sizeof(V6Acc));
	else
		sift_ht_init_ex(&set->ht, 1024, set->wide, g->stride, g->dist_off);
	set->shape = (SiftRadixShape) {.wide_key = set->wide,.ext_stride = g->stride,.ext_dist_off = g->dist_off,.ctx = g,.init = shape_init,.merge = shape_merge};
}

/* The state of a participant of a parallel run: its rows of a COUNT(DISTINCT) go to put, and its table holds what a participant may hold before it hands its groups over, whatever work_mem says */
void
sift_v6_group_scan_sink(SiftV6Group *g, SiftV6DistPut put, void *arg)
{
	g->dist_put = put;
	g->dist_arg = arg;
	g->budget = UINT64_MAX;
	g->dist_budget = 0;
}

/* The state of a partition that did not spill, emptied for the records of the next partition: its tables and buffers stay */
void
sift_v6_group_recycle(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
	{
		V6Set	   *set = &g->set[s];

		if (set->nk == 0)
			memset(set->plain, 0, Max(g->spec->naggs, 1) * sizeof(V6Acc));
		else
			sift_ht_clear(&set->ht);
		set->plain_row = false;
		set->emitted = false;
	}
	v6g_dist_recycle(g);
}

/* the group tables for the groups the planner expects, when that many (a quarter of the memory the groups may take at most) */
static void
presize(SiftV6Group *g)
{
	const uint64 per_group = sizeof(SiftAggSlot64) + 1 + g->stride;
	const uint64 room = (g->budget - g->dist_budget) / 4 / per_group;
	const uint64 est = g->spec->groups_est / g->nsets;

	if (est < 4 * SIFT_VEC_LEN)
		return;
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].nk > 0)
			sift_ht_presize(&g->set[s].ht, (uint32) Min(est, room));
}

SiftV6Group *
sift_v6_group_new(const SiftV6Spec *spec, MemoryContext cx, TupleDesc desc)
{
	SiftV6Group *g = MemoryContextAllocZero(cx, sizeof(SiftV6Group));
	MemoryContext old = MemoryContextSwitchTo(cx);
	const uint32 all = spec->nkeys >= 32 ? UINT32_MAX : (((uint32) 1) << spec->nkeys) - 1;

	g->spec = spec;
	g->cx = cx;
	g->desc = desc;
	g->budget = get_hash_memory_limit();
	g->tx.cx = cx;
	g->dist_off = SIFT_HT_NO_EXT;
	v6g_dist_init(g);
	v6g_filter_init(g);
	g->off = palloc(Max(spec->naggs, 1) * sizeof(uint32));
	for (int j = 0; j < spec->naggs; j++)
		g->off[j] = j * sizeof(V6Acc);
	g->stride = Max(spec->naggs, 1) * sizeof(V6Acc);
	g->nsets = Max(spec->nsets, 1);
	g->set = palloc0(g->nsets * sizeof(V6Set));
	initStringInfo(&g->kbuf);
	for (int s = 0; s < g->nsets; s++)
		set_init(g, &g->set[s], spec->nsets == 0 ? all : spec->set_mask[s]);
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].wide && g->wb.buf == NULL)
			sift_wide_batch_init(&g->wb, cx, SIFT_VEC_LEN);
	for (int j = 0; j < spec->naggs; j++)
		if ((spec->agg[j].kind == SV6_MIN || spec->agg[j].kind == SV6_MAX) && sift_v6_class(spec->agg[j].arg.typid) == SV6C_TEXT && g->slab == NULL)
			g->slab = sift_strslab_new(cx);
	presize(g);
	MemoryContextSwitchTo(old);
	return g;
}

static V6Acc
acc_of_in(const SiftV6AccIn *a)
{
	return (V6Acc) {.n = a->n,.v0 = a->v0,.v1 = a->v1};
}

void
sift_v6_group_fold_rec(SiftV6Group *g, const int64 *key, uint64 key_nulls, const SiftV6AccIn *acc)
{
	V6Set	   *set = &g->set[0];
	MemoryContext old = MemoryContextSwitchTo(g->cx);
	uint8	   *state;

	Assert(g->spec->nsets == 0);
	if (set->nk == 0)
		state = (uint8 *) set->plain;
	else
	{
		SiftAggSlot64 *slot;

		sift_ht_reserve(&set->ht, 1);
		if (set->packed)
		{
			int64		x[SIFT_PACK_MAX_KEYS];

			for (int i = 0; i < set->nk; i++)
				x[i] = key[set->kidx[i]];
			g->hash[0] = sift_pack_key(&set->pack, x, key_nulls, &g->ikey[0]);
			sift_ht_upsert_batch(&set->ht, g->hash, g->ikey, 1, g->slots);
			slot = g->slots[0];
		}
		else if (set->wide)
		{
			bool		found;

			resetStringInfo(&g->kbuf);
			for (int i = 0; i < set->nk; i++)
				if (!((key_nulls >> i) & 1))
					appendBinaryStringInfo(&g->kbuf, (const char *) &key[set->kidx[i]], sizeof(int64));
			slot = sift_ht_upsert_wide(&set->ht, sift_aggk6_hash1(sift_aggk6_hash_bytes(g->kbuf.data, g->kbuf.len) ^ key_nulls), g->kbuf.data, g->kbuf.len,
									   key_nulls, &found);
		}
		else
		{
			const int64 x = (key_nulls & 1) ? 0 : key[set->kidx[0]];

			g->ikey[0].i64[0] = x;
			g->ikey[0].i64[1] = key_nulls & 1;
			g->hash[0] = sift_aggk6_hash1((uint64) x);
			sift_ht_upsert_batch(&set->ht, g->hash, g->ikey, 1, g->slots);
			slot = g->slots[0];
		}
		state = sift_ht_ext(&set->ht, slot);
	}
	for (int j = 0; j < g->spec->naggs; j++)
	{
		const V6Acc in = acc_of_in(&acc[j]);

		merge_acc(g, &g->spec->agg[j], (V6Acc *) (state + g->off[j]), &in);
	}
	v6g_spill_if_over(g);
	MemoryContextSwitchTo(old);
}

static Datum
scaled_numeric_text(SiftI128 v, int scale)
{
	char		rev[48],
				out[64];
	int			n = 0,
				o = 0;
	unsigned __int128 u = ((unsigned __int128) (uint64) v.hi << 64) | v.lo;
	const bool	neg = v.hi < 0;

	if (neg)
		u = -u;
	do
	{
		rev[n++] = '0' + (int) (u % 10);
		u /= 10;
	} while (u != 0 || n <= scale);
	if (neg)
		out[o++] = '-';
	for (int i = n - 1; i >= 0; i--)
	{
		out[o++] = rev[i];
		if (i == scale && scale > 0)
			out[o++] = '.';
	}
	out[o] = '\0';
	return DirectFunctionCall3(numeric_in, CStringGetDatum(out), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

/* The digits of PostgreSQL's numeric straight from the 128-bit integer (v6_acc.h), through its wire format: no decimal string is made and parsed; only a value too wide for them takes the text */
Datum
sift_v6_scaled_numeric(SiftI128 v, int scale)
{
	int16		digits[V6_NUMERIC_MAX_DIGITS];
	StringInfoData wire;
	bool		neg;
	int			weight,
				nd = sift_v6_i128_digits(v, scale, &neg, digits, &weight);

	if (nd < 0)
		return scaled_numeric_text(v, scale);
	initStringInfo(&wire);
	pq_sendint16(&wire, nd);
	pq_sendint16(&wire, weight);
	pq_sendint16(&wire, neg ? V6_NUMERIC_SIGN_NEG : 0);
	pq_sendint16(&wire, scale);
	for (int i = 0; i < nd; i++)
		pq_sendint16(&wire, digits[i]);
	return DirectFunctionCall3(numeric_recv, PointerGetDatum(&wire), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

Datum
sift_v6_i128_numeric(SiftI128 v)
{
	return sift_v6_scaled_numeric(v, 0);
}

static Datum
int_datum(Oid typid, int64 x)
{
	switch (typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) x);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) x);
		case BOOLOID:
			return BoolGetDatum(x != 0);
		case NUMERICOID:
			return NumericGetDatum(int64_to_numeric(x));
		default:
			return Int64GetDatum(x);
	}
}

static Datum
grouping_datum(const SiftV6Func *f, uint32 mask)
{
	int32		v = 0;

	for (int i = 0; i < f->nargs; i++)
		v = (v << 1) | !((mask >> f->key[i]) & 1);
	return Int32GetDatum(v);
}

Datum
v6g_agg_datum(const SiftV6Group *g, const SiftV6Agg *a, const V6Acc *acc, uint32 mask, bool *isnull)
{
	const SiftV6Spec *spec = g->spec;

	*isnull = false;
	if (v6g_is_ext(a))
		return v6g_ext_datum(g, a, acc, isnull);
	switch (a->kind)
	{
		case SV6_GROUPING:
			return grouping_datum(&spec->func[a->arg.attno], mask);
		case SV6_COUNT_STAR:
			return Int64GetDatum(acc->n);
		case SV6_COUNT:
			return Int64GetDatum(v6acc_count(a, acc));
		case SV6_ARG_MAX:
		case SV6_ARG_MIN:
			*isnull = acc->n == 0 || acc->aux != 0;
			return *isnull ? (Datum) 0 : int_datum(a->arg.typid, acc->v1);
		case SV6_COUNT_DISTINCT:
			return Int64GetDatum(v6acc_count(a, acc));
		case SV6_SUM:
		case SV6_AVG:
			if (v6acc_is_nan(a, acc))
				return nan_datum();
			*isnull = acc->n == 0;
			if (acc->n == 0)
				return (Datum) 0;
			if (a->kind == SV6_SUM && a->arg.typid == FLOAT4OID)
				return Float4GetDatum((float4) v6acc_f64(acc));
			if (sift_v6_class(a->arg.typid) == SV6C_F64)
				return Float8GetDatum(a->kind == SV6_SUM ? v6acc_f64(acc) : v6acc_f64(acc) / (double) acc->n);
			if (a->kind == SV6_AVG)
				return DirectFunctionCall2(numeric_div, sift_v6_scaled_numeric(v6acc_i128(acc), a->arg.scale), NumericGetDatum(int64_to_numeric(acc->n)));
			if (a->restype == INT8OID)
			{
				const SiftI128 s = v6acc_i128(acc);

				if ((s.hi == 0 && (int64) s.lo >= 0) || (s.hi == -1 && (int64) s.lo < 0))
					return Int64GetDatum((int64) s.lo);
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			}
			return sift_v6_scaled_numeric(v6acc_i128(acc), a->arg.scale);
		default:
			if (v6acc_is_nan(a, acc))
				return nan_datum();
			*isnull = acc->n == 0;
			if (acc->n == 0)
				return (Datum) 0;
			if (sift_v6_class(a->arg.typid) == SV6C_TEXT)
				return PointerGetDatum(cstring_to_text_with_len((const char *) ((const SiftStrMM *) acc)->ptr, ((const SiftStrMM *) acc)->len));
			if (a->arg.typid == FLOAT4OID)
				return Float4GetDatum((float4) v6acc_f64(acc));
			if (a->arg.typid == FLOAT8OID)
				return Float8GetDatum(v6acc_f64(acc));
			if (a->arg.typid == NUMERICOID)
				return sift_v6_scaled_numeric((SiftI128) {.lo = acc->v0,.hi = (int64) acc->v0 < 0 ? -1 : 0}, a->arg.scale);
			return int_datum(a->arg.typid, (int64) acc->v0);
	}
}

void
v6g_key_datums(const SiftV6Group *g, const V6Set *set, const SiftSaltedHT *ht, const SiftAggSlot64 *s, Datum *values, bool *isnull, int only)
{
	const SiftV6Spec *spec = g->spec;

	if (set->packed)
	{
		for (int i = 0; i < set->nk; i++)
		{
			int64		x = 0;
			const int	k = set->kidx[i];

			if (only >= 0 && k != only)
				continue;
			isnull[k] = !sift_pack_get(&set->pack, &s->key, i, &x);
			values[k] = isnull[k] ? (Datum) 0 : int_datum(spec->key[k].typid, x);
		}
		return;
	}
	if (!set->wide)
	{
		isnull[set->kidx[0]] = s->key.i64[1] != 0;
		values[set->kidx[0]] = isnull[set->kidx[0]] ? (Datum) 0 : int_datum(spec->key[set->kidx[0]].typid, s->key.i64[0]);
		return;
	}
	{
		uint32		len;
		const uint8 *p = sift_ht_wide_key(ht, s, &len);

		for (int i = 0; i < set->nk; i++)
		{
			const int	k = set->kidx[i];

			isnull[k] = (s->key.wide.key_nulls >> i) & 1;
			values[k] = (Datum) 0;
			if (isnull[k])
				continue;
			if (sift_v6_class(spec->key[k].typid) == SV6C_TEXT)
			{
				uint32		n;

				memcpy(&n, p, sizeof(n));
				if (only < 0 || k == only)
					values[k] = PointerGetDatum(cstring_to_text_with_len((const char *) p + sizeof(n), n));
				p += sizeof(n) + n;
			}
			else
			{
				int64		x;

				memcpy(&x, p, sizeof(x));
				if (only < 0 || k == only)
					values[k] = int_datum(spec->key[k].typid, x);
				p += sizeof(x);
			}
		}
	}
}

static uint64
live_bytes(const SiftSaltedHT *ht)
{
	return (uint64) ht->count * (sizeof(SiftAggSlot64) + 1) + (uint64) ht->ext_used * ht->ext_stride + ht->arena_used;
}

static void
spill_all(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
	{
		V6Set	   *set = &g->set[s];

		if (set->nk == 0 || set->ht.count == 0)
			continue;
		if (set->spill == NULL)
		{
			MemoryContext old = MemoryContextSwitchTo(g->cx);

			set->spill = sift_gspill_new(&set->shape);
			MemoryContextSwitchTo(old);
		}
		sift_gspill_put_ht(set->spill, &set->ht);
		sift_ht_clear(&set->ht);
	}
	g->spilled = true;
}

void
v6g_spill_if_over(SiftV6Group *g)
{
	uint64		bytes = 0;

	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].nk > 0)
			bytes += live_bytes(&g->set[s].ht);
	if (bytes > g->budget - g->dist_budget)
		spill_all(g);
}

typedef struct V6Visit
{
	SiftV6Group *g;
	V6Set	   *set;
} V6Visit;

static void
store_groups(SiftV6Group *g, V6Set *set, const SiftSaltedHT *ht)
{
	const int	n = g->spec->nkeys + g->spec->naggs;
	Datum	   *values = palloc(Max(n, 1) * sizeof(Datum));
	bool	   *isnull = palloc(Max(n, 1));
	MemoryContext row = AllocSetContextCreate(CurrentMemoryContext, "sift agg v6 row", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(row);
	const SiftAggSlot64 *s;

	SIFT_HT_FOREACH(ht, s)
	{
		const V6Acc *acc = (const V6Acc *) sift_ht_ext(ht, s);

		memset(isnull, true, g->spec->nkeys * sizeof(bool));
		v6g_key_datums(g, set, ht, s, values, isnull, -1);
		for (int j = 0; j < g->spec->naggs; j++)
			values[g->spec->nkeys + j] = v6g_agg_datum(g, &g->spec->agg[j], &acc[j], set->mask, &isnull[g->spec->nkeys + j]);
		tuplestore_putvalues(g->store, g->desc, values, isnull);
		MemoryContextReset(row);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(row);
	pfree(values);
	pfree(isnull);
}

static void
visit_merged(void *arg, SiftSaltedHT *ht)
{
	V6Visit    *v = arg;

	store_groups(v->g, v->set, ht);
}

static void
store_plain(SiftV6Group *g, V6Set *set)
{
	const int	n = g->spec->nkeys + g->spec->naggs;
	Datum	   *values = palloc0(Max(n, 1) * sizeof(Datum));
	bool	   *isnull = palloc(Max(n, 1));

	memset(isnull, true, g->spec->nkeys * sizeof(bool));
	for (int j = 0; j < g->spec->naggs; j++)
		values[g->spec->nkeys + j] = v6g_agg_datum(g, &g->spec->agg[j], &set->plain[j], set->mask, &isnull[g->spec->nkeys + j]);
	tuplestore_putvalues(g->store, g->desc, values, isnull);
}

static void finish_in_cx(SiftV6Group *g);

void
sift_v6_group_finish(SiftV6Group *g)
{
	MemoryContext old = MemoryContextSwitchTo(g->cx);

	v6g_dist_finish(g);
	finish_in_cx(g);
	MemoryContextSwitchTo(old);
}

static void
finish_in_cx(SiftV6Group *g)
{
	g->cur = 0;
	for (int s = 0; s < g->nsets; s++)
	{
		g->set[s].emit_at = 0;
		g->set[s].emitted = false;
	}
	if (!g->spilled)
		return;
	spill_all(g);
	g->store = tuplestore_begin_heap(false, false, work_mem);
	g->mslot = MakeSingleTupleTableSlot(g->desc, &TTSOpsMinimalTuple);
	for (int s = 0; s < g->nsets; s++)
	{
		V6Visit		v = {g, &g->set[s]};

		if (g->set[s].nk == 0)
		{
			if (g->set[s].plain_row)
				store_plain(g, &g->set[s]);
		}
		else if (g->set[s].spill != NULL)
			sift_gspill_scan(g->set[s].spill, g->budget, visit_merged, &v);
	}
}

static const V6Acc *
next_group(SiftV6Group *g, V6Set *set, Datum *values, bool *isnull)
{
	const SiftAggSlot64 *s;

	if (set->nk == 0)
	{
		if (set->emitted || !set->plain_row)
			return NULL;
		set->emitted = true;
		return set->plain;
	}
	while (set->emit_at < set->ht.capacity && !(set->ht.ctrl[set->emit_at] & 0x80))
		set->emit_at++;
	if (set->emit_at >= set->ht.capacity)
		return NULL;
	s = &set->ht.slots[set->emit_at++];
	v6g_key_datums(g, set, &set->ht, s, values, isnull, -1);
	return (const V6Acc *) sift_ht_ext(&set->ht, s);
}

bool
sift_v6_group_next(SiftV6Group *g, Datum *values, bool *isnull)
{
	const SiftV6Spec *spec = g->spec;

	if (g->store != NULL)
	{
		const int	n = spec->nkeys + spec->naggs;

		MemoryContext old = MemoryContextSwitchTo(g->cx);
		const bool	got = tuplestore_gettupleslot(g->store, true, false, g->mslot);

		MemoryContextSwitchTo(old);
		if (!got)
			return false;
		slot_getallattrs(g->mslot);
		memcpy(values, g->mslot->tts_values, n * sizeof(Datum));
		memcpy(isnull, g->mslot->tts_isnull, n * sizeof(bool));
		return true;
	}
	for (; g->cur < g->nsets; g->cur++)
	{
		V6Set	   *set = &g->set[g->cur];
		const V6Acc *acc;

		memset(isnull, true, spec->nkeys * sizeof(bool));
		memset(values, 0, spec->nkeys * sizeof(Datum));
		if ((acc = next_group(g, set, values, isnull)) == NULL)
			continue;
		for (int j = 0; j < spec->naggs; j++)
			values[spec->nkeys + j] = v6g_agg_datum(g, &spec->agg[j], &acc[j], set->mask, &isnull[spec->nkeys + j]);
		return true;
	}
	return false;
}

uint64
sift_v6_group_bytes(const SiftV6Group *g)
{
	uint64		bytes = 0;

	for (int s = 0; s < g->nsets; s++)
		bytes += g->set[s].nk > 0 ? sift_ht_bytes(&g->set[s].ht) : 0;
	return bytes;
}

void
sift_v6_group_spill_stats(const SiftV6Group *g, uint64 *puts, uint64 *bytes)
{
	*puts = *bytes = 0;
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].spill != NULL)
		{
			*puts += sift_gspill_puts(g->set[s].spill);
			*bytes += sift_gspill_bytes(g->set[s].spill);
		}
}

void
sift_v6_group_free(SiftV6Group *g)
{
	v6g_dist_free(g);
	v6g_filter_free(g);
	if (g->wb.buf != NULL)
		sift_wide_batch_free(&g->wb);
	if (g->slab != NULL)
		sift_strslab_free(g->slab);
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].spill != NULL)
			sift_gspill_free(g->set[s].spill);
	if (g->mslot != NULL)
		ExecDropSingleTupleTableSlot(g->mslot);
	if (g->store != NULL)
		tuplestore_end(g->store);
	if (g->tx.buf != NULL)
		pfree(g->tx.buf);
}

const char *
sift_v6_group_mode(const SiftV6Group *g)
{
	return g->spec->nkeys == 0 ? "ungrouped" : "salted-ht";
}
