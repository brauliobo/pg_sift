/*
 * col/f2/tier/tier.c - what the host does with a part and an object store (col/f2/tier/tier.h): the upload of a part with its checks, the check of a stored object against its ref, the open and the
 * reads of a part that lives in the store, and the removal of the objects that no directory names.
 */
#include "core/host.h"

#include <ctype.h>

#include "miscadmin.h"
#include "port/pg_bitutils.h"

#include "col/f2/tier/int.h"
#include "col/f2/part/format.h"
#include "core/hash.h"

bool
sift_tier_part_eligible(const SiftPartDesc *d, int slot, int64 cutoff)
{
	return d->level == 2 && !(d->ext_flags & (SIFT_PART_EXT_REMOTE | SIFT_PART_EXT_EXPIRED)) &&
		(d->flags & SIFT_PART_FLAG_ALL_VISIBLE) && !(d->flags & (SIFT_PART_FLAG_UNFROZEN | SIFT_PART_FLAG_PACKED_SLOT)) &&
		d->dv_blk == InvalidBlockNumber && d->header_blk != InvalidBlockNumber &&
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
remote_load(const SiftTierVia *via, SiftTierRemote *r, SiftTierErr *err)
{
	uint8		first[sizeof(SiftRemotePartHeader)];
	SiftTierStatus st;
	char	   *tail;

	if (r->ref.byte_len < sizeof(first))
		return sift_tier_fail(err, SIFT_TIER_E_MISMATCH, "the directory names an object shorter than its header");
	if ((st = sift_tier_cache_read(via->cache, via->be, r->key, &r->ref, 0, sizeof(first), first, err)) != SIFT_TIER_OK ||
		(st = sift_tier_obj_parse_header(first, &r->hdr, err)) != SIFT_TIER_OK || (st = header_names_ref(&r->hdr, &r->ref, r->key, err)) != SIFT_TIER_OK)
		return st;
	r->image = palloc(r->hdr.header_len);
	if ((st = sift_tier_cache_read(via->cache, via->be, r->key, &r->ref, sizeof(first), r->hdr.header_len, r->image, err)) != SIFT_TIER_OK)
		return st;
	if (sift_tier_crc32c_extend(0, r->image, r->hdr.header_len) != r->hdr.image_crc32c)
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "the header image of object %s has the wrong checksum", r->key);
	tail = palloc(r->hdr.total_len - r->hdr.footer_off);
	st = sift_tier_cache_read(via->cache, via->be, r->key, &r->ref, r->hdr.footer_off, r->hdr.total_len - r->hdr.footer_off, tail, err);
	if (st == SIFT_TIER_OK)
		st = sift_tier_obj_parse_tail(&r->hdr, tail, &r->ranges, err);
	if (st == SIFT_TIER_OK)
		r->verified = palloc0(Max((r->hdr.nranges + 7) / 8, 1));
	pfree(tail);
	return st;
}

SiftTierStatus
sift_tier_remote_open(const SiftTierVia *via, const char *key, const SiftTierPartRef *ref, SiftTierRemote **out, SiftTierErr *err)
{
	SiftTierRemote *r = palloc0(sizeof(SiftTierRemote));
	SiftTierStatus st;

	*out = NULL;
	if (!sift_tier_key_ok(key))
	{
		pfree(r);
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);
	}
	snprintf(r->key, sizeof(r->key), "%s", key);
	r->ref = *ref;
	st = remote_load(via, r, err);
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
	if (r->verified)
		pfree(r->verified);
	pfree(r);
}

static uint64
stream_base(const SiftTierRemote *r)
{
	return sizeof(SiftRemotePartHeader) + r->hdr.header_len;
}

static bool
range_verified(const SiftTierRemote *r, uint32 i)
{
	return (r->verified[i / 8] & (1 << (i % 8))) != 0;
}

/* Range i whole from the cache or the store into buf, matched with its checksum: bytes that do not match leave no chunk behind */
static SiftTierStatus
range_verify(const SiftTierVia *via, SiftTierRemote *r, uint32 i, void *buf, SiftTierErr *err)
{
	const SiftRemoteRange *g = &r->ranges[i];
	SiftTierStatus st = sift_tier_cache_read(via->cache, via->be, r->key, &r->ref, stream_base(r) + g->off, g->len, buf, err);

	if (st != SIFT_TIER_OK)
		return st;
	if (sift_tier_crc32c_extend(0, buf, g->len) != g->crc32c)
	{
		sift_tier_cache_forget(via->cache, &r->ref, stream_base(r) + g->off, g->len);
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "range %u of object %s has the wrong checksum", i, r->key);
	}
	r->verified[i / 8] |= 1 << (i % 8);
	return SIFT_TIER_OK;
}

SiftTierStatus
sift_tier_remote_read(const SiftTierVia *via, SiftTierRemote *r, uint64 off, uint64 len, void *dst, SiftTierErr *err)
{
	uint32		first = 0,
				n;
	uint64		at = off;

	if (off + len > r->hdr.stream_len || off + len < off)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "the range [%llu, +%llu) is outside the stream of %llu bytes", (unsigned long long) off, (unsigned long long) len,
							  (unsigned long long) r->hdr.stream_len);
	n = sift_tier_ranges_cover(r->ranges, r->hdr.nranges, off, len, &first);
	for (uint32 i = first; i < first + n && r->ranges[i].off <= at; i++)
	{
		const SiftRemoteRange *g = &r->ranges[i];
		uint64		to = Min(off + len, g->off + g->len);
		char	   *whole = range_verified(r, i) ? NULL : palloc(g->len);
		SiftTierStatus st = whole != NULL ? range_verify(via, r, i, whole, err) :
		sift_tier_cache_read(via->cache, via->be, r->key, &r->ref, stream_base(r) + at, to - at, (char *) dst + (at - off), err);

		if (st == SIFT_TIER_OK && whole != NULL)
			memcpy((char *) dst + (at - off), whole + (at - g->off), to - at);
		if (whole != NULL)
			pfree(whole);
		if (st != SIFT_TIER_OK)
			return st;
		at = to;
	}
	if (at < off + len)
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "bytes %llu to %llu of the stream of object %s are in no range with a checksum", (unsigned long long) at,
							  (unsigned long long) (off + len), r->key);
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

/* The parts the directory names, by (relfilenumber, partition, generation): the keys of the objects are made from the three */
typedef struct RefSlot
{
	uint64		part_gen;
	uint32		rel_number;
	uint32		part_id;
	bool		used;
} RefSlot;

typedef struct RefSet
{
	RefSlot    *slots;
	uint32		mask;
} RefSet;

static uint32
ref_home(const RefSet *set, uint32 rel_number, uint32 part_id, uint64 part_gen)
{
	return (uint32) (sift_fmix64(sift_fmix64(((uint64) rel_number << 32) | part_id) ^ part_gen) & set->mask);
}

static void
ref_set_build(RefSet *set, const SiftTierPartRef *refs, int nrefs)
{
	set->mask = pg_nextpower2_32(Max(2 * (uint32) nrefs, 16)) - 1;
	set->slots = palloc0((set->mask + 1) * sizeof(RefSlot));
	for (int i = 0; i < nrefs; i++)
	{
		uint32		h = ref_home(set, refs[i].rel_number, refs[i].part_id, refs[i].part_gen);

		while (set->slots[h].used)
			h = (h + 1) & set->mask;
		set->slots[h] = (RefSlot) {refs[i].part_gen, refs[i].rel_number, refs[i].part_id, true};
	}
}

static bool
ref_set_has(const RefSet *set, const SiftTierPartRef *k)
{
	for (uint32 h = ref_home(set, k->rel_number, k->part_id, k->part_gen); set->slots[h].used; h = (h + 1) & set->mask)
		if (set->slots[h].rel_number == k->rel_number && set->slots[h].part_id == k->part_id && set->slots[h].part_gen == k->part_gen)
			return true;
	return false;
}

/* The object a key names of a scope: the key sift_tier_ref_key makes of that relfilenumber, partition and generation, and no other spelling of them (only those three fields of *k are set) */
static bool
key_part(uint64 scope_id, const char *key, SiftTierPartRef *k)
{
	char		canon[160];
	char	   *end;
	unsigned long long rel,
				id,
				gen;
	const char *at = key + 18;	/* past "r" and the 16 hex digits of the scope and "/" */

	if (strlen(key) < 18 || at[0] != 'n' || !isdigit((unsigned char) at[1]))
		return false;
	rel = strtoull(at + 1, &end, 10);
	if (rel > PG_UINT32_MAX || end[0] != '-' || end[1] != 'p' || !isdigit((unsigned char) end[2]))
		return false;
	id = strtoull(end + 2, &end, 10);
	if (id > PG_UINT32_MAX || end[0] != '-' || end[1] != 'g' || !isdigit((unsigned char) end[2]))
		return false;
	gen = strtoull(end + 2, &end, 10);
	if (sift_tier_object_key(canon, sizeof(canon), scope_id, (uint32) rel, (uint32) id, gen) <= 0 || strcmp(canon, key) != 0)
		return false;
	*k = (SiftTierPartRef) {.rel_number = (uint32) rel,.part_id = (uint32) id,.part_gen = gen};
	return true;
}

/*
 * The mark of an object that no commit names, "<object key>.orphan-<seconds>-<lsn>" (hex): when a sweep first found it and where the WAL was. The object is deleted by the sweep that finds the
 * mark old enough and every replication slot past that position, so that a standby that replays the rewrite of the relation, or a recovery that was started before it, still finds the object.
 */
#define MARK_INFIX		".orphan-"

static const char empty_sha256[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

static bool
mark_parse(const char *key, size_t *objlen, int64 *seconds, uint64 *lsn)
{
	const char *at = strstr(key, ".siftpart" MARK_INFIX);
	unsigned long long s,
				l;
	int			len = 0;

	if (at == NULL)
		return false;
	*objlen = (at - key) + strlen(".siftpart");
	if (sscanf(at + strlen(".siftpart" MARK_INFIX), "%llx-%llx%n", &s, &l, &len) != 2 || at[strlen(".siftpart" MARK_INFIX) + len] != '\0')
		return false;
	*seconds = (int64) s;
	*lsn = (uint64) l;
	return true;
}

static int
key_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* Whether a key of the sorted list starts with prefix */
static bool
has_prefixed(char *const *keys, int n, const char *prefix)
{
	int			lo = 0,
				hi = n;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (strcmp(keys[mid], prefix) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && strncmp(keys[lo], prefix, strlen(prefix)) == 0;
}

static bool
mark_due(const SiftTierSweepGate *g, int64 seconds, uint64 lsn)
{
	return g->now_s - seconds >= g->grace_s && (g->slots_lsn == 0 || g->slots_lsn >= lsn);
}

static SiftTierStatus
mark_put(SiftTierBackend *be, const char *objkey, const SiftTierSweepGate *g, SiftTierErr *err)
{
	char		key[240];
	SiftTierSrc empty = {0};
	SiftTierObjInfo info;

	snprintf(key, sizeof(key), "%s" MARK_INFIX "%llx-%llx", objkey, (unsigned long long) g->now_s, (unsigned long long) g->lsn);
	return be->ops->put(be, key, &empty, empty_sha256, &info, err);
}

/* A key of the scope that is not an object: a mark, whose object goes with it when its time has come, and anything else a store may hold of an upload that did not finish */
static SiftTierStatus
sweep_other(SiftTierBackend *be, uint64 scope_id, const RefSet *named, char **keys, int n, int i, const SiftTierSweepGate *g, SiftTierSweepTally *t, SiftTierErr *err)
{
	size_t		objlen;
	int64		seconds;
	uint64		lsn;
	SiftTierPartRef part;
	char		objkey[160];
	char	  **found;

	if (!mark_parse(keys[i], &objlen, &seconds, &lsn) || objlen >= sizeof(objkey))
	{
		SiftTierStatus st = be->ops->del(be, keys[i], err);

		t->deleted += st == SIFT_TIER_OK;
		return st;
	}
	snprintf(objkey, sizeof(objkey), "%.*s", (int) objlen, keys[i]);
	found = bsearch(&(const char *) {objkey}, keys, n, sizeof(char *), key_cmp);
	if (found == NULL || !key_part(scope_id, objkey, &part) || ref_set_has(named, &part))
		return be->ops->del(be, keys[i], err);
	if (!mark_due(g, seconds, lsn))
	{
		t->waiting++;
		return SIFT_TIER_OK;
	}
	if (be->ops->del(be, objkey, err) != SIFT_TIER_OK)
		return err->code;
	t->deleted++;
	return be->ops->del(be, keys[i], err);
}

SiftTierStatus
sift_tier_sweep_orphans(SiftTierBackend *be, uint64 scope_id, const SiftTierPartRef *refs, int nrefs, const SiftTierSweepGate *gate, SiftTierSweepTally *tally, SiftTierErr *err)
{
	char		prefix[32];
	KeyList		l = {0};
	SiftTierStatus st;
	RefSet		named;

	*tally = (SiftTierSweepTally) {0};
	snprintf(prefix, sizeof(prefix), "r%016llx/", (unsigned long long) scope_id);
	ref_set_build(&named, refs, nrefs);
	st = be->ops->list(be, prefix, keylist_add, &l, err);
	if (l.n > 1)
		qsort(l.keys, l.n, sizeof(char *), key_cmp);
	for (int i = 0; i < l.n && st == SIFT_TIER_OK; i++)
	{
		SiftTierPartRef part;
		char		with_mark[200];

		CHECK_FOR_INTERRUPTS();
		if (!key_part(scope_id, l.keys[i], &part))
			st = sweep_other(be, scope_id, &named, l.keys, l.n, i, gate, tally, err);
		else if (!ref_set_has(&named, &part) && snprintf(with_mark, sizeof(with_mark), "%s" MARK_INFIX, l.keys[i]) > 0 && !has_prefixed(l.keys, l.n, with_mark))
		{
			st = mark_put(be, l.keys[i], gate, err);
			tally->marked += st == SIFT_TIER_OK;
		}
	}
	pfree(named.slots);
	for (int i = 0; i < l.n; i++)
		pfree(l.keys[i]);
	if (l.keys)
		pfree(l.keys);
	return st;
}
