/*
 * col/f2/f2_tier.h - cold object storage for the parts of a Format 2.0 relation: the self-contained .siftpart object, the storage backends (file:// always, s3:// and r2:// over a libcurl found at run
 * time), the bounded local cache of ranges and the upload and read of a part. Nothing here raises: every call returns a SiftTierStatus and says why in a SiftTierErr, so that the caller decides
 * between ERROR and retry (f2_tier_host.c). No secret is ever written to a SiftTierErr, a log line or a page.
 */
#ifndef SIFT_F2_TIER_H
#define SIFT_F2_TIER_H

#include "core/host.h"

#include "core/f2/manifest.h"
#include "core/f2/tier.h"

typedef enum SiftTierStatus
{
	SIFT_TIER_OK = 0,
	SIFT_TIER_E_ARG,			/* a name, size or option the call does not accept */
	SIFT_TIER_E_URI,			/* a location this build cannot use */
	SIFT_TIER_E_NO_CURL,		/* s3:// or r2:// without libcurl.so.4 on the host */
	SIFT_TIER_E_NO_CRED,		/* s3:// or r2:// without credentials in the environment */
	SIFT_TIER_E_IO,				/* the store failed (local I/O error, HTTP status, transfer) */
	SIFT_TIER_E_NOT_FOUND,
	SIFT_TIER_E_CORRUPT,		/* the object or a cached range does not hold what its checksum says */
	SIFT_TIER_E_MISMATCH,		/* the object is not the one the directory names */
	SIFT_TIER_E_TOO_LARGE
} SiftTierStatus;

typedef struct SiftTierErr
{
	SiftTierStatus code;
	char		msg[224];
} SiftTierErr;

extern SiftTierStatus sift_tier_fail(SiftTierErr *err, SiftTierStatus code, const char *fmt, ...) pg_attribute_printf(3, 4);

/* ---- hashes (f2_tier_hash.c) ---- */

typedef struct SiftSha256
{
	uint32		h[8];
	uint64		len;
	uint8		buf[64];
	uint32		nbuf;
} SiftSha256;

#define SIFT_SHA256_LEN		32
#define SIFT_SHA256_HEX		65

extern void sift_sha256_init(SiftSha256 *c);
extern void sift_sha256_update(SiftSha256 *c, const void *data, size_t len);
extern void sift_sha256_final(SiftSha256 *c, uint8 out[SIFT_SHA256_LEN]);
extern void sift_hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, uint8 out[SIFT_SHA256_LEN]);
extern void sift_hex(const uint8 *in, size_t n, char *out);

/* CRC32C (Castagnoli) continued over data: extend(0, d) is the checksum of d, extend(extend(0, a), b) that of a followed by b */
extern uint32 sift_tier_crc32c_extend(uint32 crc, const void *data, size_t len);

/* ---- AWS Signature Version 4 (f2_tier_sigv4.c) ---- */

typedef struct SiftSigHdr
{
	const char *name;			/* lower case */
	const char *value;
} SiftSigHdr;

typedef struct SiftSigV4In
{
	const char *method;
	const char *canon_uri;		/* the path as it goes on the wire: encoded once, as sift_sigv4_uri_encode makes it */
	const char *canon_query;	/* the query as it goes on the wire, sorted by name, "" for none */
	const SiftSigHdr *hdrs;		/* every header that is signed, host and x-amz-date among them */
	int			nhdrs;
	const char *payload_hash;	/* 64 hex digits, or UNSIGNED-PAYLOAD */
	const char *amz_date;		/* YYYYMMDDTHHMMSSZ */
	const char *region;
	const char *service;
	const char *access_key;
	const char *secret_key;
} SiftSigV4In;

typedef struct SiftSigV4Out
{
	char	   *canonical_request;	/* palloc'd */
	char		signed_headers[256];
	char		string_to_sign[512];
	char		signature[SIFT_SHA256_HEX];
	char		authorization[768];
} SiftSigV4Out;

extern bool sift_sigv4_sign(const SiftSigV4In *in, SiftSigV4Out *out, SiftTierErr *err);
extern void sift_sigv4_out_free(SiftSigV4Out *out);
extern void sift_sigv4_signing_key(const char *secret_key, const char *date8, const char *region, const char *service, uint8 out[SIFT_SHA256_LEN]);

/* RFC 3986 encoding of a path (keep_slash) or of one query component; returns the length, or -1 when cap is too small */
extern int	sift_sigv4_uri_encode(const char *in, bool keep_slash, char *out, size_t cap);

/* ---- the .siftpart object (f2_tier_obj.c) ---- */

/*
 * [SiftRemotePartHeader][image: the header page of the part, header_len bytes][stream: stream_len bytes][range table: nranges SiftRemoteRange][SiftRemoteFooter]
 * Every offset of a range is into the stream. The image carries no block number that means anything off the relation: a reader takes the extents out of it.
 */
#define SIFT_REMOTE_PART_MAGIC		0x53465032U		/* "SFP2" */
#define SIFT_REMOTE_FOOTER_MAGIC	0x53465046U		/* "SFPF" */
#define SIFT_REMOTE_PART_VERSION	1
#define SIFT_REMOTE_MAX_BYTES		(UINT64CONST(5) << 30)	/* one PUT of S3 */

typedef struct SiftRemotePartHeader
{
	uint32		magic;
	uint16		version;
	uint16		hdr_size;		/* sizeof(SiftRemotePartHeader) */
	uint32		flags;			/* 0 */
	uint32		header_len;
	uint64		part_gen;
	uint32		part_id;
	uint32		nranges;
	uint64		stream_len;
	uint64		footer_off;		/* where the range table starts */
	uint64		total_len;
	uint32		image_crc32c;
	uint32		hdr_crc32c;		/* of this header with the field zero */
} SiftRemotePartHeader;

StaticAssertDecl(sizeof(SiftRemotePartHeader) == 64, "SiftRemotePartHeader must be 64 bytes");

/* A range of the stream a reader may ask for alone (a column chunk of a granule), with the checksum of its bytes */
typedef struct SiftRemoteRange
{
	uint32		col;
	uint32		granule;
	uint64		off;
	uint32		len;
	uint32		crc32c;
} SiftRemoteRange;

StaticAssertDecl(sizeof(SiftRemoteRange) == 24, "SiftRemoteRange must be 24 bytes");

typedef struct SiftRemoteFooter
{
	uint32		magic;
	uint32		nranges;
	uint32		table_crc32c;
	uint32		pad;
} SiftRemoteFooter;

StaticAssertDecl(sizeof(SiftRemoteFooter) == 16, "SiftRemoteFooter must be 16 bytes");

/* The bytes of an object, read by offset: what a backend puts and the digest runs over */
typedef struct SiftTierSrc
{
	uint64		len;
	void		(*read) (void *arg, uint64 off, uint64 n, void *dst);
	void	   *arg;
} SiftTierSrc;

/* What the host gives for one part: its header page image, its stream by offset and the ranges its columns have in the stream (ascending off) */
typedef struct SiftTierPartSrc
{
	uint64		part_gen;
	uint32		part_id;
	const void *image;
	uint32		image_len;
	uint64		stream_len;
	void		(*read_stream) (void *arg, uint64 off, uint64 n, void *dst);
	void	   *arg;
	const SiftRemoteRange *ranges;
	uint32		nranges;
} SiftTierPartSrc;

/* The object of a part as bytes: a SiftTierSrc over the header, the image, the stream (through the callback) and the footer */
typedef struct SiftTierObj
{
	SiftRemotePartHeader hdr;
	const SiftTierPartSrc *part;
	uint8	   *tail;			/* range table and footer */
	uint64		tail_len;
	SiftTierSrc src;
} SiftTierObj;

extern SiftTierStatus sift_tier_obj_build(SiftTierObj *obj, const SiftTierPartSrc *part, SiftTierErr *err);
extern void sift_tier_obj_free(SiftTierObj *obj);

/* The object name relative to the policy location: one directory per relation (scope_id), one object per part generation */
extern int	sift_tier_object_key(char *out, size_t cap, uint64 scope_id, uint32 part_id, uint64 part_gen);

/* CRC32C and SHA-256 of the whole source, read in chunks */
extern void sift_tier_src_digest(const SiftTierSrc *src, uint32 *crc, uint8 sha[SIFT_SHA256_LEN]);

/* The header read from the first bytes of an object, checked against itself (magic, version, checksum, offsets) */
extern SiftTierStatus sift_tier_obj_parse_header(const void *buf, SiftRemotePartHeader *hdr, SiftTierErr *err);

/* The range table and footer (hdr.total_len - hdr.footer_off bytes at hdr.footer_off), checked and copied out; free with pfree */
extern SiftTierStatus sift_tier_obj_parse_tail(const SiftRemotePartHeader *hdr, const void *tail, SiftRemoteRange **ranges, SiftTierErr *err);

/* The ranges that cover [off, off + len) of the stream: the first and the count (ranges ascending by off); count 0 when none intersects */
extern uint32 sift_tier_ranges_cover(const SiftRemoteRange *ranges, uint32 n, uint64 off, uint64 len, uint32 *first);

/* ---- storage backends (f2_tier_io.c, f2_tier_curl.c, f2_tier_s3.c) ---- */

#define SIFT_TIER_ETAG_LEN		64

typedef struct SiftTierObjInfo
{
	uint64		len;
	char		etag[SIFT_TIER_ETAG_LEN];
} SiftTierObjInfo;

typedef struct SiftTierBackend SiftTierBackend;

typedef struct SiftTierBackendOps
{
	/* the object whole from src, atomically visible once it returns; sha256_hex is its digest, which a store may check */
	SiftTierStatus (*put) (SiftTierBackend *be, const char *key, const SiftTierSrc *src, const char *sha256_hex, SiftTierObjInfo *info, SiftTierErr *err);
	SiftTierStatus (*head) (SiftTierBackend *be, const char *key, SiftTierObjInfo *info, SiftTierErr *err);
	/* exactly len bytes from off; an object that ends sooner is SIFT_TIER_E_CORRUPT */
	SiftTierStatus (*get) (SiftTierBackend *be, const char *key, uint64 off, uint64 len, void *dst, SiftTierErr *err);
	/* deleting an object that is not there succeeds */
	SiftTierStatus (*del) (SiftTierBackend *be, const char *key, SiftTierErr *err);
	/* every object whose key starts with prefix, once each, the key as put */
	SiftTierStatus (*list) (SiftTierBackend *be, const char *prefix, void (*fn) (void *arg, const char *key), void *arg, SiftTierErr *err);
	void		(*close) (SiftTierBackend *be);
} SiftTierBackendOps;

struct SiftTierBackend
{
	const SiftTierBackendOps *ops;
	bool		is_local;		/* a directory of this host: an upload is read back whole to verify it, where a remote store checks the digest of the PUT itself */
};

typedef struct SiftTierOpen
{
	const char *uri;			/* file:///abs/dir, s3://bucket/prefix or r2://bucket/prefix: no user, password or query */
	const char *endpoint;		/* http(s)://host[:port] of an S3 compatible store; required by r2:// */
	const char *(*getenv_fn) (const char *name);	/* credentials and region; NULL: getenv */
	const char *curl_soname;	/* NULL: libcurl.so.4 */
} SiftTierOpen;

extern SiftTierBackend *sift_tier_open(const SiftTierOpen *o, SiftTierErr *err);
extern void sift_tier_close(SiftTierBackend *be);

/* Whether the location is one this build accepts: a supported scheme, an absolute path or a bucket, no user, password, query or fragment, no ".." and no byte outside the key alphabet */
extern SiftTierStatus sift_tier_uri_check(const char *uri, SiftTierErr *err);

/* The two backends, for sift_tier_open */
extern SiftTierBackend *sift_tier_file_open(const char *dir, SiftTierErr *err);
extern SiftTierBackend *sift_tier_s3_open(const SiftTierOpen *o, bool r2, const char *bucket, const char *prefix, SiftTierErr *err);

/* ---- the bounded local cache (f2_tier_cache.c) ---- */

typedef struct SiftTierCache SiftTierCache;

typedef struct SiftTierCacheStats
{
	uint64		hits;			/* chunks served from the directory */
	uint64		misses;			/* chunks fetched */
	uint64		fetches;		/* range requests the misses made (adjacent misses share one) */
	uint64		fetched_bytes;
	uint64		evictions;
	uint64		resident_bytes;
	uint64		resident_chunks;
} SiftTierCacheStats;

/* dir exists and belongs to this instance; budget_bytes bounds the chunk files of it; chunk_bytes is the unit of a fetch (a multiple of 4096) */
extern SiftTierCache *sift_tier_cache_open(const char *dir, uint64 budget_bytes, uint32 chunk_bytes, SiftTierErr *err);
extern void sift_tier_cache_close(SiftTierCache *c);
extern const SiftTierCacheStats *sift_tier_cache_stats(const SiftTierCache *c);

/* [off, off + len) of the object, from the chunks the directory holds and from Range requests for the others; the chunks are kept and the least used evicted past the budget */
extern SiftTierStatus sift_tier_cache_read(SiftTierCache *c, SiftTierBackend *be, const char *key, const SiftTierPartRef *ref, uint64 off, uint64 len, void *dst, SiftTierErr *err);

/* Takes the chunks that hold [off, off + len) of the object out of the cache (bytes that were fetched whole and found wrong must not be served again) */
extern void sift_tier_cache_forget(SiftTierCache *c, const SiftTierPartRef *ref, uint64 off, uint64 len);

/* ---- upload and read of a part (f2_tier.c) ---- */

/*
 * A part that may move to object storage: of level 2, sealed and settled (all visible, frozen), with no deletion vector or column delta waiting, not packed, not already remote and not about to
 * expire, whose newest value of the time column (slot) is older than cutoff. A part with a time bound newer than the cutoff is still hot: it is read often and rewritten by compaction.
 * time_dirty: the column delta chain of its partition patched that time column, so the bounds of the part do not hold and none is eligible.
 */
extern bool sift_tier_part_eligible(const SiftPartDesc *d, int slot, int64 cutoff, bool time_dirty);

/* Writes the object, checks what the store has (size, ETag, the header read back, and for a store that is a file the whole object), fills ref; the caller commits it to the directory */
extern SiftTierStatus sift_tier_upload(SiftTierBackend *be, const char *key, const SiftTierPartSrc *part, uint32 policy_id, TimestampTz now, SiftTierPartRef *ref, SiftTierErr *err);

/* The whole object read back and compared with the ref: SHA-256, CRC32C and length (for sift.check and for the tests) */
extern SiftTierStatus sift_tier_verify_object(SiftTierBackend *be, const char *key, const SiftTierPartRef *ref, SiftTierErr *err);

/*
 * What sift.check asks of an object the directory names: it is in the store (NOT_FOUND), its header is a .siftpart header of this version for this very part (magic, version, header checksum,
 * offsets, generation, partition, lengths), its size is the directory's, and its SHA-256 and CRC32C are (MISMATCH, CORRUPT). Reads the whole object once.
 */
extern SiftTierStatus sift_tier_check_object(SiftTierBackend *be, const char *key, const SiftTierPartRef *ref, SiftTierErr *err);

/* A part in the store, open: its header, image and range table in memory, its stream through the cache */
typedef struct SiftTierRemote
{
	SiftTierBackend *be;
	SiftTierCache *cache;
	char		key[160];
	SiftTierPartRef ref;
	SiftRemotePartHeader hdr;
	uint8	   *image;
	SiftRemoteRange *ranges;
} SiftTierRemote;

extern SiftTierStatus sift_tier_remote_open(SiftTierBackend *be, SiftTierCache *cache, const char *key, const SiftTierPartRef *ref, SiftTierRemote **out, SiftTierErr *err);
extern void sift_tier_remote_close(SiftTierRemote *r);

/* Bytes [off, off + len) of the stream, and the same checked against the checksums of the ranges that cover them whole */
extern SiftTierStatus sift_tier_remote_read(SiftTierRemote *r, uint64 off, uint64 len, void *dst, SiftTierErr *err);
extern SiftTierStatus sift_tier_remote_read_range(SiftTierRemote *r, uint32 range_idx, void *dst, SiftTierErr *err);

/*
 * Deletes the objects of the relation (scope_id) that the directory does not name: what an upload that crashed before its commit leaves, and the temporary files of a store that is a directory.
 * Runs under the maintenance lock, which every upload holds too, so an object that is not named is not one in flight. Returns the count in *ndeleted.
 */
extern SiftTierStatus sift_tier_sweep_orphans(SiftTierBackend *be, uint64 scope_id, const SiftTierPartRef *refs, int nrefs, int *ndeleted, SiftTierErr *err);

#endif							/* SIFT_F2_TIER_H */
