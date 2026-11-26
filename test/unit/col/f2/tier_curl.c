/*
 * test/unit/col/f2/tier_curl.c - the bound of a transfer of the s3:// backend (col/f2/tier/curl.c): a store that answers one byte a second never trips the low-speed limit, so every request
 * carries a total timeout, a minute and one second more for each 256 KiB of its body, and the maintenance that holds its locks across a transfer waits that long at most.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include <stdarg.h>

#include "test/common/bench.h"

#include "col/f2/tier/curl.c"

static long timeout_s = -1;

static CURLcode
fake_setopt(CURL *h, int opt, ...)
{
	va_list		ap;

	va_start(ap, opt);
	if (opt == 13)
		timeout_s = va_arg(ap, long);
	va_end(ap);
	return SC_OK;
}

static CURLcode
fake_perform(CURL *h)
{
	return SC_OK;
}

static CURLcode
fake_getinfo(CURL *h, int info, ...)
{
	va_list		ap;

	va_start(ap, info);
	*va_arg(ap, long *) = 200;
	va_end(ap);
	return SC_OK;
}

static void
fake_reset(CURL *h)
{
}

static struct curl_slist *
fake_append(struct curl_slist *l, const char *s)
{
	return l;
}

static void
fake_free_all(struct curl_slist *l)
{
}

static void
src_read(void *arg, uint64 off, uint64 n, void *dst)
{
}

static long
timeout_of(const char *method, uint64 body)
{
	SiftCurl	c = {.easy_setopt = fake_setopt,.easy_perform = fake_perform,.easy_getinfo = fake_getinfo,.easy_reset = fake_reset,.slist_append = fake_append,
	.slist_free_all = fake_free_all};
	SiftTierSrc src = {.len = body,.read = src_read};
	char		dst[16];
	SiftCurlReq r = {.method = method,.url = "http://192.0.2.1/bucket/key",.upload = strcmp(method, "PUT") == 0 ? &src : NULL,.dst = dst,
	.dst_cap = strcmp(method, "GET") == 0 ? body : 0};
	SiftTierErr err = {0};

	timeout_s = -1;
	SIFT_CHECKF(sift_curl_perform(&c, &r, &err) == SIFT_TIER_OK, "the %s goes through", method);
	return timeout_s;
}

static void
test_every_transfer_has_a_total_timeout(void)
{
	SIFT_CHECKF(timeout_of("HEAD", 0) == 60, "a HEAD may take a minute (%ld)", timeout_of("HEAD", 0));
	SIFT_CHECKF(timeout_of("GET", UINT64_C(16) << 20) == 60 + 64, "a GET of 16 MiB may take a minute and 64 seconds (%ld)", timeout_of("GET", UINT64_C(16) << 20));
	SIFT_CHECKF(timeout_of("PUT", UINT64_C(256) << 20) == 60 + 1024, "a PUT of 256 MiB may take a minute and 1024 seconds (%ld)", timeout_of("PUT", UINT64_C(256) << 20));
}

int
main(void)
{
	test_every_transfer_has_a_total_timeout();
	return sift_check_summary();
}
