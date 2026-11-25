/*
 * col/f2/cube/pairmap.c - the pairs (member, scope key) of a scope, in memory as a compressed sparse row table: the keys of a member are a
 * slice of one array. A build whose granules arrive in ascending member order finds each member by galloping from the previous one.
 */
/* check-memory: allow-allocs the arrays follow the pairs of the scope relation, which the build holds in memory by design (12 bytes a member, 4 a pair) */
#include "core/host.h"

#include "core/cmp.h"
#include "col/f2/cube/meta.h"
#include "col/f2/cube/probe.h"

static int
pair_cmp(const void *a, const void *b)
{
	const SiftScopeMemberKey *x = a,
			   *y = b;

	if (x->member != y->member)
		return x->member < y->member ? -1 : 1;
	return x->key < y->key ? -1 : x->key > y->key;
}

SiftScopePairMap *
sift_spm_build(SiftScopeMemberKey *pairs, uint32 n, bool multiset)
{
	SiftScopePairMap *pm = palloc0(sizeof(*pm));
	/* MEM[MEM-SCOPE-03]: pair map CSR arrays at counted size (8 B * keys + 12 B * members) */
	int64	   *keys = palloc(sizeof(int64) * Max(n, 1));
	uint32		nk = 0,
				nmem = 0,
				m = 0;

	qsort(pairs, n, sizeof(*pairs), pair_cmp);
	for (uint32 i = 0; i < n; i++)
		if (multiset || i == 0 || pair_cmp(&pairs[i - 1], &pairs[i]) != 0)
			pairs[m++] = pairs[i];
	n = m;
	for (uint32 i = 0; i < n; i++)
		keys[i] = pairs[i].key;
	qsort(keys, n, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < n; i++)
		if (nk == 0 || keys[nk - 1] != keys[i])
			keys[nk++] = keys[i];
	for (uint32 i = 0; i < n; i++)
		nmem += i == 0 || pairs[i].member != pairs[i - 1].member;
	pm->nkeys = nk;
	pm->scope_keys = repalloc(keys, sizeof(int64) * Max(nk, 1));
	pm->npairs = n;
	pm->member_ids = palloc(sizeof(int64) * Max(nmem, 1));
	pm->offsets = palloc(sizeof(uint32) * (nmem + 1));
	pm->key_ordinals = palloc(sizeof(uint32) * Max(n, 1));
	for (uint32 i = 0; i < n; i++)
	{
		if (i == 0 || pairs[i].member != pairs[i - 1].member)
		{
			pm->member_ids[pm->nmembers] = pairs[i].member;
			pm->offsets[pm->nmembers++] = i;
		}
		pm->key_ordinals[i] = (uint32) sift_spm_key_ordinal(pm, pairs[i].key);
	}
	pm->offsets[pm->nmembers] = n;
	return pm;
}

SiftScopePairMap *
sift_spm_probed(int64 *keys, uint32 nkeys, uint32 nmembers, uint32 npairs, SiftScopePairProbe *probe)
{
	SiftScopePairMap *pm = palloc0(sizeof(*pm));

	pm->scope_keys = keys;
	pm->nkeys = nkeys;
	pm->nmembers = nmembers;
	pm->npairs = npairs;
	pm->probe = probe;
	return pm;
}

/* The key ordinals of the member in the buffer of the probe, ascending: how many */
static uint32
probe_ords(const SiftScopePairMap *pm, int64 member)
{
	SiftScopePairProbe *pr = pm->probe;
	const int64 *keys;
	uint32		n = pr->member_keys(pr->arg, member, &keys);

	if (n > pr->cap_ords)
	{
		pr->cap_ords = Max(n, pr->cap_ords * 2);
		pr->ords = pr->ords == NULL ? MemoryContextAlloc(GetMemoryChunkContext(pr), sizeof(uint32) * pr->cap_ords) : repalloc(pr->ords, sizeof(uint32) * pr->cap_ords);
	}
	for (uint32 i = 0; i < n; i++)
	{
		int32		ord = sift_spm_key_ordinal(pm, keys[i]);

		SIFT_INVARIANT(ord >= 0, "scope pairs: key %lld of member %lld is not a key of the scope", (long long) keys[i], (long long) member);
		pr->ords[i] = (uint32) ord;
	}
	return n;
}

/* The first member at or after from whose id is >= member: galloping, then a binary search in the last step */
static uint32
member_lower_bound(const SiftScopePairMap *pm, uint32 from, int64 member)
{
	uint32		step = 1,
				lo = from,
				hi;

	if (from >= pm->nmembers || pm->member_ids[from] >= member)
		return from;
	while (lo + step < pm->nmembers && pm->member_ids[lo + step] < member)
	{
		lo += step;
		step *= 2;
	}
	hi = Min(lo + step, pm->nmembers);
	lo++;
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (pm->member_ids[mid] < member)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static int32
base_ordinal(const int64 *keys, uint32 n, int64 key)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (keys[mid] == key)
			return (int32) mid;
		if (keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return -1;
}

int32
sift_spm_key_ordinal(const SiftScopePairMap *pm, int64 scope_key)
{
	int32		at;

	if (pm->base == NULL)
		return base_ordinal(pm->scope_keys, pm->nkeys, scope_key);
	if ((at = base_ordinal(pm->base->scope_keys, pm->base_nkeys, scope_key)) >= 0)
		return at;
	at = base_ordinal(pm->extra_keys, pm->nextra_keys, scope_key);
	return at < 0 ? -1 : (int32) pm->base_nkeys + at;
}

int64
sift_spm_key_of(const SiftScopePairMap *pm, uint32 ord)
{
	if (pm->base == NULL)
		return pm->scope_keys[ord];
	return ord < pm->base_nkeys ? pm->base->scope_keys[ord] : pm->extra_keys[ord - pm->base_nkeys];
}

/*
 * The map of the object with the pairs of members (ascending, distinct) replaced by now (by member, then key): the edited members get a small table of their own and every other
 * member is the object's, which is borrowed and never copied. Keys new to the object are kept apart (extra_keys) and numbered after the object's. For the reads of one statement only.
 */
SiftScopePairMap *
sift_spm_patched(const SiftScopePairMap *pm, const int64 *members, uint32 nm, const SiftScopeMemberKey *now, uint32 nnow)
{
	SiftScopePairMap *out = palloc0(sizeof(*out));
	int64	   *extra = palloc(sizeof(int64) * Max(nnow, 1));
	uint32		nx = 0,
				u = 0;

	for (uint32 k = 0; k < nnow; k++)
		if (sift_spm_key_ordinal(pm, now[k].key) < 0)
			extra[nx++] = now[k].key;
	qsort(extra, nx, sizeof(int64), sift_cmp_i64);
	for (uint32 k = 0; k < nx; k++)
		if (u == 0 || extra[u - 1] != extra[k])
			extra[u++] = extra[k];
	out->base = pm;
	out->base_nkeys = pm->nkeys;
	out->extra_keys = extra;
	out->nextra_keys = u;
	out->nkeys = pm->nkeys + u;
	out->tomb_members = memcpy(palloc(sizeof(int64) * Max(nm, 1)), members, sizeof(int64) * nm);
	out->ntomb = nm;
	out->member_ids = palloc(sizeof(int64) * Max(nnow, 1));
	out->offsets = palloc(sizeof(uint32) * (nnow + 1));
	out->key_ordinals = palloc(sizeof(uint32) * Max(nnow, 1));
	for (uint32 k = 0; k < nnow; k++)
	{
		if (k == 0 || now[k].member != now[k - 1].member)
		{
			out->member_ids[out->nmembers] = now[k].member;
			out->offsets[out->nmembers++] = k;
		}
		out->key_ordinals[k] = (uint32) sift_spm_key_ordinal(out, now[k].key);
	}
	out->offsets[out->nmembers] = nnow;
	out->npairs = nnow;
	return out;
}

void
sift_spm_free(SiftScopePairMap *pm)
{
	if (pm->probe != NULL)
	{
		pm->probe->close(pm->probe->arg);
		if (pm->probe->ords != NULL)
			pfree(pm->probe->ords);
		pfree(pm->probe);
		pfree(pm->scope_keys);
		pfree(pm);
		return;
	}
	pfree(pm->member_ids);
	pfree(pm->offsets);
	pfree(pm->key_ordinals);
	if (pm->base != NULL)
	{
		pfree(pm->tomb_members);
		pfree(pm->extra_keys);
	}
	else
		pfree(pm->scope_keys);
	pfree(pm);
}

bool
sift_spm_find(SiftScopePairMap *pm, int64 member, uint32 *lo, uint32 *hi)
{
	uint32		at;

	if (pm->probe != NULL)
	{
		*lo = 0;
		*hi = probe_ords(pm, member);
		pm->key_ordinals = pm->probe->ords;
		return *hi > 0;
	}
	if (pm->cursor >= pm->nmembers || pm->member_ids[pm->cursor] > member)
		pm->cursor = 0;			/* members did not arrive ascending: search again from the start */
	at = member_lower_bound(pm, pm->cursor, member);
	pm->cursor = Min(at, pm->nmembers);
	if (at >= pm->nmembers || pm->member_ids[at] != member)
		return false;
	*lo = pm->offsets[at];
	*hi = pm->offsets[at + 1];
	return true;
}

bool
sift_spm_slice(const SiftScopePairMap *pm, int64 member, const uint32 **ords, uint32 *n)
{
	uint32		at;

	if (pm->probe != NULL)
	{
		*n = probe_ords(pm, member);
		*ords = pm->probe->ords;
		return *n > 0;
	}
	if (pm->base != NULL && bsearch(&member, pm->tomb_members, pm->ntomb, sizeof(int64), sift_cmp_i64) == NULL)
		return sift_spm_slice(pm->base, member, ords, n);
	at = member_lower_bound(pm, 0, member);
	if (at >= pm->nmembers || pm->member_ids[at] != member)
		return false;
	*ords = pm->key_ordinals + pm->offsets[at];
	*n = pm->offsets[at + 1] - pm->offsets[at];
	return true;
}

/* A member's keys are ascending by key (the ordinals of a plain map, the order of now of a patch): a short slice is read through, a long one is searched */
bool
sift_spm_owns_key(const SiftScopePairMap *pm, int64 member, int64 key)
{
	const uint32 *ords;
	uint32		n,
				lo = 0;

	if (!sift_spm_slice(pm, member, &ords, &n))
		return false;
	while (n > 8 && lo < n)
	{
		uint32		mid = lo + (n - lo) / 2;
		int64		k = sift_spm_key_of(pm, ords[mid]);

		if (k == key)
			return true;
		if (k < key)
			lo = mid + 1;
		else
			n = mid;
	}
	for (; lo < n; lo++)
		if (sift_spm_key_of(pm, ords[lo]) == key)
			return true;
	return false;
}

bool
sift_spm_range_hits(const SiftScopePairMap *pm, int64 lo, int64 hi)
{
	uint32		at;

	if (pm->probe != NULL)
		return pm->probe->range_hits(pm->probe->arg, lo, hi);
	at = member_lower_bound(pm, 0, lo);
	return at < pm->nmembers && pm->member_ids[at] <= hi;
}

bool
sift_spm_first_multi(const SiftScopePairMap *pm, int64 *member)
{
	if (pm->probe != NULL)
	{
		*member = pm->probe->first_multi;
		return pm->probe->has_multi;
	}
	for (uint32 i = 0; i < pm->nmembers; i++)
		if (pm->offsets[i + 1] - pm->offsets[i] > 1)
		{
			*member = pm->member_ids[i];
			return true;
		}
	return false;
}

int64
sift_spm_primary_key(SiftScopePairMap *pm, int64 member, bool *multi)
{
	const uint32 *ords;
	uint32		n;
	bool		found = sift_spm_slice(pm, member, &ords, &n) && n > 0;

	if (multi != NULL)
		*multi = found && n > 1;
	return found ? sift_spm_key_of(pm, ords[0]) : SIFT_SCOPE_UNASSIGNED_KEY;
}

uint32
sift_spm_single_pct(const SiftScopePairMap *pm)
{
	uint64		single = 0;

	if (pm->probe != NULL)
		single = pm->probe->nsingle;
	else
		for (uint32 i = 0; i < pm->nmembers; i++)
			single += pm->offsets[i + 1] - pm->offsets[i] == 1;
	return pm->nmembers == 0 ? 100 : (uint32) (single * 100 / pm->nmembers);
}

uint32
sift_spm_cluster_flags(const SiftScopePairMap *pm)
{
	int64		member;

	return sift_spm_first_multi(pm, &member) ? SIFT_SMETA_CLUSTERED_PP : SIFT_SMETA_CLUSTERED_11;
}

/* A member of the pairs has a key twice: they were built as a multiset */
static bool
pairs_repeat(const SiftScopePairMap *pm)
{
	if (pm->probe != NULL)
		return pm->probe->repeat;
	for (uint32 i = 0; i < pm->nmembers; i++)
		for (uint32 q = pm->offsets[i] + 1; q < pm->offsets[i + 1]; q++)
			if (pm->key_ordinals[q] == pm->key_ordinals[q - 1])
				return true;
	return false;
}

uint32
sift_spm_cluster_auto(const SiftScopePairMap *pm)
{
	return pm->nmembers > 0 && sift_spm_single_pct(pm) >= SIFT_SCOPE_PP_AUTO_PCT && !pairs_repeat(pm) ? sift_spm_cluster_flags(pm) : 0;
}

static void
diff_add(SiftScopeMemberKey **pairs, uint32 *n, uint32 *cap, int64 member, int64 key)
{
	if (*n == *cap)
	{
		*cap = *cap == 0 ? 16 : *cap * 2;
		*pairs = *pairs == NULL ? palloc(*cap * sizeof(**pairs)) : repalloc(*pairs, *cap * sizeof(**pairs));
	}
	(*pairs)[(*n)++] = (SiftScopeMemberKey) {member, key};
}

/* The keys of member ia of a that b holds a different number of times (ib < 0: b does not have the member), each once: one pass over the two ascending slices, a run of a key at a time */
static void
diff_member(const SiftScopePairMap *a, uint32 ia, const SiftScopePairMap *b, int64 ib, SiftScopeMemberKey **pairs, uint32 *n, uint32 *cap)
{
	uint32		pa = a->offsets[ia],
				ea = a->offsets[ia + 1],
				pb = ib < 0 ? 0 : b->offsets[ib],
				eb = ib < 0 ? 0 : b->offsets[ib + 1];

	while (pa < ea || pb < eb)
	{
		int64		ka = pa < ea ? a->scope_keys[a->key_ordinals[pa]] : 0,
					kb = pb < eb ? b->scope_keys[b->key_ordinals[pb]] : 0,
					key = pb >= eb || (pa < ea && ka <= kb) ? ka : kb;
		uint32		ca = 0,
					cb = 0;

		for (; pa < ea && a->scope_keys[a->key_ordinals[pa]] == key; pa++)
			ca++;
		for (; pb < eb && b->scope_keys[b->key_ordinals[pb]] == key; pb++)
			cb++;
		if (ca != cb)
			diff_add(pairs, n, cap, a->member_ids[ia], key);
	}
}

static int
pair_by_key(const void *a, const void *b)
{
	const SiftScopeMemberKey *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->member < y->member ? -1 : x->member > y->member;
}

/* The (member, key) pairs held a different number of times by the two maps, ordered by key then member, each once */
static uint32
diff_pairs(const SiftScopePairMap *a, const SiftScopePairMap *b, SiftScopeMemberKey **pairs)
{
	uint32		i = 0,
				j = 0,
				n = 0,
				cap = 0,
				u = 0;

	*pairs = NULL;
	while (i < a->nmembers || j < b->nmembers)
	{
		if (j == b->nmembers || (i < a->nmembers && a->member_ids[i] < b->member_ids[j]))
			diff_member(a, i++, b, -1, pairs, &n, &cap);
		else if (i == a->nmembers || b->member_ids[j] < a->member_ids[i])
			diff_member(b, j++, a, -1, pairs, &n, &cap);
		else
		{
			diff_member(a, i, b, j, pairs, &n, &cap);
			diff_member(b, j, a, i, pairs, &n, &cap);
			i++;
			j++;
		}
	}
	if (n > 0)
		qsort(*pairs, n, sizeof(**pairs), pair_by_key);
	for (uint32 k = 0; k < n; k++)
		if (u == 0 || pair_by_key(&(*pairs)[u - 1], &(*pairs)[k]) != 0)
			(*pairs)[u++] = (*pairs)[k];
	return u;
}

uint32
sift_spm_diff_keys(const SiftScopePairMap *a, const SiftScopePairMap *b, int64 **keys)
{
	SiftScopeMemberKey *pairs;
	uint32		n = diff_pairs(a, b, &pairs),
				u = 0;

	*keys = palloc(sizeof(int64) * Max(n, 1));
	for (uint32 i = 0; i < n; i++)
		if (u == 0 || (*keys)[u - 1] != pairs[i].key)
			(*keys)[u++] = pairs[i].key;
	return u;
}

uint32
sift_spm_diff(const SiftScopePairMap *a, const SiftScopePairMap *b, SiftScopeDirty **dirty)
{
	SiftScopeMemberKey *pairs;
	uint32		n = diff_pairs(a, b, &pairs),
				nd = 0;
	int64	   *members = palloc(sizeof(int64) * Max(n, 1));

	*dirty = palloc(sizeof(SiftScopeDirty) * Max(n, 1));
	for (uint32 i = 0; i < n; i++)
	{
		members[i] = pairs[i].member;
		if (nd == 0 || (*dirty)[nd - 1].key != pairs[i].key)
			(*dirty)[nd++] = (SiftScopeDirty) {pairs[i].key, members + i, 0};
		(*dirty)[nd - 1].nmembers++;
	}
	return nd;
}
