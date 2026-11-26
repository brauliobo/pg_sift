/*
 * col/f2/tier/obj.c - the .siftpart object: its layout in bytes, the checks a reader makes of its header and range table, and the digest of a whole object (col/f2/tier/tier.h).
 */
#include "core/host.h"

#include "miscadmin.h"

#include "col/f2/tier/tier.h"

#define DIGEST_CHUNK	(UINT32_C(1) << 20)
#define MAX_RANGES		(UINT32_C(1) << 24)

int
sift_tier_object_key(char *out, size_t cap, uint64 scope_id, uint32 rel_number, uint32 part_id, uint64 part_gen)
{
	int			n = snprintf(out, cap, "r%016llx/n%u-p%u-g%llu.siftpart", (unsigned long long) scope_id, rel_number, part_id, (unsigned long long) part_gen);

	return n > 0 && (size_t) n < cap ? n : -1;
}

int
sift_tier_ref_key(char *out, size_t cap, uint64 scope_id, const SiftTierPartRef *ref)
{
	return sift_tier_object_key(out, cap, scope_id, ref->rel_number, ref->part_id, ref->part_gen);
}

static uint32
header_crc(const SiftRemotePartHeader *h)
{
	SiftRemotePartHeader c = *h;

	c.hdr_crc32c = 0;
	return sift_tier_crc32c_extend(0, &c, sizeof(c));
}

static void
obj_read(void *arg, uint64 off, uint64 n, void *dst)
{
	const SiftTierObj *o = arg;
	uint64		image_at = sizeof(SiftRemotePartHeader),
				stream_at = image_at + o->hdr.header_len,
				tail_at = o->hdr.footer_off;
	char	   *out = dst;

	Assert(off + n <= o->src.len);
	while (n > 0)
	{
		uint64		take;

		if (off < image_at)
		{
			take = Min(n, image_at - off);
			memcpy(out, (const char *) &o->hdr + off, take);
		}
		else if (off < stream_at)
		{
			take = Min(n, stream_at - off);
			memcpy(out, (const char *) o->part->image + (off - image_at), take);
		}
		else if (off < tail_at)
		{
			take = Min(n, tail_at - off);
			o->part->read_stream(o->part->arg, off - stream_at, take, out);
		}
		else
		{
			take = n;
			memcpy(out, o->tail + (off - tail_at), take);
		}
		out += take;
		off += take;
		n -= take;
	}
}

static SiftTierStatus
check_part(const SiftTierPartSrc *p, SiftTierErr *err)
{
	uint64		end = 0;

	if (p->image == NULL || p->image_len == 0 || p->read_stream == NULL || p->part_gen == 0 || p->nranges > MAX_RANGES || (p->nranges > 0 && p->ranges == NULL))
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "the part to tier has no image, stream reader or generation");
	for (uint32 i = 0; i < p->nranges; i++)
	{
		if (p->ranges[i].len == 0 || p->ranges[i].off < end || p->ranges[i].off + p->ranges[i].len > p->stream_len)
			return sift_tier_fail(err, SIFT_TIER_E_ARG, "range %u of the part is empty, overlaps the one before or ends past the stream", i);
		end = p->ranges[i].off + p->ranges[i].len;
	}
	if (p->stream_len > SIFT_REMOTE_MAX_BYTES - p->image_len - p->nranges * sizeof(SiftRemoteRange))
		return sift_tier_fail(err, SIFT_TIER_E_TOO_LARGE, "the part is larger than one object (%llu bytes) can hold", (unsigned long long) SIFT_REMOTE_MAX_BYTES);
	return SIFT_TIER_OK;
}

SiftTierStatus
sift_tier_obj_build(SiftTierObj *obj, const SiftTierPartSrc *part, SiftTierErr *err)
{
	SiftRemoteFooter *foot;
	SiftTierStatus st = check_part(part, err);

	if (st != SIFT_TIER_OK)
		return st;
	memset(obj, 0, sizeof(*obj));
	obj->part = part;
	obj->tail_len = part->nranges * sizeof(SiftRemoteRange) + sizeof(SiftRemoteFooter);
	obj->tail = palloc0(obj->tail_len);
	if (part->nranges > 0)
		memcpy(obj->tail, part->ranges, part->nranges * sizeof(SiftRemoteRange));
	foot = (SiftRemoteFooter *) (obj->tail + part->nranges * sizeof(SiftRemoteRange));
	foot->magic = SIFT_REMOTE_FOOTER_MAGIC;
	foot->nranges = part->nranges;
	foot->table_crc32c = sift_tier_crc32c_extend(0, obj->tail, part->nranges * sizeof(SiftRemoteRange));
	obj->hdr = (SiftRemotePartHeader) {
		.magic = SIFT_REMOTE_PART_MAGIC,.version = SIFT_REMOTE_PART_VERSION,.hdr_size = sizeof(SiftRemotePartHeader),.header_len = part->image_len,
		.part_gen = part->part_gen,.part_id = part->part_id,.nranges = part->nranges,.stream_len = part->stream_len,
		.footer_off = sizeof(SiftRemotePartHeader) + part->image_len + part->stream_len,.image_crc32c = sift_tier_crc32c_extend(0, part->image, part->image_len)
	};
	obj->hdr.total_len = obj->hdr.footer_off + obj->tail_len;
	obj->hdr.hdr_crc32c = header_crc(&obj->hdr);
	obj->src = (SiftTierSrc) {.len = obj->hdr.total_len,.read = obj_read,.arg = obj};
	return SIFT_TIER_OK;
}

void
sift_tier_obj_free(SiftTierObj *obj)
{
	if (obj->tail)
		pfree(obj->tail);
	obj->tail = NULL;
}

void
sift_tier_src_digest(const SiftTierSrc *src, uint32 *crc, uint8 sha[SIFT_SHA256_LEN])
{
	char	   *buf = palloc(DIGEST_CHUNK);
	SiftSha256	c;

	sift_sha256_init(&c);
	*crc = 0;
	for (uint64 off = 0; off < src->len;)
	{
		uint64		n = Min(DIGEST_CHUNK, src->len - off);

		CHECK_FOR_INTERRUPTS();
		src->read(src->arg, off, n, buf);
		sift_sha256_update(&c, buf, n);
		*crc = sift_tier_crc32c_extend(*crc, buf, n);
		off += n;
	}
	sift_sha256_final(&c, sha);
	pfree(buf);
}

SiftTierStatus
sift_tier_obj_parse_header(const void *buf, SiftRemotePartHeader *hdr, SiftTierErr *err)
{
	memcpy(hdr, buf, sizeof(*hdr));
	if (hdr->magic != SIFT_REMOTE_PART_MAGIC)
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "the object is not a .siftpart (wrong magic)");
	if (hdr->version != SIFT_REMOTE_PART_VERSION || hdr->hdr_size != sizeof(*hdr) || hdr->flags != 0)
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "the object has a version or flags this build does not read");
	if (hdr->hdr_crc32c != header_crc(hdr))
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "the header of the object has the wrong checksum");
	if (hdr->header_len == 0 || hdr->nranges > MAX_RANGES || hdr->total_len > SIFT_REMOTE_MAX_BYTES ||
		hdr->footer_off != sizeof(*hdr) + (uint64) hdr->header_len + hdr->stream_len ||
		hdr->total_len != hdr->footer_off + (uint64) hdr->nranges * sizeof(SiftRemoteRange) + sizeof(SiftRemoteFooter))
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "the header of the object holds lengths that do not add up");
	return SIFT_TIER_OK;
}

SiftTierStatus
sift_tier_obj_parse_tail(const SiftRemotePartHeader *hdr, const void *tail, SiftRemoteRange **ranges, SiftTierErr *err)
{
	uint64		table_len = (uint64) hdr->nranges * sizeof(SiftRemoteRange),
				end = 0;
	SiftRemoteFooter foot;

	memcpy(&foot, (const char *) tail + table_len, sizeof(foot));
	if (foot.magic != SIFT_REMOTE_FOOTER_MAGIC || foot.nranges != hdr->nranges || foot.table_crc32c != sift_tier_crc32c_extend(0, tail, table_len))
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "the range table of the object has the wrong footer or checksum");
	*ranges = NULL;
	if (hdr->nranges == 0)
		return SIFT_TIER_OK;
	*ranges = palloc(table_len);
	memcpy(*ranges, tail, table_len);
	for (uint32 i = 0; i < hdr->nranges; i++)
	{
		const SiftRemoteRange *r = &(*ranges)[i];

		if (r->len == 0 || r->off < end || r->off + r->len > hdr->stream_len)
		{
			pfree(*ranges);
			*ranges = NULL;
			return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "range %u of the object lies outside its stream or overlaps", i);
		}
		end = r->off + r->len;
	}
	return SIFT_TIER_OK;
}

uint32
sift_tier_ranges_cover(const SiftRemoteRange *ranges, uint32 n, uint64 off, uint64 len, uint32 *first)
{
	uint32		lo = 0,
				hi = n,
				cnt = 0;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (ranges[mid].off + ranges[mid].len <= off)
			lo = mid + 1;
		else
			hi = mid;
	}
	*first = lo;
	while (lo + cnt < n && len > 0 && ranges[lo + cnt].off < off + len)
		cnt++;
	return cnt;
}
