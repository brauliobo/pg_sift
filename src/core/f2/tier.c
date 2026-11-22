/*
 * core/f2/tier.c - the page of the tier directory (core/f2/tier.h) and the edit of its refs.
 */
#include "core/host.h"

#include "core/f2/tier.h"

#define TDIR(p)		((SiftTierDirPageData *) SiftPageGetData(p))

void
sift_tier_page_fill(Page page, uint64 gen, uint64 manifest_version, BlockNumber next_blk, const SiftTierPartRef *refs, int n)
{
	SiftTierDirPageData *d;

	Assert(n >= 0 && n <= (int) SIFT_TIERDIR_CAP);
	sift_page_init_part(page, SIFT_PT_TIERDIR, gen, 0);
	d = TDIR(page);
	d->magic = SIFT_TIERDIR_MAGIC;
	d->nentries = n;
	d->version = SIFT_TIERDIR_VERSION;
	d->next_blk = next_blk;
	d->manifest_version = manifest_version;
	d->tier_dir_gen = gen;
	memcpy(d->refs, refs, n * sizeof(SiftTierPartRef));
	SiftPageSetUsed(page, offsetof(SiftTierDirPageData, refs) + n * sizeof(SiftTierPartRef));
	sift_page_seal(page);
}

const char *
sift_tier_page_check(const Page page, uint64 gen)
{
	const SiftTierDirPageData *d = (const SiftTierDirPageData *) SiftPageGetData(page);

	if (SiftPageGetUsed(page) < offsetof(SiftTierDirPageData, refs))
		return "the tier directory page is shorter than its header";
	if (d->magic != SIFT_TIERDIR_MAGIC)
		return "the tier directory page has the wrong magic";
	if (d->version != SIFT_TIERDIR_VERSION)
		return "the tier directory page has a version this build does not read";
	if (!sift_page_sum_ok(page))
		return "the tier directory page has the wrong checksum";
	if (d->tier_dir_gen != gen)
		return "the tier directory page belongs to another generation of the chain";
	if (d->nentries == 0 || d->nentries > SIFT_TIERDIR_CAP ||
		SiftPageGetUsed(page) != offsetof(SiftTierDirPageData, refs) + d->nentries * sizeof(SiftTierPartRef))
		return "the tier directory page holds a count of refs that its bytes do not";
	return NULL;
}

static int
ref_cmp_gen(const void *key, const void *elem)
{
	uint64		g = *(const uint64 *) key,
				h = ((const SiftTierPartRef *) elem)->part_gen;

	return g < h ? -1 : g > h;
}

const SiftTierPartRef *
sift_tier_refs_find(const SiftTierPartRef *refs, int n, uint64 part_gen)
{
	return n > 0 ? bsearch(&part_gen, refs, n, sizeof(SiftTierPartRef), ref_cmp_gen) : NULL;
}

static int
ref_cmp(const void *a, const void *b)
{
	uint64		x = ((const SiftTierPartRef *) a)->part_gen,
				y = ((const SiftTierPartRef *) b)->part_gen;

	return x < y ? -1 : x > y;
}

int
sift_tier_refs_apply(SiftTierPartRef *dst, const SiftTierPartRef *refs, int n, const SiftTierPartRef *set, int nset, const uint64 *drop_gens, int ndrop)
{
	int			out = 0;

	for (int i = 0; i < n; i++)
	{
		bool		dropped = false;

		for (int k = 0; k < ndrop; k++)
			dropped |= drop_gens[k] == refs[i].part_gen;
		if (!dropped)
			dst[out++] = refs[i];
	}
	if (out != n - ndrop)
		return -1;
	for (int i = 0; i < nset; i++)
	{
		const SiftTierPartRef *at = sift_tier_refs_find(dst, out, set[i].part_gen);

		for (int k = 0; k < i; k++)
			if (set[k].part_gen == set[i].part_gen)
				return -1;
		if (at != NULL)
			dst[at - dst] = set[i];
		else
			dst[out++] = set[i];
	}
	qsort(dst, out, sizeof(SiftTierPartRef), ref_cmp);
	return out;
}

const char *
sift_tier_refs_check(const SiftTierPartRef *refs, int n)
{
	const uint32 known = SIFT_TIER_REMOTE_ONLY | SIFT_TIER_LOCAL_CACHE | SIFT_TIER_DELETE_PENDING;

	for (int i = 0; i < n; i++)
	{
		if (i > 0 && refs[i - 1].part_gen >= refs[i].part_gen)
			return "the refs of the tier directory are not in ascending part_gen";
		if (refs[i].part_gen == 0 || refs[i].byte_len == 0 || refs[i].header_len == 0)
			return "a ref of the tier directory has no generation, length or header";
		if (refs[i].flags & ~known)
			return "a ref of the tier directory has a flag this build does not know";
		if (((refs[i].flags & SIFT_TIER_REMOTE_ONLY) != 0) == ((refs[i].flags & SIFT_TIER_LOCAL_CACHE) != 0))
			return "a ref of the tier directory is neither remote only nor a local cache copy";
	}
	return NULL;
}
