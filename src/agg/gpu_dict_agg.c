/*
 * agg/gpu_dict_agg.c - a text column on the device as the codes of the dictionaries of the parts (agg/gpu_dict_agg.h): the code of each constant in each part, and the units of the scan that
 * share what the codes mean.
 */
#include "postgres.h"

#include "agg/gpu_dict_agg.h"
#include "col/gpu.h"

/* The code of the string in the dictionary of the part, whose strings are in byte order; -1: the part has none like it */
static int64
code_in(const SiftPartDict *d, const char *s, uint32 len)
{
	uint32		lo = 0,
				hi = d->n;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2,
					mlen = d->off[mid + 1] - d->off[mid];
		int			c = memcmp(d->bytes + d->off[mid], s, Min(mlen, len));

		if (c == 0)
			c = (mlen > len) - (mlen < len);
		if (c == 0)
			return mid;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return -1;
}

static bool
has_text_key(const SiftGpuQuery *q)
{
	for (int i = 0; q->spec != NULL && i < q->spec->nkeys; i++)
		if (sift_gpu_slot_is_text(q, sift_gpu_query_slot(q, q->spec->key[i].attno)))
			return true;
	return false;
}

/* The codes of the constants in part i, and whether they leave it no row */
static bool
part_codes(const SiftGpuQuery *q, const SiftGpuPlan *p, uint32 i, int64 *code)
{
	const SiftGpuDictQual *dq = q->dict;
	bool		none = false;

	for (int j = 0; dq != NULL && j < dq->n; j++)
	{
		code[j] = code_in(sift_gpu_f2_dict(p, i, dq->cmp[j].slot), dq->cmp[j].str, dq->cmp[j].len);
		none |= code[j] < 0 && !dq->cmp[j].ne;
	}
	return none;
}

static bool
same_codes(const SiftGpuDictQual *dq, const int64 *a, const int64 *b)
{
	for (int j = 0; dq != NULL && j < dq->n; j++)
		if (a[j] != b[j])
			return false;
	return true;
}

static bool
compile_where(const SiftGpuDictQual *dq, const int64 *code, SiftGpuUnit *u)
{
	for (int j = 0; dq != NULL && j < dq->n; j++)
		dq->cmp[j].code->i64 = code[j];
	return dq == NULL || dq->where == NULL || sift_vexpr_compile(dq->where, &u->where);
}

SiftGpuUnits *
sift_gpu_units(const SiftGpuQuery *q, const SiftGpuPlan *p, MemoryContext cx, const char **why)
{
	SiftGpuUnits *us;
	int64		code[SIFT_GPU_DICT_CMPS] = {0},
				last[SIFT_GPU_DICT_CMPS] = {0};
	bool		text = false;

	*why = NULL;
	for (int c = 0; c < q->ncols; c++)
		text |= sift_gpu_slot_is_text(q, c);
	if (!text)
		return NULL;
	us = MemoryContextAllocZero(cx, sizeof(SiftGpuUnits));
	us->text_key = has_text_key(q);
	us->unit = MemoryContextAllocZero(cx, Max(p->ndevice, 1) * sizeof(SiftGpuUnit));
	for (uint32 i = 0; i < p->ndevice; i++)
	{
		SiftGpuUnit *u = us->n > 0 ? &us->unit[us->n - 1] : NULL;
		uint32		g0,
					g1;
		const bool	none = part_codes(q, p, i, code);

		sift_gpu_f2_part(p, i, &g0, &g1);
		if (u != NULL && !us->text_key && same_codes(q->dict, code, last))
		{
			u->g1 = g1;
			continue;
		}
		u = &us->unit[us->n++];
		u->g0 = g0;
		u->g1 = g1;
		u->part = i;
		u->skip = none;
		memcpy(last, code, sizeof(last));
		if (!none && !compile_where(q->dict, code, u))
		{
			*why = "a comparison with the constant of a text does not compile for the device";
			return NULL;
		}
	}
	return us;
}

bool
sift_gpu_unit_str(const SiftGpuPlan *p, const SiftGpuUnit *u, int slot, int64 code, SiftV6StrIn *out)
{
	const SiftPartDict *d = sift_gpu_f2_dict(p, u->part, slot);

	if (code < 0 || code >= (int64) d->n)
		return false;
	*out = (SiftV6StrIn) {.p = d->bytes + d->off[code],.len = d->off[code + 1] - d->off[code]};
	return true;
}

int
sift_gpu_units_to_run(const SiftGpuUnits *us)
{
	int			n = 0;

	for (uint32 i = 0; us != NULL && i < us->n; i++)
		n += !us->unit[i].skip;
	return us != NULL ? n : 1;
}

const SiftV6StrIn *
sift_gpu_key_strings(const SiftGpuQuery *q, const SiftGpuPlan *p, const SiftGpuUnits *us, const SiftGpuUnit *u, uint32 nkeys, const int *key_slot, const int64 *key, uint64 nulls,
					 SiftV6StrIn *str)
{
	if (us == NULL || !us->text_key)
		return NULL;
	memset(str, 0, SIFT_VK_MAXKEYS * sizeof(*str));
	for (uint32 i = 0; i < nkeys; i++)
		if (sift_gpu_slot_is_text(q, key_slot[i]) && !((nulls >> i) & 1))
			SIFT_INVARIANT(sift_gpu_unit_str(p, u, key_slot[i], key[i], &str[i]), "sift gpu: the code %lld of a text key is in no dictionary of its part", (long long) key[i]);
	return str;
}
