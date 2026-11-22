/*
 * col/scope_v6/pairmap.c - the pairs (member, scope key) of a scope, in memory as a compressed sparse row table: the keys of a member are a
 * slice of one array. A build whose granules arrive in ascending member order finds each member by galloping from the previous one.
 */
/* check-memory: allow-allocs the arrays follow the pairs of the scope relation, which the build holds in memory by design (12 bytes a member, 4 a pair) */
#include "core/host.h"

#include "core/cmp.h"
#include "col/scope_v6/meta.h"

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
	int64	   *keys = palloc(sizeof(int64) * Max(n, 1));
	uint32		nk = 0,
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
	pm->nkeys = nk;
	pm->scope_keys = keys;
	pm->npairs = n;
	pm->member_ids = palloc(sizeof(int64) * Max(n, 1));
	pm->offsets = palloc(sizeof(uint32) * (n + 1));
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

void
sift_spm_free(SiftScopePairMap *pm)
{
	pfree(pm->member_ids);
	pfree(pm->offsets);
	pfree(pm->key_ordinals);
	pfree(pm->scope_keys);
	pfree(pm);
}

int32
sift_spm_key_ordinal(const SiftScopePairMap *pm, int64 scope_key)
{
	uint32		lo = 0,
				hi = pm->nkeys;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (pm->scope_keys[mid] == scope_key)
			return (int32) mid;
		if (pm->scope_keys[mid] < scope_key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return -1;
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

bool
sift_spm_find(SiftScopePairMap *pm, int64 member, uint32 *lo, uint32 *hi)
{
	uint32		at;

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
sift_spm_range_hits(const SiftScopePairMap *pm, int64 lo, int64 hi)
{
	uint32		at = member_lower_bound(pm, 0, lo);

	return at < pm->nmembers && pm->member_ids[at] <= hi;
}

bool
sift_spm_first_multi(const SiftScopePairMap *pm, int64 *member)
{
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
	uint32		lo,
				hi;
	bool		found = sift_spm_find(pm, member, &lo, &hi) && lo < hi;

	if (multi != NULL)
		*multi = found && hi - lo > 1;
	return found ? pm->scope_keys[pm->key_ordinals[lo]] : SIFT_SCOPE_UNASSIGNED_KEY;
}

uint32
sift_spm_single_pct(const SiftScopePairMap *pm)
{
	uint64		single = 0;

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

static uint32
key_count(const SiftScopePairMap *pm, uint32 at, int64 key)
{
	uint32		n = 0;

	for (uint32 i = pm->offsets[at]; i < pm->offsets[at + 1]; i++)
		n += pm->scope_keys[pm->key_ordinals[i]] == key;
	return n;
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

/* The pairs of member at in pm that are not held the same number of times by member bt in other (bt < 0: the member is not there) */
static void
diff_member(const SiftScopePairMap *pm, uint32 at, const SiftScopePairMap *other, int64 bt, SiftScopeMemberKey **pairs, uint32 *n, uint32 *cap)
{
	for (uint32 i = pm->offsets[at]; i < pm->offsets[at + 1]; i++)
	{
		int64		key = pm->scope_keys[pm->key_ordinals[i]];

		if (bt < 0 || key_count(pm, at, key) != key_count(other, (uint32) bt, key))
			diff_add(pairs, n, cap, pm->member_ids[at], key);
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
