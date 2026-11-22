/*
 * col/f2/f2_tier_sigv4.c - AWS Signature Version 4 for the requests of the object store backend (s3:// and r2://): the canonical request, the string to sign, the signing key and the
 * Authorization header, over the SHA-256 of f2_tier_hash.c. The secret key goes into the HMAC chain and nowhere else: not into the output, not into an error.
 */
#include "core/host.h"

#include "col/f2/f2_tier.h"

#define ALGORITHM		"AWS4-HMAC-SHA256"

typedef struct Buf
{
	char	   *d;
	size_t		n;
	size_t		cap;
} Buf;

static void
buf_add(Buf *b, const char *s, size_t len)
{
	if (b->n + len + 1 > b->cap)
	{
		b->cap = Max(b->cap * 2, b->n + len + 1 + 256);
		b->d = b->d ? repalloc(b->d, b->cap) : palloc(b->cap);
	}
	memcpy(b->d + b->n, s, len);
	b->n += len;
	b->d[b->n] = '\0';
}

static void
buf_str(Buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

int
sift_sigv4_uri_encode(const char *in, bool keep_slash, char *out, size_t cap)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t		n = 0;

	for (const unsigned char *p = (const unsigned char *) in; *p; p++)
	{
		bool		plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
			*p == '-' || *p == '_' || *p == '.' || *p == '~' || (keep_slash && *p == '/');

		if (n + (plain ? 1 : 3) + 1 > cap)
			return -1;
		if (plain)
			out[n++] = *p;
		else
		{
			out[n++] = '%';
			out[n++] = hex[*p >> 4];
			out[n++] = hex[*p & 15];
		}
	}
	out[n] = '\0';
	return (int) n;
}

void
sift_sigv4_signing_key(const char *secret_key, const char *date8, const char *region, const char *service, uint8 out[SIFT_SHA256_LEN])
{
	char		seed[256];
	uint8		k1[SIFT_SHA256_LEN],
				k2[SIFT_SHA256_LEN],
				k3[SIFT_SHA256_LEN];

	snprintf(seed, sizeof(seed), "AWS4%s", secret_key);
	sift_hmac_sha256(seed, strlen(seed), date8, 8, k1);
	sift_hmac_sha256(k1, sizeof(k1), region, strlen(region), k2);
	sift_hmac_sha256(k2, sizeof(k2), service, strlen(service), k3);
	sift_hmac_sha256(k3, sizeof(k3), "aws4_request", 12, out);
	memset(seed, 0, sizeof(seed));
}

/* The value as a signature sees it: leading and trailing blanks off, every run of blanks one space */
static void
add_trimmed(Buf *b, const char *v)
{
	bool		gap = false,
				any = false;

	for (; *v; v++)
	{
		if (*v == ' ' || *v == '\t')
			gap = true;
		else
		{
			if (gap && any)
				buf_add(b, " ", 1);
			buf_add(b, v, 1);
			gap = false;
			any = true;
		}
	}
}

static int
hdr_cmp(const void *a, const void *b)
{
	return strcmp(((const SiftSigHdr *) a)->name, ((const SiftSigHdr *) b)->name);
}

static bool
inputs_ok(const SiftSigV4In *in, SiftTierErr *err)
{
	size_t		dl = in->amz_date ? strlen(in->amz_date) : 0;

	if (in->method != NULL && in->canon_uri != NULL && in->canon_query != NULL && in->payload_hash != NULL && in->region != NULL && in->service != NULL &&
		in->access_key != NULL && in->secret_key != NULL && in->nhdrs > 0 && dl == 16 && in->amz_date[8] == 'T' && in->amz_date[15] == 'Z')
		return true;
	sift_tier_fail(err, SIFT_TIER_E_ARG, "the signing request is incomplete");
	return false;
}

/* The headers sorted by name; NULL (and the reason) when a name is there twice */
static SiftSigHdr *
sorted_headers(const SiftSigV4In *in, SiftTierErr *err)
{
	SiftSigHdr *h = palloc(in->nhdrs * sizeof(SiftSigHdr));

	memcpy(h, in->hdrs, in->nhdrs * sizeof(SiftSigHdr));
	qsort(h, in->nhdrs, sizeof(SiftSigHdr), hdr_cmp);
	for (int i = 1; i < in->nhdrs; i++)
		if (strcmp(h[i - 1].name, h[i].name) == 0)
		{
			sift_tier_fail(err, SIFT_TIER_E_ARG, "header %s is signed twice", h[i].name);
			pfree(h);
			return NULL;
		}
	return h;
}

bool
sift_sigv4_sign(const SiftSigV4In *in, SiftSigV4Out *out, SiftTierErr *err)
{
	SiftSigHdr *h;
	Buf			canon = {0},
				signed_h = {0};
	char		date8[9],
				scope[160],
				chash[SIFT_SHA256_HEX],
				khex[SIFT_SHA256_HEX];
	uint8		digest[SIFT_SHA256_LEN],
				key[SIFT_SHA256_LEN];
	SiftSha256	c;

	memset(out, 0, sizeof(*out));
	if (!inputs_ok(in, err))
		return false;
	h = sorted_headers(in, err);
	if (h == NULL)
		return false;
	buf_str(&canon, in->method);
	buf_str(&canon, "\n");
	buf_str(&canon, in->canon_uri);
	buf_str(&canon, "\n");
	buf_str(&canon, in->canon_query);
	buf_str(&canon, "\n");
	for (int i = 0; i < in->nhdrs; i++)
	{
		buf_str(&canon, h[i].name);
		buf_str(&canon, ":");
		add_trimmed(&canon, h[i].value);
		buf_str(&canon, "\n");
		if (i > 0)
			buf_str(&signed_h, ";");
		buf_str(&signed_h, h[i].name);
	}
	buf_str(&canon, "\n");
	buf_str(&canon, signed_h.d);
	buf_str(&canon, "\n");
	buf_str(&canon, in->payload_hash);
	pfree(h);
	if (signed_h.n >= sizeof(out->signed_headers))
	{
		pfree(canon.d);
		pfree(signed_h.d);
		sift_tier_fail(err, SIFT_TIER_E_ARG, "the signed header list is too long");
		return false;
	}
	memcpy(out->signed_headers, signed_h.d, signed_h.n + 1);
	pfree(signed_h.d);

	memcpy(date8, in->amz_date, 8);
	date8[8] = '\0';
	snprintf(scope, sizeof(scope), "%s/%s/%s/aws4_request", date8, in->region, in->service);
	sift_sha256_init(&c);
	sift_sha256_update(&c, canon.d, canon.n);
	sift_sha256_final(&c, digest);
	sift_hex(digest, sizeof(digest), chash);
	snprintf(out->string_to_sign, sizeof(out->string_to_sign), ALGORITHM "\n%s\n%s\n%s", in->amz_date, scope, chash);
	sift_sigv4_signing_key(in->secret_key, date8, in->region, in->service, key);
	sift_hmac_sha256(key, sizeof(key), out->string_to_sign, strlen(out->string_to_sign), digest);
	memset(key, 0, sizeof(key));
	sift_hex(digest, sizeof(digest), khex);
	memcpy(out->signature, khex, sizeof(khex));
	snprintf(out->authorization, sizeof(out->authorization), ALGORITHM " Credential=%s/%s, SignedHeaders=%s, Signature=%s", in->access_key, scope,
			 out->signed_headers, out->signature);
	out->canonical_request = canon.d;
	return true;
}

void
sift_sigv4_out_free(SiftSigV4Out *out)
{
	if (out->canonical_request)
		pfree(out->canonical_request);
	out->canonical_request = NULL;
}
