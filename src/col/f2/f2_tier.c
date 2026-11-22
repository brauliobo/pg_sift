/*
 * col/f2/f2_tier.c - what the host does with a part and an object store (col/f2/f2_tier.h): the upload of a part with its checks, the check of a stored object against its ref, the open and the
 * reads of a part that lives in the store, and the removal of the objects that no directory names.
 */
#include "core/host.h"

#include <ctype.h>

#include "miscadmin.h"
#include "port/pg_bitutils.h"

#include "col/f2/f2_tier_int.h"
#include "col/f2/part_format.h"
#include "core/hash.h"

bool
sift_tier_part_eligible(const SiftPartDesc *d, int slot, int64 cutoff)
{
	return d->level == 2 && !(d->ext_flags & (SIFT_PART_EXT_REMOTE | SIFT_PART_EXT_EXPIRED)) &&
		(d->flags & SIFT_PART_FLAG_ALL_VISIBLE) && !(d->flags & (SIFT_PART_FLAG_UNFROZEN | SIFT_PART_FLAG_PACKED_SLOT)) &&
		d->dv_blk == InvalidBlockNumber && d->col_delta_blk == InvalidBlockNumber && d->header_blk != InvalidBlockNumber &&
		(d->flags & (slot == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1)) && d->time_max[slot] < cutoff;
}

static void
ref_hash(const uint8 sha[SIFT_SHA256_LEN], uint64 hash[2])
{
	memcpy(hash, sha, sizeof(uint64) * 2);
}

/* The bytes at [off, off + n) of the object as put compared with the ones the store returns */
static SiftTierStatus
same_bytes(SiftTierBackend *be, const char *key, const SiftTierSrc *src, uint64 off, uint64 n, SiftTierErr *err)
{
	char	   *want = palloc(n),
			   *got = palloc(n);
	SiftTierStatus st;

	src->read(src->arg, off, n, want);
	st = be->ops->get(be, key, off, n, got, err);
	if (st == SIFT_TIER_OK && memcmp(want, got, n) != 0)
		st = sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "the store returned other bytes than were put at offset %llu", (unsigned long long) off);
	pfree(want);
	pfree(got);
	return st;
}

/* The whole object, hashed as the store returns it */
static SiftTierStatus
digest_stored(SiftTierBackend *be, const char *key, uint64 len, uint32 *crc, uint8 sha[SIFT_SHA256_LEN], SiftTierErr *err)
{
	char	   *buf = palloc(SIFT_TIER_CHUNK);
	SiftSha256	c;
	SiftTierStatus st = SIFT_TIER_OK;

	sift_sha256_init(&c);
	*crc = 0;
	for (uint64 off = 0; off < len && st == SIFT_TIER_OK;)
	{
		uint64		n = Min(SIFT_TIER_CHUNK, len - off);

		CHECK_FOR_INTERRUPTS();
		st = be->ops->get(be, key, off, n, buf, err);
		sift_sha256_update(&c, buf, n);
		*crc = sift_tier_crc32c_extend(*crc, buf, n);
		off += n;
	}
	sift_sha256_final(&c, sha);
	pfree(buf);
	return st;
}

static SiftTierStatus
check_stored(SiftTierBackend *be, const char *key, const SiftTierObj *obj, const SiftTierObjInfo *put_info, uint32 crc, const uint8 sha[SIFT_SHA256_LEN], SiftTierErr *err)
{
	SiftTierObjInfo info;
	SiftTierStatus st = be->ops->head(be, key, &info, err);

	if (st != SIFT_TIER_OK)
		return st;
	if (info.len != obj->src.len || (put_info->etag[0] != '\0' && info.etag[0] != '\0' && strcmp(info.etag, put_info->etag) != 0))
		return sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "the store holds %llu bytes (ETag differs: %s), %llu were put", (unsigned long long) info.len,
							  strcmp(info.etag, put_info->etag) != 0 ? "yes" : "no", (unsigned long long) obj->src.len);
	if ((st = same_bytes(be, key, &obj->src, 0, sizeof(SiftRemotePartHeader), err)) != SIFT_TIER_OK)
		return st;
	if ((st = same_bytes(be, key, &obj->src, obj->hdr.footer_off, obj->tail_len, err)) != SIFT_TIER_OK)
		return st;
	if (be->is_local)
	{
		uint32		got_crc;
		uint8		got_sha[SIFT_SHA256_LEN];

		if ((st = digest_stored(be, key, obj->src.len, &got_crc, got_sha, err)) != SIFT_TIER_OK)
			return st;
		if (got_crc != crc || memcmp(got_sha, sha, SIFT_SHA256_LEN) != 0)
			return sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "the object read back does not have the digest of the object put");
	}
	return SIFT_TIER_OK;
}

SiftTierStatus
sift_tier_upload(SiftTierBackend *be, const char *key, const SiftTierPartSrc *part, uint32 policy_id, TimestampTz now, SiftTierPartRef *ref, SiftTierErr *err)
{
	SiftTierObj obj;
	SiftTierObjInfo info;
	uint32		crc;
	uint8		sha[SIFT_SHA256_LEN];
	char		hex[SIFT_SHA256_HEX];
	SiftTierStatus st;

	if (!sift_tier_key_ok(key))
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);
	if ((st = sift_tier_obj_build(&obj, part, err)) != SIFT_TIER_OK)
		return st;
	sift_tier_src_digest(&obj.src, &crc, sha);
	sift_hex(sha, sizeof(sha), hex);
	st = be->ops->put(be, key, &obj.src, hex, &info, err);
	if (st == SIFT_TIER_OK)
		st = check_stored(be, key, &obj, &info, crc, sha, err);
	if (st == SIFT_TIER_OK)
	{
		memset(ref, 0, sizeof(*ref));
		ref->part_gen = part->part_gen;
		ref->part_id = part->part_id;
		ref->flags = SIFT_TIER_REMOTE_ONLY;
		ref->byte_len = obj.src.len;
		ref->header_len = part->image_len;
		ref->stream_crc32c = crc;
		ref_hash(sha, ref->obj_hash);
		ref->policy_id = policy_id;
		ref->tiered_at = now;
		memcpy(ref->etag, info.etag, Min(strlen(info.etag), sizeof(ref->etag)));
	}
	sift_tier_obj_free(&obj);
	return st;
}

SiftTierStatus
sift_tier_verify_object(SiftTierBackend *be, const char *key, const SiftTierPartRef *ref, SiftTierErr *err)
{
	SiftTierObjInfo info;
	uint32		crc;
	uint8		sha[SIFT_SHA256_LEN];
	uint64		hash[2];
	SiftTierStatus st = be->ops->head(be, key, &info, err);

	if (st != SIFT_TIER_OK)
		return st;
	if (info.len != ref->byte_len)
		return sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "object %s holds %llu bytes, the directory says %llu", key, (unsigned long long) info.len, (unsigned long long) ref->byte_len);
	if ((st = digest_stored(be, key, info.len, &crc, sha, err)) != SIFT_TIER_OK)
		return st;
	ref_hash(sha, hash);
	if (crc != ref->stream_crc32c || hash[0] != ref->obj_hash[0] || hash[1] != ref->obj_hash[1])
		return sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "object %s is not the object the directory names (checksum or SHA-256 differs)", key);
	return SIFT_TIER_OK;
}

/* The header of an object read from the store is the one of the part the directory names for that key */
static SiftTierStatus
header_names_ref(const SiftRemotePartHeader *hdr, const SiftTierPartRef *ref, const char *key, SiftTierErr *err)
{
	if (hdr->total_len != ref->byte_len || hdr->header_len != ref->header_len || hdr->part_gen != ref->part_gen || hdr->part_id != ref->part_id)
		return sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "object %s is not part %llu of the directory", key, (unsigned long long) ref->part_gen);
	return SIFT_TIER_OK;
}

SiftTierStatus
sift_tier_check_object(SiftTierBackend *be, const char *key, const SiftTierPartRef *ref, SiftTierErr *err)
{
	uint8		first[sizeof(SiftRemotePartHeader)];
	SiftRemotePartHeader hdr;
	SiftTierObjInfo info;
	SiftTierStatus st = be->ops->head(be, key, &info, err);

	if (st != SIFT_TIER_OK)
		return st;
	if (info.len < sizeof(first))
		return sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "object %s holds %llu bytes, fewer than its header", key, (unsigned long long) info.len);
	if ((st = be->ops->get(be, key, 0, sizeof(first), first, err)) != SIFT_TIER_OK || (st = sift_tier_obj_parse_header(first, &hdr, err)) != SIFT_TIER_OK ||
		(st = header_names_ref(&hdr, ref, key, err)) != SIFT_TIER_OK)
		return st;
	return sift_tier_verify_object(be, key, ref, err);
}

static SiftTierStatus
remote_load(SiftTierRemote *r, SiftTierErr *err)
{
	uint8		first[sizeof(SiftRemotePartHeader)];
	SiftTierStatus st;
	char	   *tail;

	if (r->ref.byte_len < sizeof(first))
		return sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "the directory names an object shorter than its header");
	if ((st = sift_tier_cache_read(r->cache, r->be, r->key, &r->ref, 0, sizeof(first), first, err)) != SIFT_TIER_OK ||
		(st = sift_tier_obj_parse_header(first, &r->hdr, err)) != SIFT_TIER_OK || (st = header_names_ref(&r->hdr, &r->ref, r->key, err)) != SIFT_TIER_OK)
		return st;
	r->image = palloc(r->hdr.header_len);
	if ((st = sift_tier_cache_read(r->cache, r->be, r->key, &r->ref, sizeof(first), r->hdr.header_len, r->image, err)) != SIFT_TIER_OK)
		return st;
	if (sift_tier_crc32c_extend(0, r->image, r->hdr.header_len) != r->hdr.image_crc32c)
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "the header image of object %s has the wrong checksum", r->key);
	tail = palloc(r->hdr.total_len - r->hdr.footer_off);
	st = sift_tier_cache_read(r->cache, r->be, r->key, &r->ref, r->hdr.footer_off, r->hdr.total_len - r->hdr.footer_off, tail, err);
	if (st == SIFT_TIER_OK)
		st = sift_tier_obj_parse_tail(&r->hdr, tail, &r->ranges, err);
	pfree(tail);
	return st;
}

SiftTierStatus
sift_tier_remote_open(SiftTierBackend *be, SiftTierCache *cache, const char *key, const SiftTierPartRef *ref, SiftTierRemote **out, SiftTierErr *err)
{
	SiftTierRemote *r = palloc0(sizeof(SiftTierRemote));
	SiftTierStatus st;

	*out = NULL;
	if (!sift_tier_key_ok(key))
	{
		pfree(r);
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);
	}
	r->be = be;
	r->cache = cache;
	snprintf(r->key, sizeof(r->key), "%s", key);
	r->ref = *ref;
	st = remote_load(r, err);
	if (st != SIFT_TIER_OK)
	{
		sift_tier_remote_close(r);
		return st;
	}
	*out = r;
	return SIFT_TIER_OK;
}

void
sift_tier_remote_close(SiftTierRemote *r)
{
	if (r->image)
		pfree(r->image);
	if (r->ranges)
		pfree(r->ranges);
	pfree(r);
}

SiftTierStatus
sift_tier_remote_read(SiftTierRemote *r, uint64 off, uint64 len, void *dst, SiftTierErr *err)
{
	if (off + len > r->hdr.stream_len || off + len < off)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "the range [%llu, +%llu) is outside the stream of %llu bytes", (unsigned long long) off, (unsigned long long) len,
							  (unsigned long long) r->hdr.stream_len);
	return sift_tier_cache_read(r->cache, r->be, r->key, &r->ref, sizeof(SiftRemotePartHeader) + r->hdr.header_len + off, len, dst, err);
}

SiftTierStatus
sift_tier_remote_read_range(SiftTierRemote *r, uint32 range_idx, void *dst, SiftTierErr *err)
{
	const SiftRemoteRange *g;
	SiftTierStatus st;

	if (range_idx >= r->hdr.nranges)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "the object has %u ranges, not %u", r->hdr.nranges, range_idx + 1);
	g = &r->ranges[range_idx];
	if ((st = sift_tier_remote_read(r, g->off, g->len, dst, err)) != SIFT_TIER_OK)
		return st;
	if (sift_tier_crc32c_extend(0, dst, g->len) != g->crc32c)
	{
		sift_tier_cache_forget(r->cache, &r->ref, sizeof(SiftRemotePartHeader) + r->hdr.header_len + g->off, g->len);
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "range %u of object %s has the wrong checksum", range_idx, r->key);
	}
	return SIFT_TIER_OK;
}

typedef struct KeyList
{
	char	  **keys;
	int			n;
	int			cap;
} KeyList;

static void
keylist_add(void *arg, const char *key)
{
	KeyList    *l = arg;

	if (l->n == l->cap)
	{
		l->cap = l->cap ? l->cap * 2 : 64;
		l->keys = l->keys ? repalloc(l->keys, l->cap * sizeof(char *)) : palloc(l->cap * sizeof(char *));
	}
	l->keys[l->n] = palloc(strlen(key) + 1);
	strcpy(l->keys[l->n++], key);
}

/* The parts the directory names, by (partition, generation): the keys of the objects are made from the two */
typedef struct RefSlot
{
	uint64		part_gen;
	uint32		part_id;
	bool		used;
} RefSlot;

typedef struct RefSet
{
	RefSlot    *slots;
	uint32		mask;
} RefSet;

static uint32
ref_home(const RefSet *set, uint32 part_id, uint64 part_gen)
{
	return (uint32) (sift_fmix64(((uint64) part_id << 32) ^ part_gen) & set->mask);
}

static void
ref_set_build(RefSet *set, const SiftTierPartRef *refs, int nrefs)
{
	set->mask = pg_nextpower2_32(Max(2 * (uint32) nrefs, 16)) - 1;
	set->slots = palloc0((set->mask + 1) * sizeof(RefSlot));
	for (int i = 0; i < nrefs; i++)
	{
		uint32		h = ref_home(set, refs[i].part_id, refs[i].part_gen);

		while (set->slots[h].used)
			h = (h + 1) & set->mask;
		set->slots[h] = (RefSlot) {refs[i].part_gen, refs[i].part_id, true};
	}
}

static bool
ref_set_has(const RefSet *set, uint32 part_id, uint64 part_gen)
{
	for (uint32 h = ref_home(set, part_id, part_gen); set->slots[h].used; h = (h + 1) & set->mask)
		if (set->slots[h].part_id == part_id && set->slots[h].part_gen == part_gen)
			return true;
	return false;
}

/* The object a key names of a scope: the key sift_tier_object_key makes of that partition and generation, and no other spelling of them */
static bool
key_names_part(const RefSet *set, uint64 scope_id, const char *key)
{
	char		canon[160];
	char	   *end;
	unsigned long long id,
				gen;
	const char *at = key + 18;	/* past "r" and the 16 hex digits of the scope and "/" */

	if (strlen(key) < 18 || at[0] != 'p' || !isdigit((unsigned char) at[1]))
		return false;
	id = strtoull(at + 1, &end, 10);
	if (id > PG_UINT32_MAX || end[0] != '-' || end[1] != 'g' || !isdigit((unsigned char) end[2]))
		return false;
	gen = strtoull(end + 2, &end, 10);
	return sift_tier_object_key(canon, sizeof(canon), scope_id, (uint32) id, gen) > 0 && strcmp(canon, key) == 0 && ref_set_has(set, (uint32) id, gen);
}

SiftTierStatus
sift_tier_sweep_orphans(SiftTierBackend *be, uint64 scope_id, const SiftTierPartRef *refs, int nrefs, int *ndeleted, SiftTierErr *err)
{
	char		prefix[32];
	KeyList		l = {0};
	SiftTierStatus st;
	RefSet		named;

	*ndeleted = 0;
	snprintf(prefix, sizeof(prefix), "r%016llx/", (unsigned long long) scope_id);
	ref_set_build(&named, refs, nrefs);
	st = be->ops->list(be, prefix, keylist_add, &l, err);
	for (int i = 0; i < l.n && st == SIFT_TIER_OK; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (!key_names_part(&named, scope_id, l.keys[i]) && (st = be->ops->del(be, l.keys[i], err)) == SIFT_TIER_OK)
			(*ndeleted)++;
	}
	pfree(named.slots);
	for (int i = 0; i < l.n; i++)
		pfree(l.keys[i]);
	if (l.keys)
		pfree(l.keys);
	return st;
}
