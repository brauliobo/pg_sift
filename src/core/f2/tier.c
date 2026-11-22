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

static int
gen_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/*
 * The refs after the edit, ascending by part_gen in dst: refs (ascending) without the dropped parts, the ones in set put in their place or added. -1 when a drop is not in refs or is named twice,
 * or a part is set twice. The drops and the set are sorted once and the three lists merged in one pass; a part both dropped and set is added again (the drop applies first).
 */
int
sift_tier_refs_apply(SiftTierPartRef *dst, const SiftTierPartRef *refs, int n, const SiftTierPartRef *set, int nset, const uint64 *drop_gens, int ndrop)
{
	uint64		dstack[64];
	SiftTierPartRef sstack[32];
	uint64	   *d = ndrop <= 64 ? dstack : palloc(ndrop * sizeof(uint64));	/* check-memory: allow the drops of one tier edit */
	SiftTierPartRef *s = nset <= 32 ? sstack : palloc(nset * sizeof(SiftTierPartRef));	/* check-memory: allow the refs one tier edit sets */
	int			ci = 0,
				di = 0,
				si = 0,
				out = 0;
	bool		ok = true;

	if (ndrop > 0)
		memcpy(d, drop_gens, ndrop * sizeof(uint64));
	if (nset > 0)
		memcpy(s, set, nset * sizeof(SiftTierPartRef));
	qsort(d, ndrop, sizeof(uint64), gen_cmp);
	qsort(s, nset, sizeof(SiftTierPartRef), ref_cmp);
	for (int i = 1; i < ndrop && ok; i++)
		ok = d[i - 1] != d[i];
	for (int i = 1; i < nset && ok; i++)
		ok = s[i - 1].part_gen != s[i].part_gen;
	while (ok && (ci < n || si < nset))
	{
		while (ok && ci < n && di < ndrop && d[di] <= refs[ci].part_gen)
		{
			ok = d[di] == refs[ci].part_gen;
			ci += ok;
			di++;
		}
		if (!ok || (ci == n && si == nset))
			break;
		if (si == nset || (ci < n && refs[ci].part_gen < s[si].part_gen))
			dst[out++] = refs[ci++];
		else
		{
			ci += ci < n && refs[ci].part_gen == s[si].part_gen;
			dst[out++] = s[si++];
		}
	}
	ok = ok && di == ndrop;
	if (d != dstack)
		pfree(d);
	if (s != sstack)
		pfree(s);
	return ok ? out : -1;
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
