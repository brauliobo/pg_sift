/*
 * col/f2/f2_tier_check.c - sift.check of the parts that live in object storage: each has its object in the store, with the header of a .siftpart of this version that names the part, the size the
 * tier directory says and the SHA-256 and CRC32C it says. An object that is gone or is not the one the directory names is damage of the relation, as a missing page would be; a store that cannot
 * be reached says nothing about the relation and is an ERROR of the check.
 */
#include "postgres.h"

#include "utils/rel.h"

#include "col/f2/f2_tier_host.h"
#include "col/f2/part_format.h"
#include "core/page_io.h"

static bool
is_damage(SiftTierStatus st)
{
	return st == SIFT_TIER_E_NOT_FOUND || st == SIFT_TIER_E_MISMATCH || st == SIFT_TIER_E_CORRUPT;
}

static void
check_part(Relation rel, const SiftPartDesc *d, const SiftTierPartRef *ref)
{
	SiftTierPolicy policy;
	SiftTierErr err = {0};
	SiftTierStatus st;
	char		key[160];

	if (ref == NULL)
		SIFT_CORRUPT(rel, "Part %llu is in object storage and the tier directory has no ref for it.", (unsigned long long) d->part_gen);
	sift_tier_policy_by_id(rel, ref->policy_id, &policy);
	sift_tier_object_key(key, sizeof(key), policy.scope_id, ref->part_id, ref->part_gen);
	st = sift_tier_check_object(sift_tier_backend(&policy), key, ref, &err);
	if (is_damage(st))
		SIFT_CORRUPT(rel, "Part %llu: its object in the store is not sound: %s.", (unsigned long long) d->part_gen, err.msg);
	if (st != SIFT_TIER_OK)
		sift_tier_raise(st, &err, "cannot check a part in object storage");
}

void
sift_tier_check_remote(Relation rel)
{
	SiftManifestRead all = sift_manifest_read_range(rel, NULL, NULL);
	int			nrefs;
	const SiftTierPartRef *refs = sift_tier_refs(rel, &nrefs);

	for (int i = 0; i < all.n; i++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &all.entries[i], &np);

		for (int p = 0; p < np; p++)
			if (parts[p].ext_flags & SIFT_PART_EXT_REMOTE)
				check_part(rel, &parts[p], refs != NULL ? sift_tier_refs_find(refs, nrefs, parts[p].part_gen) : NULL);
	}
}
