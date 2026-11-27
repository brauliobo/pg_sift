/*
 * col/f2/tier/curl.c - one HTTP request over a libcurl that is opened with dlopen("libcurl.so.4" or "libcurl.4.dylib") the first time a s3:// or r2:// location is used: the build links no HTTP library, as it
 * links no Vulkan, and a host without libcurl reads and writes file:// locations and gets one clear error for the others. Only the few entry points and option numbers of the easy interface
 * that the request needs are declared here (the values are those of curl.h, which tests/test_curl_abi.c compares with the header of the host when there is one).
 */
#include "core/host.h"

#include <dlfcn.h>

#include "miscadmin.h"

#include "col/f2/tier/curlabi.h"
#include "col/f2/tier/int.h"
#include "core/waits.h"

struct SiftCurl
{
	void	   *lib;
	CURL	   *easy;
	CURLcode	(*global_init) (long flags);
	CURL	   *(*easy_init) (void);
	CURLcode	(*easy_setopt) (CURL *h, int opt, ...);
	CURLcode	(*easy_perform) (CURL *h);
	CURLcode	(*easy_getinfo) (CURL *h, int info, ...);
	void		(*easy_reset) (CURL *h);
	void		(*easy_cleanup) (CURL *h);
	const char *(*easy_strerror) (CURLcode c);
	struct curl_slist *(*slist_append) (struct curl_slist *l, const char *s);
	void		(*slist_free_all) (struct curl_slist *l);
};

typedef struct Xfer
{
	SiftCurlReq *r;
	uint64		sent;
	ErrorData  *held;
	long		hdr_status;		/* the status line of the last response header block */
	long		retry_after_s;	/* the Retry-After of the response, 0 when it has none */
	uint64		grow_cap;		/* the bytes *r->grow was sized to by this transfer: it doubles, so a listing is copied O(log n) times */
	bool		overflow;
} Xfer;

#define RETRY_ATTEMPTS		4
#define BACKOFF_FIRST_MS	100
#define BACKOFF_MAX_MS		5000
#define FLOOR_SECS			60	/* every transfer may take this long, and one second more for each FLOOR_BYTES_PER_SEC of its body */
#define FLOOR_BYTES_PER_SEC	(UINT64_C(256) << 10)
#define GROW_MAX			(UINT64_C(64) << 20)

/* The host's work that may raise, run where curl cannot be jumped out of: the error is kept to be raised again when curl is out of the way */
static ErrorData *
run_guarded(void (*fn) (void *arg), void *arg)
{
	ErrorData *volatile held = NULL;
	MemoryContext cx = CurrentMemoryContext;

	PG_TRY();
	{
		fn(arg);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(cx);
		held = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	return held;
}

typedef struct GrowArg
{
	Xfer	   *x;
	const char *src;
	uint64		n;
} GrowArg;

static void
grow_append(void *arg)
{
	GrowArg    *a = arg;
	SiftCurlReq *r = a->x->r;
	char	  **buf = r->grow;
	uint64		need = r->grow_len + a->n + 1;

	if (need > a->x->grow_cap)
	{
		a->x->grow_cap = Min(Max(a->x->grow_cap * 2, need), GROW_MAX + 1);
		*buf = *buf != NULL ? repalloc(*buf, a->x->grow_cap) : palloc(a->x->grow_cap);
	}
	memcpy(*buf + r->grow_len, a->src, a->n);
	(*buf)[r->grow_len + a->n] = '\0';
}

SiftTierStatus
sift_curl_open(const char *soname, SiftCurl **out, SiftTierErr *err)
{
#ifdef __APPLE__
	const char *name = soname ? soname : "libcurl.4.dylib";
#else
	const char *name = soname ? soname : "libcurl.so.4";
#endif
	SiftCurl   *c = palloc0(sizeof(SiftCurl));
	struct
	{
		const char *sym;
		void	  **at;
	}			syms[] = {
		{"curl_global_init", (void **) &c->global_init}, {"curl_easy_init", (void **) &c->easy_init}, {"curl_easy_setopt", (void **) &c->easy_setopt},
		{"curl_easy_perform", (void **) &c->easy_perform}, {"curl_easy_getinfo", (void **) &c->easy_getinfo}, {"curl_easy_reset", (void **) &c->easy_reset},
		{"curl_easy_cleanup", (void **) &c->easy_cleanup}, {"curl_easy_strerror", (void **) &c->easy_strerror}, {"curl_slist_append", (void **) &c->slist_append},
		{"curl_slist_free_all", (void **) &c->slist_free_all}
	};

	*out = NULL;
	c->lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
	if (c->lib == NULL)
	{
		pfree(c);
		return sift_tier_fail(err, SIFT_TIER_E_NO_CURL, "%s cannot be loaded: s3:// and r2:// locations need libcurl on the server host; install it or use a file:// location", name);
	}
	for (size_t i = 0; i < lengthof(syms); i++)
	{
		*syms[i].at = dlsym(c->lib, syms[i].sym);
		if (*syms[i].at == NULL)
		{
			dlclose(c->lib);
			pfree(c);
			return sift_tier_fail(err, SIFT_TIER_E_NO_CURL, "%s has no %s: it is not a libcurl this build can use", name, syms[i].sym);
		}
	}
	if (c->global_init(SC_GLOBAL_DEFAULT) != SC_OK || (c->easy = c->easy_init()) == NULL)
	{
		dlclose(c->lib);
		pfree(c);
		return sift_tier_fail(err, SIFT_TIER_E_NO_CURL, "%s did not initialize", name);
	}
	*out = c;
	return SIFT_TIER_OK;
}

void
sift_curl_close(SiftCurl *c)
{
	c->easy_cleanup(c->easy);
	dlclose(c->lib);
	pfree(c);
}

static size_t
write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
	Xfer	   *x = ud;
	SiftCurlReq *r = x->r;
	uint64		n = (uint64) size * nmemb;

	if (r->grow != NULL)
	{
		GrowArg		a = {x, ptr, n};

		if (r->grow_len + n > GROW_MAX)
		{
			x->overflow = true;
			return 0;
		}
		x->held = run_guarded(grow_append, &a);
		if (x->held != NULL)
			return 0;
		r->grow_len += n;
	}
	else if (r->dst != NULL && x->hdr_status / 100 != 2)
		return n;
	else if (r->dst != NULL && r->received + n <= r->dst_cap)
		memcpy(r->dst + r->received, ptr, n);
	else if (r->dst != NULL)
	{
		x->overflow = true;
		return 0;
	}
	r->received += n;
	return n;
}

static size_t
header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
	Xfer	   *x = ud;
	SiftCurlReq *r = x->r;
	size_t		n = size * nmemb;
	char		line[256];
	const char *sp;

	if (n >= sizeof(line))
		return n;
	memcpy(line, ptr, n);
	line[n] = '\0';
	if (strncmp(line, "HTTP/", 5) == 0 && (sp = strchr(line, ' ')) != NULL)
		x->hdr_status = strtol(sp, NULL, 10);
	else if (pg_strncasecmp(line, "content-length:", 15) == 0)
		r->content_length = strtoull(line + 15, NULL, 10);
	else if (pg_strncasecmp(line, "retry-after:", 12) == 0)
		x->retry_after_s = strtol(line + 12, NULL, 10);
	else if (pg_strncasecmp(line, "etag:", 5) == 0)
	{
		const char *v = line + 5;
		size_t		k = 0;

		while (*v == ' ' || *v == '"')
			v++;
		while (*v && *v != '"' && *v != '\r' && *v != '\n' && k + 1 < sizeof(r->etag))
			r->etag[k++] = *v++;
		r->etag[k] = '\0';
	}
	return n;
}

typedef struct ReadArg
{
	const SiftTierSrc *src;
	uint64		off;
	uint64		n;
	char	   *buf;
} ReadArg;

static void
read_source(void *arg)
{
	ReadArg    *a = arg;

	a->src->read(a->src->arg, a->off, a->n, a->buf);
}

static size_t
read_cb(char *buf, size_t size, size_t nitems, void *ud)
{
	Xfer	   *x = ud;
	uint64		n = Min((uint64) size * nitems, x->r->upload->len - x->sent);
	ReadArg		a = {x->r->upload, x->sent, n, buf};

	if (n == 0)
		return 0;
	x->held = run_guarded(read_source, &a);
	if (x->held != NULL)
		return SC_READFUNC_ABORT;
	x->sent += n;
	return n;
}

static int
progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	(void) ud;
	(void) dltotal;
	(void) dlnow;
	(void) ultotal;
	(void) ulnow;
	return InterruptPending ? 1 : 0;
}

static CURLcode
set_options(SiftCurl *c, SiftCurlReq *r, Xfer *x, struct curl_slist *hdrs)
{
	CURLcode	rc = c->easy_setopt(c->easy, SC_URL, r->url);

	rc = rc ? rc : c->easy_setopt(c->easy, SC_FOLLOWLOCATION, 0L);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_PROTOCOLS, SC_PROTO_HTTP_HTTPS);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_NOSIGNAL, 1L);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_CONNECTTIMEOUT, 10L);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_LOW_SPEED_LIMIT, 1L);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_LOW_SPEED_TIME, 60L);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_TIMEOUT, (long) (FLOOR_SECS + (r->upload != NULL ? r->upload->len : r->dst_cap) / FLOOR_BYTES_PER_SEC));
	rc = rc ? rc : c->easy_setopt(c->easy, SC_USERAGENT, "pg_sift-tier");
	rc = rc ? rc : c->easy_setopt(c->easy, SC_HTTPHEADER, hdrs);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_WRITEFUNCTION, write_cb);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_WRITEDATA, x);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_HEADERFUNCTION, header_cb);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_HEADERDATA, x);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_NOPROGRESS, 0L);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_XFERINFOFUNCTION, progress_cb);
	rc = rc ? rc : c->easy_setopt(c->easy, SC_XFERINFODATA, x);
	if (rc)
		return rc;
	if (strcmp(r->method, "HEAD") == 0)
		return c->easy_setopt(c->easy, SC_NOBODY, 1L);
	if (strcmp(r->method, "DELETE") == 0)
		return c->easy_setopt(c->easy, SC_CUSTOMREQUEST, "DELETE");
	if (strcmp(r->method, "PUT") == 0)
	{
		rc = c->easy_setopt(c->easy, SC_UPLOAD, 1L);
		rc = rc ? rc : c->easy_setopt(c->easy, SC_INFILESIZE_LARGE, (curl_off_t) r->upload->len);
		rc = rc ? rc : c->easy_setopt(c->easy, SC_READFUNCTION, read_cb);
		return rc ? rc : c->easy_setopt(c->easy, SC_READDATA, x);
	}
	return SC_OK;
}

static bool
transient_error(CURLcode rc)
{
	return rc == SC_COULDNT_RESOLVE_HOST || rc == SC_COULDNT_CONNECT || rc == SC_OPERATION_TIMEDOUT || rc == SC_GOT_NOTHING || rc == SC_SEND_ERROR || rc == SC_RECV_ERROR;
}

static bool
transient_status(long status)
{
	return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

/* One transfer; *again: the request may be sent again (a transient failure of the transport or of the store), *retry_after_s: what the store asked to wait */
static SiftTierStatus
perform_once(SiftCurl *c, SiftCurlReq *r, SiftTierErr *err, bool *again, long *retry_after_s)
{
	Xfer		x = {.r = r};
	struct curl_slist *hdrs = NULL;
	CURLcode	rc;
	long		status = 0;

	r->status = 0;
	r->received = 0;
	r->grow_len = 0;
	if (r->grow != NULL && *r->grow != NULL)
		(*r->grow)[0] = '\0';
	r->content_length = UINT64_MAX;
	r->etag[0] = '\0';
	c->easy_reset(c->easy);
	for (int i = 0; i < r->nheaders; i++)
		hdrs = c->slist_append(hdrs, r->headers[i]);
	if (strcmp(r->method, "PUT") == 0)
		hdrs = c->slist_append(hdrs, "Expect:");
	rc = set_options(c, r, &x, hdrs);
	if (rc == SC_OK)
		rc = c->easy_perform(c->easy);
	if (rc == SC_OK)
		c->easy_getinfo(c->easy, SC_INFO_RESPONSE_CODE, &status);
	c->slist_free_all(hdrs);
	r->status = status;
	*retry_after_s = x.retry_after_s;
	*again = (rc == SC_OK && transient_status(status)) || transient_error(rc);
	if (x.held != NULL)
		ReThrowError(x.held);
	if (rc == SC_ABORTED_BY_CALLBACK)
	{
		CHECK_FOR_INTERRUPTS();
		return sift_tier_fail(err, SIFT_TIER_E_IO, "the transfer was interrupted");
	}
	if (rc != SC_OK)
		return sift_tier_fail(err, SIFT_TIER_E_IO, "the transfer failed: %s", x.overflow ? "the response is longer than expected" : c->easy_strerror(rc));
	return SIFT_TIER_OK;
}

SiftTierStatus
sift_curl_perform(SiftCurl *c, SiftCurlReq *r, SiftTierErr *err)
{
	for (int attempt = 1;; attempt++)
	{
		bool		again;
		long		retry_after_s;
		SiftTierStatus st = perform_once(c, r, err, &again, &retry_after_s);

		if (!again || attempt == RETRY_ATTEMPTS)
			return st;
		sift_wait(SIFT_WAIT_NAP, Max(Min((long) BACKOFF_FIRST_MS << (attempt - 1), BACKOFF_MAX_MS), Min(retry_after_s * 1000, BACKOFF_MAX_MS)));
	}
}
