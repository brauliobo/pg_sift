/*
 * col/scope_v6/probe.h - the pairs of a scope that are not held: a member's keys are asked of the source of the pairs when a build meets the member, so that what the map holds is the keys
 * of the scope and not its pairs (sift_spm_probed). What the map answers from its tables for a held map it answers here from the totals the source counted when it made the keys: reads of
 * one member, the range of the members, the clustering. A map that is probed is for a build; a refresh, a diff or a patch (sift_spm_patched) reads the tables of a held one.
 */
#ifndef SIFT_COL_SCOPE_V6_PROBE_H
#define SIFT_COL_SCOPE_V6_PROBE_H

#include "core/host.h"

#include "col/scope_v6/meta.h"

struct SiftScopePairProbe
{
	void	   *arg;
	/* the keys of the member (ascending, a key twice only for a multiset) in *keys, the callee's buffer valid until the next call */
	uint32		(*member_keys) (void *arg, int64 member, const int64 **keys);
	/* some pair has a member in [lo, hi] (a pair that has no key may count: the caller then finds none) */
	bool		(*range_hits) (void *arg, int64 lo, int64 hi);
	void		(*close) (void *arg);
	uint32		nsingle;		/* members that have one key */
	bool		repeat;			/* a member has a key twice */
	bool		has_multi;		/* some member has more than one key; first_multi is the smallest */
	int64		first_multi;
	uint32	   *ords;			/* the ordinals of the keys of the member asked last */
	uint32		cap_ords;
};

/* The map of a scope whose pairs are probed: keys ascending and distinct (taken), nmembers and npairs the totals of the pairs, the probe (its callbacks, its totals) as the source made it */
extern SiftScopePairMap *sift_spm_probed(int64 *keys, uint32 nkeys, uint32 nmembers, uint32 npairs, SiftScopePairProbe *probe);

#endif
