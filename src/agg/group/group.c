/*
 * agg/group/group.c - the groups of SiftAggV6: the keys and aggregates of a batch of up to 2048 rows folded into a table of groups, and the groups as the Datums of the plan.
 *
 * A set of keys with none: one set of accumulators folded by the vector kernels. A key of one column of 64 bits or less is the key of a slot of agg/ht/ht.h; any other key is
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

#include "agg/gpu/scan.h"
#include "agg/ht/ht.h"
#include "agg/ht/spill.h"
#include "agg/ht/state.h"
#include "agg/group/int.h"
#include "agg/group/nan.h"
#include "agg/engine.h"
#include "agg/argext/argext.h"
#include "agg/datum.h"
#include "agg/gen/gen.h"
#include "agg/settings.h"
#include "agg/group/compact.h"
#include "agg/group/rows.h"
#include "agg/pct.h"
#include "core/memtrack.h"

/* NaN rows a numeric aggregate folds: counted in aux, but the sum of a derived numeric is NaN for good (v6_acc_wide.h) */
static inline void
fold_nan(const SiftV6Agg *a, V6Acc *acc, uint64 rows)
{
	if (rows == 0)
		return;
	if (v6g_sum_wide(a))
		v6acc_wide_set_nan(acc);
	else
		acc->aux += rows;
}

static void
wide_row(V6Acc *acc, const SiftVec *v, uint16 r)
{
	switch (v->type)
	{
		case SVEC_NUM256:
			v6acc_wide_add_i256(acc, &((const SiftI256 *) v->data)[r]);
			break;
		case SVEC_NUM128:
			v6acc_wide_add_i128(acc, ((const SiftI128 *) v->data)[r]);
			break;
		default:
			v6acc_wide_add_i64(acc, int_at(v, r));
			break;
	}
}

/* the rows of mk (all with a value) of a derived numeric: a vector of int64 is summed by the kernel (2048 of them fit 128 bits), the others a row at a time */
static void
wide_batch(V6Acc *acc, const SiftVec *v, const uint64 *mk, uint16 n_raw)
{
	uint32		r;

	if (v->type == SVEC_NUM64)
		v6acc_wide_add_i128(acc, sift_aggk6->sum_i64_to_i128(v->data, mk, n_raw, (SiftI128) {0}));
	else
		V6_FOR_EACH_ROW(mk, n_raw, r)
			wide_row(acc, v, (uint16) r);
}

/* sift.arg_max, sift.arg_min: n the rows with a cmp, v0 the order key of the best cmp, v1 its val, aux 1 when that val is NULL (v6_argext.h) */
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
		fold_nan(a, acc, sift_v6_agg_takes_nan(a) && v6acc_nan_row(v, r));
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
				v6g_sum_f64(acc, ((const double *) v->data)[r]);
			else if (v6g_sum_wide(a))
				wide_row(acc, v, r);
			else
			{
				SiftI128	s = v6acc_i128(acc);

				sift_i128_add(&s, v->type == SVEC_NUM128 ? ((const SiftI128 *) v->data)[r] : sift_i128_of_i64(int_at(v, r)));
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
		if (v6g_is_gen(a))
		{
			uint32		r;

			V6_FOR_EACH_ROW(mk, bs->n_raw, r)
				v6_gen_fold(g->gen, j, acc, argvec, (uint16) r);
			continue;
		}
		if (a->kind == SV6_ARG_MAX || a->kind == SV6_ARG_MIN)
		{
			uint32		r;

			V6_FOR_EACH_ROW(mk, bs->n_raw, r)
				fold_row(g, a, acc, v, argvec[g->spec->naggs + j], (uint16) r);
			continue;
		}
		if (v->nan != NULL && sift_v6_agg_takes_nan(a))
			fold_nan(a, acc, v6acc_nan_rows(v, mk, bs->n_raw));
		if (sift_v6_kind_distinct(a->kind))
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
			uint32		r;

			V6_FOR_EACH_ROW(mk, bs->n_raw, r)
				if (is_valid(v, r))
					fold_row(g, a, acc, v, NULL, (uint16) r);
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
				if (v->type == SVEC_F64)
				{
					uint32		r;

					V6_FOR_EACH_ROW(mk, bs->n_raw, r)
						v6g_sum_f64(acc, ((const double *) v->data)[r]);
				}
				else if (v6g_sum_wide(a))
					wide_batch(acc, v, mk, bs->n_raw);
				else if (v->type == SVEC_NUM128)
					v6acc_set_i128(acc, sift_aggk6->sum_i128(v->data, mk, bs->n_raw, v6acc_i128(acc)));
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

void
v6g_append_key_str(SiftV6Group *g, const char *p, uint32 len)
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
		v6g_append_key_str(g, d->bytes + d->off[code], d->off[code + 1] - d->off[code]);
	return sift_ht_upsert_wide_lazy(&set->ht, sift_aggk6_hash1(sift_aggk6_hash_bytes(g->kbuf.data, g->kbuf.len) ^ null), g->kbuf.data, g->kbuf.len, null, &found);
}

/* A new epoch of code_slots: every code is then unresolved, with no pass over the codes (the epochs are zeroed only when the counter wraps) */
static void
code_epoch_next(SiftV6Group *g, uint32 ncodes)
{
	if (ncodes > g->code_cap)
	{
		const uint32 cap = Max(256u, pg_nextpower2_32(ncodes));

		g->code_slots = g->code_slots ? repalloc(g->code_slots, cap * sizeof(*g->code_slots)) : palloc(cap * sizeof(*g->code_slots));
		g->code_epoch = g->code_epoch ? repalloc(g->code_epoch, cap * sizeof(*g->code_epoch)) : palloc(cap * sizeof(*g->code_epoch));
		memset(g->code_epoch + g->code_cap, 0, (cap - g->code_cap) * sizeof(*g->code_epoch));
		g->code_cap = cap;
	}
	if (++g->code_epoch_cur == 0)
	{
		memset(g->code_epoch, 0, g->code_cap * sizeof(*g->code_epoch));
		g->code_epoch_cur = 1;
	}
}

static inline uint32
code_of(const SiftV6Group *g, const SiftVec *kv, uint16 k)
{
	return is_valid(kv, g->sel[k]) ? ((const uint32 *) kv->data)[g->sel[k]] : ((const SiftGranuleDict *) kv->dict)->n;
}

static inline SiftAggSlot64 *
code_resolve(SiftV6Group *g, V6Set *set, const SiftGranuleDict *d, uint32 c)
{
	if (g->code_epoch[c] != g->code_epoch_cur)
	{
		g->code_slots[c] = code_slot(g, set, d, c);
		g->code_epoch[c] = g->code_epoch_cur;
	}
	return g->code_slots[c];
}

/* The slots of the rows of a batch of one dictionary-coded key: a code is resolved once. A group made by a code can grow the table, which moves every slot taken so far: the codes are resolved again from the first row */
static void
code_slots(SiftV6Group *g, V6Set *set, const SiftVec *kv, uint16 n)
{
	const SiftGranuleDict *d = kv->dict;
	uint32		gen;

	code_epoch_next(g, d->n + 1);
	gen = set->ht.gen;
	for (uint16 k = 0; k < n; k++)
	{
		g->slots[k] = code_resolve(g, set, d, code_of(g, kv, k));
		if (set->ht.gen != gen)
		{
			code_epoch_next(g, d->n + 1);
			gen = set->ht.gen;
			k = (uint16) -1;
		}
	}
}

static inline bool
locked(const V6Set *set)
{
	return set->ht.lock_at != 0 && !set->frozen && set->ht.count >= set->ht.lock_at;
}

/* The rows of the batch the groups take: those of the mask, and once the table is locked only those whose first key is one of its groups' (when that is an integer) */
static uint16
batch_rows(SiftV6Group *g, V6Set *set, const SiftBatchSel *bs, SiftVec *const *keyvec)
{
	const SiftVec *k0 = keyvec[set->kidx[0]];
	uint64		hit[SIFT_VEC_WORDS];

	if (!locked(set))
		return sift_aggk6->mask_to_sel16(g->sel, bs->mask, bs->n_raw);
	if (!set->lock.built)
		sift_lock_set_build(&set->lock, &set->ht, set->packed, &set->pack, k0->type == SVEC_I64);
	if (!set->lock.usable || k0->type != SVEC_I64)
		return sift_aggk6->mask_to_sel16(g->sel, bs->mask, bs->n_raw);
	sift_lock_set_rows(&set->lock, k0, bs->mask, bs->n_raw, hit);
	return sift_aggk6->mask_to_sel16(g->sel, hit, bs->n_raw);
}

void
v6g_grouped_slots(SiftV6Group *g, V6Set *set, uint16 n, SiftVec *const *keyvec)
{
	const bool	coded = set->wide && set->nk == 1 && keyvec[set->kidx[0]]->type == SVEC_DICT32;

	if (coded)
		code_slots(g, set, keyvec[set->kidx[0]], n);
	else if (set->packed)
	{
		SiftVec    *kvs[SIFT_PACK_MAX_KEYS];

		for (int i = 0; i < set->nk; i++)
			kvs[i] = keyvec[set->kidx[i]];
		sift_pack_batch(&set->pack, kvs, g->sel, n, g->hash, g->ikey);
		sift_ht_upsert_batch(&set->ht, g->hash, g->ikey, n, g->slots);
	}
	else if (set->wide && set->nk == 1 && keyvec[set->kidx[0]]->type == SVEC_STRVIEW)
		sift_str_batch_upsert(&set->ht, keyvec[set->kidx[0]], g->sel, n, g->hash, g->slots);
	else if (set->wide)
	{
		sift_wide_batch_build(&g->wb, set->kidx, set->nk, keyvec, g->sel, n);
		sift_wide_batch_upsert(&set->ht, &g->wb, n, g->slots);
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
	}
}

static uint64
wide_row_bytes(const V6Set *set, SiftVec *const *keyvec, uint16 r)
{
	uint64		bytes = 0;

	for (int i = 0; i < set->nk; i++)
	{
		const SiftVec *kv = keyvec[set->kidx[i]];

		if (!is_valid(kv, r))
			continue;
		if (kv->type == SVEC_STRVIEW)
			bytes += sizeof(uint32) + ((const SiftStrView *) kv->data)[r].len;
		else if (kv->type == SVEC_DICT32)
		{
			const SiftGranuleDict *d = kv->dict;
			const uint32 c = ((const uint32 *) kv->data)[r];

			bytes += sizeof(uint32) + (d->off[c + 1] - d->off[c]);
		}
		else
			bytes += sizeof(int64);
	}
	return bytes;
}

static void
grouped_slice(SiftV6Group *g, V6Set *set, uint16 n, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	v6g_grouped_slots(g, set, n, keyvec);
	if (set->frozen)
		n = v6g_rows_split(g, set, keyvec, argvec, n);
	else if (locked(set))
		n = sift_lock_compact(g->sel, g->slots, n);
	for (int d = 0; d < g->ndist; d++)
		v6g_dist_group_rows(g, set, g->dagg[d], argvec, n);
	for (int j = 0; j < g->spec->naggs; j++)
	{
		if (g->spec->agg[j].kind == SV6_COUNT_STAR && g->spec->agg[j].fclause == 0)
		{
			for (uint16 k = 0; k < n; k++)
				((V6Acc *) (sift_ht_ext(&set->ht, g->slots[k]) + g->off[j]))->n++;
			continue;
		}
		if (v6g_is_gen(&g->spec->agg[j]))
		{
			for (uint16 k = 0; k < n; k++)
				if (v6g_row_takes(g, &g->spec->agg[j], g->sel[k]))
					v6_gen_fold(g->gen, j, (V6Acc *) (sift_ht_ext(&set->ht, g->slots[k]) + g->off[j]), argvec, g->sel[k]);
			continue;
		}
		for (uint16 k = 0; k < n; k++)
			if (v6g_row_takes(g, &g->spec->agg[j], g->sel[k]))
				fold_row(g, &g->spec->agg[j], (V6Acc *) (sift_ht_ext(&set->ht, g->slots[k]) + g->off[j]), argvec[j], argvec[g->spec->naggs + j], g->sel[k]);
	}
}

static void
grouped_batch(SiftV6Group *g, V6Set *set, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	uint16		n;

	if (v6g_compact_batch(g, set, bs, keyvec, argvec))
		return;
	n = batch_rows(g, set, bs, keyvec);
	g->frows = g->nfilters > 0 ? sift_fmasks_rows(g->fmasks) : NULL;
	if (set->wide && n > 1)
	{
		const uint64 cap = Max(Min((g->budget - g->dist_budget) / 4, MaxAllocSize / 4), 64 * 1024);
		uint64		total = 0;

		for (uint16 k = 0; k < n && total <= cap; k++)
			total += wide_row_bytes(set, keyvec, g->sel[k]);
		if (total > cap)
		{
			uint16		saved[SIFT_VEC_LEN];

			memcpy(saved, g->sel, n * sizeof(uint16));
			for (uint16 off = 0; off < n;)
			{
				uint16		chunk = 0;
				uint64		bytes = 0;

				while (off + chunk < n)
				{
					const uint64 rb = wide_row_bytes(set, keyvec, saved[off + chunk]);

					if (chunk > 0 && bytes + rb > cap)
						break;
					bytes += rb;
					chunk++;
				}
				memcpy(g->sel, saved + off, chunk * sizeof(uint16));
				grouped_slice(g, set, chunk, keyvec, argvec);
				off += chunk;
				if (off < n)
					v6g_rows_if_over(g);
			}
			return;
		}
	}
	grouped_slice(g, set, n, keyvec, argvec);
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
	if (g->spec->constant_groups && n > 0)
		g->set[0].plain_row = true;
}

void
v6g_update_set(SiftV6Group *g, int only, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	MemoryContext old;

	if (bs->n_active == 0)
		return;
	old = MemoryContextSwitchTo(g->cx);
	v6g_filter_eval(g, bs, argvec);
	if (g->gkeys != NULL)
		keyvec = sift_v6_genkeys_apply(g->gkeys, bs, keyvec);
	for (int s = 0; s < g->nsets; s++)
		if (only >= 0 && s != only)
			continue;
		else if (g->set[s].nk == 0)
		{
			g->set[s].plain_row |= g->spec->constant_groups;
			plain_batch(g, &g->set[s], bs, argvec);
		}
		else
			grouped_batch(g, &g->set[s], bs, keyvec, argvec);
	v6g_rows_if_over(g);
	MemoryContextSwitchTo(old);
}

void
sift_v6_group_update(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	v6g_update_set(g, -1, bs, keyvec, argvec);
}

void
v6g_merge_acc(SiftV6Group *g, const SiftV6Agg *a, V6Acc *d, const V6Acc *s)
{
	if (v6g_is_gen(a))
		v6_gen_combine(g->gen, (int) (a - g->spec->agg), d, s);
	else if (v6g_is_ext(a))
		v6g_ext_merge(g, a, d, s);
	else if (v6g_is_text_mm(a))
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
		v6g_merge_acc(g, &spec->agg[j], (V6Acc *) dst_ext + j, (const V6Acc *) src_ext + j);
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

#define SET_INIT_SLOTS	64

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
	set->plain_row = !spec->constant_groups;
	if (set->nk == 0)
		set->plain = palloc0(Max(spec->naggs, 1) * sizeof(V6Acc));
	else
		/* MEM[MEM-AGG-03]: SiftV6Group set_init table (SET_INIT_SLOTS=64 slots * 65B = 4KB per grouping set, grown with the groups; was 1024 slots = 66.5KB, 1MB for CUBE of 4) */
		sift_ht_init_ex(&set->ht, SET_INIT_SLOTS, set->wide, g->stride, g->dist_off);
	if (spec->nsets == 0 && g->ndist == 0 && set->nk > 0)
		set->ht.lock_at = spec->lock_k;
	set->shape = (SiftRadixShape) {.wide_key = set->wide,.ext_stride = g->stride,.ext_dist_off = g->dist_off,.ctx = g,.init = shape_init,.merge = shape_merge};
}

/* the group table of a set for the groups the planner expects, when that many (a quarter of the memory the groups may take at most) */
void
v6g_presize_set(SiftV6Group *g, V6Set *set)
{
	const uint64 per_group = sizeof(SiftAggSlot64) + 1 + g->stride;
	const uint64 room = (g->budget - g->dist_budget) / 4 / per_group;
	const uint64 est = g->spec->groups_est / g->nsets;

	if (set->nk > 0 && est >= 4 * SIFT_VEC_LEN)
		sift_ht_presize(&set->ht, (uint32) Min(est, room));
}

/* the sorts of the percentile_cont of the sets of no keys: their answers (finish), or their end (the state is dropped) */
static void
plain_pcts(SiftV6Group *g, bool finish)
{
	for (int s = 0; s < g->nsets; s++)
		for (int j = 0; g->set[s].nk == 0 && j < g->spec->naggs; j++)
			if (g->spec->agg[j].kind == SV6_PCT_CONT && finish)
				sift_v6_pct_finish(&g->set[s].plain[j], g->spec->agg[j].frac);
			else if (g->spec->agg[j].kind == SV6_PCT_CONT)
				sift_v6_pct_release(&g->set[s].plain[j]);
}

/* The state of a participant of a parallel run: its rows of a COUNT(DISTINCT) go to put, and its table holds what a participant may hold before it hands its groups over, whatever work_mem says */
void
sift_v6_group_scan_sink(SiftV6Group *g, SiftV6DistPut put, void *arg)
{
	g->dist_put = put;
	g->dist_arg = arg;
	if (g->gen != NULL)
		v6_gen_unbound(g->gen);
	for (int s = 0; s < g->nsets; s++)
		if (v6g_compact_eligible(g, &g->set[s]))
			v6g_presize_set(g, &g->set[s]);
	g->budget = UINT64_MAX;
	g->dist_budget = 0;
	for (int s = 0; s < g->nsets; s++)
		g->set[s].cmp_off = true;
}

/* The state of a partition that did not spill, emptied for the records of the next partition: its tables and buffers stay */
void
sift_v6_group_recycle(SiftV6Group *g)
{
	SIFT_INVARIANT(!g->spilled, "a state whose tables went to the spill was recycled");
	plain_pcts(g, false);
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
	v6g_bulk_free(g);
	if (g->gen != NULL)
		v6_gen_release(g->gen);
}

/* the group tables of the sets that have no compact table to take the groups first */
static void
presize(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
		if (!v6g_compact_eligible(g, &g->set[s]))
			v6g_presize_set(g, &g->set[s]);
}

SiftV6Group *
sift_v6_group_new(const SiftV6Spec *spec, MemoryContext cx, TupleDesc desc)
{
	SiftV6Group *g = MemoryContextAllocZero(cx, sizeof(SiftV6Group));
	MemoryContext old = MemoryContextSwitchTo(cx);
	const uint32 all = spec->nkeys >= 32 ? UINT32_MAX : (((uint32) 1) << spec->nkeys) - 1;
	int			npct = 0;

	g->gkeys = sift_v6_genkeys_new(cx, spec);
	g->spec = g->gkeys != NULL ? sift_v6_genkeys_spec(g->gkeys) : spec;
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
		if (v6g_is_text_mm(&spec->agg[j]) && g->slab == NULL)
			g->slab = sift_strslab_new(cx);
	for (int j = 0; j < spec->naggs; j++)
		npct += spec->agg[j].kind == SV6_PCT_CONT;
	g->pct_kb = Max(work_mem / Max(npct, 1), 64);
	if (sift_v6_spec_has_gen(spec))
		g->gen = v6_gen_new(spec, cx, g->budget);
	v6g_spill_own(g);
	v6g_mem_begin(g);
	presize(g);
	MemoryContextSwitchTo(old);
	return g;
}

static void finish_in_cx(SiftV6Group *g);

void
sift_v6_group_finish(SiftV6Group *g)
{
	MemoryContext old = MemoryContextSwitchTo(g->cx);

	v6g_dist_finish(g);
	v6g_bulk_settle(g);
	plain_pcts(g, true);
	for (int s = 0; s < g->nsets && g->spec->topk == 0; s++)
		v6g_compact_flush(g, &g->set[s]);
	v6g_spill_if_over(g);
	v6g_mem_check(g, true);
	if (g->gen != NULL)
		v6_gen_finish(g->gen, g);
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
	v6g_spill_all(g);
	/* MEM[MEM-AGG-02]: SiftV6Group spill-emit peak (the tables are destroyed before the partitions are read, a budget * 3/4 table at a time; no store of the groups: each partition's are returned, and its file is gone) */
	v6g_spill_open(g);
}

static const V6Acc *
next_group(SiftV6Group *g, V6Set *set, Datum *values, bool *isnull)
{
	const SiftSaltedHT *ht = set->it != NULL ? set->out : &set->ht;
	const SiftAggSlot64 *s;

	if (set->nk == 0)
	{
		if (set->emitted || !set->plain_row)
			return NULL;
		set->emitted = true;
		return set->plain;
	}
	for (;;)
	{
		while (ht != NULL && set->emit_at < ht->capacity && !(ht->ctrl[set->emit_at] & 0x80))
			set->emit_at++;
		if (ht != NULL && set->emit_at < ht->capacity)
			break;
		if (set->it == NULL || (ht = v6g_spill_next(set)) == NULL)
			return NULL;
	}
	s = &ht->slots[set->emit_at++];
	v6g_key_datums(g, set, ht, s, values, isnull, V6_KEYS_ALL);
	return (const V6Acc *) sift_ht_ext(ht, s);
}

bool
sift_v6_group_next(SiftV6Group *g, Datum *values, bool *isnull)
{
	const SiftV6Spec *spec = g->spec;

	if (g->bulk != NULL)
		return v6g_bulk_next(g, values, isnull);
	for (int s = 0; s < g->nsets; s++)
		v6g_compact_flush(g, &g->set[s]);
	do
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
	while (v6g_rows_replay(g));
	return false;
}

uint64
sift_v6_group_bytes(const SiftV6Group *g)
{
	uint64		bytes = v6g_bulk_bytes(g);

	for (int s = 0; s < g->nsets; s++)
		bytes += g->set[s].nk > 0 ? sift_ht_bytes(&g->set[s].ht) + v6g_compact_bytes(&g->set[s]) : 0;
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
	v6g_rows_stats(g, puts, bytes);
}

void
sift_v6_group_free(SiftV6Group *g)
{
	plain_pcts(g, false);
	v6g_rows_free(g);
	for (int s = 0; s < g->nsets; s++)
		v6g_compact_free(&g->set[s]);
	v6g_dist_free(g);
	v6g_filter_free(g);
	v6g_bulk_free(g);
	if (g->gen != NULL)
		v6_gen_free(g->gen);
	if (g->wb.buf != NULL)
		sift_wide_batch_free(&g->wb);
	if (g->slab != NULL)
		sift_strslab_free(g->slab);
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].spill != NULL)
		{
			if (g->set[s].it != NULL)
				sift_gspill_iter_end(g->set[s].it);
			sift_gspill_free(g->set[s].spill);
		}
	if (g->tx.buf != NULL)
		pfree(g->tx.buf);
	if (g->gkeys != NULL)
		sift_v6_genkeys_free(g->gkeys);
}

const char *
sift_v6_group_mode(const SiftV6Group *g)
{
	return g->spec->nkeys == 0 ? "ungrouped" : "salted-ht";
}
