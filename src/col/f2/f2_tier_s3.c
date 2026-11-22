/*
 * col/f2/f2_tier_s3.c - the s3:// and r2:// backend: PUT, HEAD, Range GET, DELETE and a listing of an S3 compatible store (AWS, R2, MinIO), each request signed with Signature Version 4
 * (f2_tier_sigv4.c) and sent by the libcurl that f2_tier_curl.c opens. The credentials are read from the environment of the server when the backend opens (AWS_ACCESS_KEY_ID,
 * AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN, or the same names with _FILE for a file that holds the value) and live only in this backend's memory: no catalog, page, GUC or message has them.
 */
#include "core/host.h"

#include <time.h>

#include "col/f2/f2_tier_int.h"

typedef struct S3Be
{
	SiftTierBackend base;
	SiftCurl   *curl;
	bool		path_style;
	char		scheme[8];
	char		host[256];		/* with the port when it is not the default one: what the Host header says */
	char		bucket[64];
	char		prefix[480];
	char		region[64];
	char		access[128];
	char		secret[128];
	char		token[2048];
} S3Be;

static const char *
env_value(const SiftTierOpen *o, const char *name)
{
	const char *v = o->getenv_fn ? o->getenv_fn(name) : getenv(name);

	return v != NULL && v[0] != '\0' ? v : NULL;
}

/* NAME, else the first line of the file named by NAME_FILE; false when neither is set, and then err is untouched; an unreadable or oversize value fails err */
static bool
credential(const SiftTierOpen *o, const char *name, char *out, size_t cap, SiftTierErr *err)
{
	char		file_name[64];
	const char *v = env_value(o, name),
			   *path;
	FILE	   *f;

	out[0] = '\0';
	if (v != NULL)
	{
		if (strlen(v) >= cap)
			return sift_tier_fail(err, SIFT_TIER_E_NO_CRED, "%s is longer than %zu bytes", name, cap - 1), false;
		snprintf(out, cap, "%s", v);
		return true;
	}
	snprintf(file_name, sizeof(file_name), "%s_FILE", name);
	path = env_value(o, file_name);
	if (path == NULL)
		return false;
	f = fopen(path, "r");
	if (f == NULL || fgets(out, cap, f) == NULL)
		sift_tier_fail(err, SIFT_TIER_E_NO_CRED, "the file named by %s cannot be read", file_name);
	if (f != NULL)
		fclose(f);
	out[strcspn(out, "\r\n")] = '\0';
	return err->code == SIFT_TIER_E_NO_CRED ? false : out[0] != '\0';
}

/* http(s)://host[:port] and nothing else: no user, no path, no query */
static bool
parse_endpoint(const char *ep, char *scheme, size_t scap, char *host, size_t hcap)
{
	const char *p = strstr(ep, "://");
	size_t		sl;

	if (p == NULL)
		return false;
	sl = p - ep;
	if (!((sl == 4 && strncmp(ep, "http", 4) == 0) || (sl == 5 && strncmp(ep, "https", 5) == 0)) || sl >= scap)
		return false;
	p += 3;
	if (*p == '\0' || strlen(p) >= hcap || strpbrk(p, "/@?#% ") != NULL)
		return false;
	memcpy(scheme, ep, sl);
	scheme[sl] = '\0';
	snprintf(host, hcap, "%s", p);
	return true;
}

static void
amz_now(char *out)
{
	time_t		t = time(NULL);
	struct tm	tm;

	gmtime_r(&t, &tm);
	strftime(out, 17, "%Y%m%dT%H%M%SZ", &tm);
}

/* One signed request for key (NULL: the bucket itself, outside the prefix of the location) with the canonical query; the headers and the signature are built here and nowhere else */
static SiftTierStatus
s3_request(S3Be *b, const char *method, const char *key, const char *query, uint64 range_off, uint64 range_len, const char *payload_hash, SiftCurlReq *r, SiftTierErr *err)
{
	char		raw[1100],
				uri[1400],
				url[1900],
				date[17],
				range[64],
				auth[800 + 16],
				extra[4][2200];
	SiftSigHdr	hdrs[5];
	const char *lines[5];
	int			nh = 0,
				nl = 0;
	SiftSigV4In in;
	SiftSigV4Out sig;
	SiftTierStatus st;
	int			n = 0;

	n += snprintf(raw + n, sizeof(raw) - n, "%s%s", b->path_style ? "/" : "", b->path_style ? b->bucket : "");
	if (key != NULL)
		n += snprintf(raw + n, sizeof(raw) - n, "%s%s/%s", b->prefix[0] ? "/" : "", b->prefix, key);
	if (n >= (int) sizeof(raw) || sift_sigv4_uri_encode(n == 0 ? "/" : raw, true, uri, sizeof(uri)) < 0)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "the object name is too long");
	amz_now(date);
	hdrs[nh++] = (SiftSigHdr) {"host", b->host};
	hdrs[nh++] = (SiftSigHdr) {"x-amz-content-sha256", payload_hash};
	hdrs[nh++] = (SiftSigHdr) {"x-amz-date", date};
	if (b->token[0] != '\0')
		hdrs[nh++] = (SiftSigHdr) {"x-amz-security-token", b->token};
	if (range_len > 0)
	{
		snprintf(range, sizeof(range), "bytes=%llu-%llu", (unsigned long long) range_off, (unsigned long long) (range_off + range_len - 1));
		hdrs[nh++] = (SiftSigHdr) {"range", range};
	}
	in = (SiftSigV4In) {.method = method,.canon_uri = uri,.canon_query = query,.hdrs = hdrs,.nhdrs = nh,.payload_hash = payload_hash,.amz_date = date,
		.region = b->region,.service = "s3",.access_key = b->access,.secret_key = b->secret};
	if (!sift_sigv4_sign(&in, &sig, err))
		return err->code;
	snprintf(auth, sizeof(auth), "Authorization: %s", sig.authorization);
	lines[nl++] = auth;
	for (int i = 1; i < nh; i++)
	{
		snprintf(extra[i - 1], sizeof(extra[i - 1]), "%s: %s", hdrs[i].name, hdrs[i].value);
		lines[nl++] = extra[i - 1];
	}
	snprintf(url, sizeof(url), "%s://%s%s%s%s", b->scheme, b->host, uri, query[0] ? "?" : "", query);
	r->method = method;
	r->url = url;
	r->headers = lines;
	r->nheaders = nl;
	st = sift_curl_perform(b->curl, r, err);
	sift_sigv4_out_free(&sig);
	return st;
}

static SiftTierStatus
http_fail(const SiftCurlReq *r, SiftTierErr *err, const char *what, const char *key)
{
	return sift_tier_fail(err, r->status == 404 ? SIFT_TIER_E_NOT_FOUND : SIFT_TIER_E_IO, "%s of object %s: the store answered HTTP %ld", what, key, r->status);
}

static SiftTierStatus
s3_put(SiftTierBackend *be, const char *key, const SiftTierSrc *src, const char *sha256_hex, SiftTierObjInfo *info, SiftTierErr *err)
{
	SiftCurlReq r = {.upload = src};
	SiftTierStatus st;

	if (!sift_tier_key_ok(key) || strlen(sha256_hex) != 64)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name or its digest is not SHA-256", key);
	if (src->len > SIFT_REMOTE_MAX_BYTES)
		return sift_tier_fail(err, SIFT_TIER_E_TOO_LARGE, "the object is larger than one PUT");
	st = s3_request((S3Be *) be, "PUT", key, "", 0, 0, sha256_hex, &r, err);
	if (st != SIFT_TIER_OK)
		return st;
	if (r.status / 100 != 2)
		return http_fail(&r, err, "the PUT", key);
	info->len = src->len;
	snprintf(info->etag, sizeof(info->etag), "%s", r.etag);
	return SIFT_TIER_OK;
}

#define EMPTY_SHA256	"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

static SiftTierStatus
s3_head(SiftTierBackend *be, const char *key, SiftTierObjInfo *info, SiftTierErr *err)
{
	SiftCurlReq r = {0};
	SiftTierStatus st = sift_tier_key_ok(key) ? s3_request((S3Be *) be, "HEAD", key, "", 0, 0, EMPTY_SHA256, &r, err) : sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);

	if (st != SIFT_TIER_OK)
		return st;
	if (r.status / 100 != 2 || r.content_length == UINT64_MAX)
		return http_fail(&r, err, "the HEAD", key);
	info->len = r.content_length;
	snprintf(info->etag, sizeof(info->etag), "%s", r.etag);
	return SIFT_TIER_OK;
}

static SiftTierStatus
s3_get(SiftTierBackend *be, const char *key, uint64 off, uint64 len, void *dst, SiftTierErr *err)
{
	SiftCurlReq r = {.dst = dst,.dst_cap = len};
	SiftTierStatus st;

	if (len == 0)
		return SIFT_TIER_OK;
	st = sift_tier_key_ok(key) ? s3_request((S3Be *) be, "GET", key, "", off, len, EMPTY_SHA256, &r, err) : sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);
	if (st != SIFT_TIER_OK)
		return st;
	if (r.status == 416)
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "object %s ends before byte %llu", key, (unsigned long long) off);
	if (r.status != 206 && r.status != 200)
		return http_fail(&r, err, "the GET", key);
	if (r.received != len)
		return sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "object %s ends before byte %llu", key, (unsigned long long) (off + r.received));
	return SIFT_TIER_OK;
}

static SiftTierStatus
s3_del(SiftTierBackend *be, const char *key, SiftTierErr *err)
{
	SiftCurlReq r = {0};
	SiftTierStatus st = sift_tier_key_ok(key) ? s3_request((S3Be *) be, "DELETE", key, "", 0, 0, EMPTY_SHA256, &r, err) : sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);

	if (st != SIFT_TIER_OK)
		return st;
	return r.status / 100 == 2 || r.status == 404 ? SIFT_TIER_OK : http_fail(&r, err, "the DELETE", key);
}

/* The text between <tag> and </tag> after *at, which moves past it; NULL when there is none */
static char *
xml_next(char **at, const char *tag)
{
	char		open[32],
				close[32];
	char	   *s,
			   *e;

	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	s = strstr(*at, open);
	if (s == NULL)
		return NULL;
	s += strlen(open);
	e = strstr(s, close);
	if (e == NULL)
		return NULL;
	*e = '\0';
	*at = e + strlen(close);
	return s;
}

static SiftTierStatus
s3_list(SiftTierBackend *be, const char *prefix, void (*fn) (void *arg, const char *key), void *arg, SiftTierErr *err)
{
	S3Be	   *b = (S3Be *) be;
	char		full[640],
				enc[1400],
				query[1900];
	char		token[1024] = "";
	size_t		strip = b->prefix[0] ? strlen(b->prefix) + 1 : 0;

	snprintf(full, sizeof(full), "%s%s%s", b->prefix, b->prefix[0] ? "/" : "", prefix);
	for (;;)
	{
		char	   *body = NULL;
		SiftCurlReq r = {.grow = &body};
		SiftTierStatus st;
		char	   *at,
				   *key,
				   *trunc;

		sift_sigv4_uri_encode(full, false, enc, sizeof(enc));
		if (token[0] != '\0')
		{
			char		tenc[1100];

			sift_sigv4_uri_encode(token, false, tenc, sizeof(tenc));
			snprintf(query, sizeof(query), "continuation-token=%s&list-type=2&prefix=%s", tenc, enc);
		}
		else
			snprintf(query, sizeof(query), "list-type=2&prefix=%s", enc);
		st = s3_request(b, "GET", NULL, query, 0, 0, EMPTY_SHA256, &r, err);
		if (st == SIFT_TIER_OK && r.status != 200)
			st = http_fail(&r, err, "the listing", prefix);
		if (st != SIFT_TIER_OK)
		{
			if (body)
				pfree(body);
			return st;
		}
		at = body ? body : (char *) "";
		trunc = strstr(at, "<IsTruncated>true</IsTruncated>");
		token[0] = '\0';
		if (trunc != NULL)
		{
			char	   *tp = at,
					   *t = xml_next(&tp, "NextContinuationToken");

			if (t != NULL && strlen(t) < sizeof(token))
				snprintf(token, sizeof(token), "%s", t);
		}
		while ((key = xml_next(&at, "Key")) != NULL)
			if (strlen(key) > strip && sift_tier_key_ok(key + strip))
				fn(arg, key + strip);
		if (body)
			pfree(body);
		if (token[0] == '\0')
			return SIFT_TIER_OK;
	}
}

static void
s3_close(SiftTierBackend *be)
{
	S3Be	   *b = (S3Be *) be;

	sift_curl_close(b->curl);
	memset(b->secret, 0, sizeof(b->secret));
	memset(b->token, 0, sizeof(b->token));
	pfree(b);
}

static const SiftTierBackendOps s3_ops = {s3_put, s3_head, s3_get, s3_del, s3_list, s3_close};

static bool
set_addressing(S3Be *b, const SiftTierOpen *o, bool r2, SiftTierErr *err)
{
	const char *region = env_value(o, "AWS_REGION") ? env_value(o, "AWS_REGION") : env_value(o, "AWS_DEFAULT_REGION");

	if (o->endpoint != NULL && o->endpoint[0] != '\0')
	{
		if (!parse_endpoint(o->endpoint, b->scheme, sizeof(b->scheme), b->host, sizeof(b->host)))
		{
			sift_tier_fail(err, SIFT_TIER_E_URI, "the endpoint must be http://host[:port] or https://host[:port], with no user, path or query");
			return false;
		}
		b->path_style = true;
	}
	else if (r2)
	{
		sift_tier_fail(err, SIFT_TIER_E_URI, "an r2:// location needs the endpoint of the account (https://<account>.r2.cloudflarestorage.com)");
		return false;
	}
	else if (region == NULL)
	{
		sift_tier_fail(err, SIFT_TIER_E_URI, "an s3:// location needs AWS_REGION in the environment of the server, or an endpoint");
		return false;
	}
	else
	{
		snprintf(b->scheme, sizeof(b->scheme), "https");
		b->path_style = strchr(b->bucket, '.') != NULL;
		if (b->path_style)
			snprintf(b->host, sizeof(b->host), "s3.%s.amazonaws.com", region);
		else
			snprintf(b->host, sizeof(b->host), "%s.s3.%s.amazonaws.com", b->bucket, region);
	}
	snprintf(b->region, sizeof(b->region), "%s", r2 ? "auto" : region ? region : "us-east-1");
	return true;
}

static bool
load_credentials(S3Be *b, const SiftTierOpen *o, SiftTierErr *err)
{
	err->code = SIFT_TIER_OK;
	if (credential(o, "AWS_ACCESS_KEY_ID", b->access, sizeof(b->access), err) && credential(o, "AWS_SECRET_ACCESS_KEY", b->secret, sizeof(b->secret), err))
		return credential(o, "AWS_SESSION_TOKEN", b->token, sizeof(b->token), err) || err->code == SIFT_TIER_OK;
	if (err->code == SIFT_TIER_OK)
		sift_tier_fail(err, SIFT_TIER_E_NO_CRED, "AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY (or their _FILE variants) are not set in the environment of the server");
	return false;
}

SiftTierBackend *
sift_tier_s3_open(const SiftTierOpen *o, bool r2, const char *bucket, const char *prefix, SiftTierErr *err)
{
	S3Be	   *b = palloc0(sizeof(S3Be));

	snprintf(b->bucket, sizeof(b->bucket), "%s", bucket);
	snprintf(b->prefix, sizeof(b->prefix), "%s", prefix);
	if (sift_curl_open(o->curl_soname, &b->curl, err) != SIFT_TIER_OK)
		goto fail;
	if (!set_addressing(b, o, r2, err) || !load_credentials(b, o, err))
	{
		sift_curl_close(b->curl);
		goto fail;
	}
	b->base.ops = &s3_ops;
	return &b->base;

fail:
	memset(b, 0, sizeof(*b));
	pfree(b);
	return NULL;
}
