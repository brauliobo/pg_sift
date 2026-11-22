/*
 * col/f2/f2_tier_int.h - what the files of the object store engine share and the rest of the build does not see: the location parser and the libcurl that is opened at run time.
 */
#ifndef SIFT_F2_TIER_INT_H
#define SIFT_F2_TIER_INT_H

#include "col/f2/f2_tier.h"

#define SIFT_TIER_CHUNK		(UINT32_C(1) << 20)

typedef struct SiftTierUri
{
	bool		is_file;
	bool		is_r2;
	char		bucket[64];
	char		path[480];		/* file: the absolute directory; s3 and r2: the key prefix without a leading or trailing slash, possibly empty */
} SiftTierUri;

extern SiftTierStatus sift_tier_uri_parse(const char *uri, SiftTierUri *u, SiftTierErr *err);

/* An object name the backends accept: the alphabet of a location, no "..", no leading or doubled slash, at most 160 bytes */
extern bool sift_tier_key_ok(const char *key);

/* ---- libcurl, opened with dlopen ---- */

typedef struct SiftCurl SiftCurl;

typedef struct SiftCurlReq
{
	const char *method;			/* GET, PUT, HEAD or DELETE */
	const char *url;
	const char *const *headers;	/* "Name: value" */
	int			nheaders;
	const SiftTierSrc *upload;	/* the body of a PUT */
	char	   *dst;			/* the body of a response goes here, at most dst_cap bytes of it ... */
	uint64		dst_cap;
	char	  **grow;			/* ... or into a palloc'd NUL ended string that grows (a listing) */
	uint64		grow_len;
	/* what came back */
	long		status;
	uint64		received;
	uint64		content_length;	/* UINT64_MAX when the response has none */
	char		etag[SIFT_TIER_ETAG_LEN];
} SiftCurlReq;

/* ERROR free: SIFT_TIER_E_NO_CURL (and the dlerror text) when the library or one of the functions is not there */
extern SiftTierStatus sift_curl_open(const char *soname, SiftCurl **out, SiftTierErr *err);
extern void sift_curl_close(SiftCurl *c);

/*
 * One request. A host error raised by the upload source (a part that cannot be read) is held back until curl is out of the way and then raised again; a pending interrupt stops the transfer
 * and is raised the same way. Any other failure is a status.
 */
extern SiftTierStatus sift_curl_perform(SiftCurl *c, SiftCurlReq *r, SiftTierErr *err);

#endif							/* SIFT_F2_TIER_INT_H */
