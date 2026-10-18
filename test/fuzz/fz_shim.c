/*
 * fz_shim.c - the backend symbols the decoders need, for the fuzz targets (test/fuzz/README.md). Same idea as tools/colcodec/ccshim.c,
 * with the limits of the real allocator: palloc refuses more than MaxAllocSize (1 GB) unless the caller asks for huge, and every chunk is
 * its own malloc, so AddressSanitizer sees any overrun of a palloc'd object. ereport(ERROR) longjmps to fz_try(); the SQLSTATE of
 * the error decides whether it is the answer a decoder may give to corrupt bytes or a finding.
 */
#include "postgres.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "storage/buffile.h"

#include "sift.h"
#include "sift_colcodec.h"
#include "sift_col.h"
#include "sift_shred.h"
#include "sift_store.h"
#include "fz_shim.h"

extern void kt_cpu_init(void);	/* tools/kernels/shim.c */

MemoryContext CurrentMemoryContext = (MemoryContext) 1;

/* ---------- allocation: one malloc per chunk, all linked so that a statement's memory is freed together ---------- */

typedef struct FzChunk
{
	struct FzChunk *prev,
			   *next;
	size_t		size;
	size_t		off;			/* of the chunk header from the block (aligned allocations) */
} FzChunk;

static FzChunk fz_arena = {&fz_arena, &fz_arena, 0, 0};
static size_t fz_live;

#define FZ_MAX_LIVE ((size_t) 3 << 30)	/* the "server" is out of memory beyond this */

static void *
fz_alloc(size_t size, size_t align, int flags)
{
	FzChunk    *c;
	size_t		hdr = align > sizeof(FzChunk) ? align : sizeof(FzChunk);
	uint8	   *base;

	if (size > MaxAllocSize && !(flags & MCXT_ALLOC_HUGE))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("invalid memory alloc request size %zu", size)));
	size_t		al = align > 16 ? align : 16;

	if (size > FZ_MAX_LIVE || fz_live + size > FZ_MAX_LIVE || !(base = aligned_alloc(al, (hdr + size + al - 1) & ~(al - 1))))
	{
		if (flags & MCXT_ALLOC_NO_OOM)
			return NULL;
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory: request of %zu bytes", size)));
	}
	c = (FzChunk *) (base + hdr) - 1;
	c->size = size;
	c->off = hdr;
	c->next = fz_arena.next;
	c->prev = &fz_arena;
	fz_arena.next->prev = c;
	fz_arena.next = c;
	fz_live += size;
	if (flags & MCXT_ALLOC_ZERO)
		memset(c + 1, 0, size);
	return c + 1;
}

static void
fz_free(void *p)
{
	FzChunk    *c = (FzChunk *) p - 1;

	c->prev->next = c->next;
	c->next->prev = c->prev;
	fz_live -= c->size;
	free((uint8 *) p - c->off);
}

void
fz_keep(void)
{
	fz_arena.next = fz_arena.prev = &fz_arena;	/* what exists now is permanent; the chunks keep their stale links and are never freed */
}

void
fz_reset(void)
{
	while (fz_arena.next != &fz_arena)
		fz_free(fz_arena.next + 1);
}

void *
MemoryContextAllocExtended(MemoryContext c, Size n, int flags)
{
	return fz_alloc(n, 16, flags);
}

void *
MemoryContextAlloc(MemoryContext c, Size n)
{
	return fz_alloc(n, 16, 0);
}

void *
MemoryContextAllocZero(MemoryContext c, Size n)
{
	return fz_alloc(n, 16, MCXT_ALLOC_ZERO);
}

void *
MemoryContextAllocHuge(MemoryContext c, Size n)
{
	return fz_alloc(n, 16, MCXT_ALLOC_HUGE);
}

void *
MemoryContextAllocAligned(MemoryContext c, Size n, Size align, int flags)
{
	return fz_alloc(n, align, flags);
}

void *
palloc(Size n)
{
	return fz_alloc(n, 16, 0);
}

void *
palloc0(Size n)
{
	return fz_alloc(n, 16, MCXT_ALLOC_ZERO);
}

void *
repalloc(void *p, Size n)
{
	void	   *q = fz_alloc(n, 16, 0);
	size_t		old = ((FzChunk *) p - 1)->size;

	memcpy(q, p, old < n ? old : n);
	fz_free(p);
	return q;
}

void *
repalloc_huge(void *p, Size n)
{
	void	   *q = fz_alloc(n, 16, MCXT_ALLOC_HUGE);
	size_t		old = ((FzChunk *) p - 1)->size;

	memcpy(q, p, old < n ? old : n);
	fz_free(p);
	return q;
}

void
pfree(void *p)
{
	fz_free(p);
}

char *
pstrdup(const char *s)
{
	return strcpy(palloc(strlen(s) + 1), s);
}

char *
pnstrdup(const char *s, Size n)
{
	char	   *r = palloc(n + 1);

	memcpy(r, s, n);
	r[n] = '\0';
	return r;
}

MemoryContext
AllocSetContextCreateInternal(MemoryContext parent, const char *name, Size a, Size b, Size c)
{
	return (MemoryContext) 1;
}

void
MemoryContextDelete(MemoryContext c)
{
}

void
MemoryContextReset(MemoryContext c)
{
}

/* ---------- StringInfo ---------- */

void
initStringInfo(StringInfo s)
{
	s->maxlen = 1024;
	s->data = palloc(s->maxlen);
	s->len = 0;
	s->cursor = 0;
	s->data[0] = '\0';
}

void
resetStringInfo(StringInfo s)
{
	s->len = 0;
	s->cursor = 0;
	s->data[0] = '\0';
}

void
enlargeStringInfo(StringInfo s, int needed)
{
	if (needed < 0 || (Size) needed >= MaxAllocSize - (Size) s->len)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("string buffer too large")));
	if (s->len + needed + 1 <= s->maxlen)
		return;
	while (s->len + needed + 1 > s->maxlen)
		s->maxlen *= 2;
	s->data = repalloc(s->data, s->maxlen);
}

void
appendBinaryStringInfo(StringInfo s, const void *d, int n)
{
	enlargeStringInfo(s, n);
	memcpy(s->data + s->len, d, n);
	s->len += n;
	s->data[s->len] = '\0';
}

void
appendStringInfoString(StringInfo s, const char *str)
{
	appendBinaryStringInfo(s, str, (int) strlen(str));
}

void
appendStringInfoChar(StringInfo s, char c)
{
	appendBinaryStringInfo(s, &c, 1);
}

void
appendStringInfo(StringInfo s, const char *fmt,...)
{
	char		buf[256];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	appendStringInfoString(s, buf);
}

/* ---------- errors ---------- */

static jmp_buf fz_jmp;
static bool fz_armed;
int			fz_sqlstate;
char		fz_errmsg[256];
static char fz_errloc[128];

bool
errstart(int elevel, const char *domain)
{
	fz_sqlstate = 0;			/* elog(ERROR) never sets one: XX000 */
	return elevel >= ERROR;
}

bool
errstart_cold(int elevel, const char *domain)
{
	return errstart(elevel, domain);
}

int
errcode(int sqlerrcode)
{
	fz_sqlstate = sqlerrcode;
	return 0;
}

static int
fz_msg(const char *fmt, va_list ap)
{
	vsnprintf(fz_errmsg, sizeof(fz_errmsg), fmt, ap);
	return 0;
}

int
errmsg(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	fz_msg(fmt, ap);
	va_end(ap);
	return 0;
}

int
errmsg_internal(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	fz_msg(fmt, ap);
	va_end(ap);
	return 0;
}

int
errdetail(const char *fmt,...)
{
	return 0;
}

int
errdetail_internal(const char *fmt,...)
{
	return 0;
}

int
errhint(const char *fmt,...)
{
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	if (!fz_armed)
	{
		fprintf(stderr, "ereport(ERROR) outside fz_try at %s:%d: %s\n", filename, lineno, fz_errmsg);
		abort();
	}
	snprintf(fz_errloc, sizeof(fz_errloc), "%s:%d %s", filename, lineno, funcname);
	fz_armed = false;
	longjmp(fz_jmp, 1);
}

void
fz_bad(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	fputs("FINDING: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	abort();
}

/*
 * An error that is not of a corruption class (an elog(ERROR) is XX000, "internal error") is safe but wrong: the caller of a reader tells
 * corrupt data from a bug by the SQLSTATE. Reported once per message as FINDING-LOW; FZ_STRICT=1 makes it fatal.
 */
static void
fz_wrong_class(void)
{
	static uint64 seen[64];
	static int	nseen;
	uint64		h = 1469598103934665603ULL;

	for (const char *p = fz_errmsg; *p && !(*p >= '0' && *p <= '9'); p++)
		h = (h ^ (uint8) *p) * 1099511628211ULL;
	if (getenv("FZ_STRICT"))
		fz_bad("error of class %d (XX000 is 0) where corrupt input should raise a corruption error: %s", fz_sqlstate, fz_errmsg);
	for (int i = 0; i < nseen; i++)
		if (seen[i] == h)
			return;
	if (nseen < 64)
		seen[nseen++] = h;
	fprintf(stderr, "FINDING-LOW: SQLSTATE class %d (0 = XX000) for corrupt input: %s (%s)\n", fz_sqlstate, fz_errmsg, fz_errloc);
}

bool
fz_try(void (*fn) (void *), void *arg)
{
	if (setjmp(fz_jmp))
	{
		if (getenv("FZ_NOERR"))
			fz_bad("error %d where the input should be valid: %s (%s)", fz_sqlstate, fz_errmsg, fz_errloc);
		switch (fz_sqlstate)
		{
			case ERRCODE_DATA_CORRUPTED:
			case ERRCODE_INDEX_CORRUPTED:
			case ERRCODE_FEATURE_NOT_SUPPORTED:
			case ERRCODE_PROGRAM_LIMIT_EXCEEDED:
			case ERRCODE_INVALID_PARAMETER_VALUE:
			case ERRCODE_SYNTAX_ERROR:
			case ERRCODE_OUT_OF_MEMORY:
				return false;
			default:
				fz_wrong_class();
				return false;
		}
	}
	fz_armed = true;
	fn(arg);
	fz_armed = false;
	return true;
}

#ifdef USE_ASSERT_CHECKING
void
ExceptionalCondition(const char *conditionName, const char *fileName, int lineNumber)
{
	fprintf(stderr, "TRAP: failed Assert(\"%s\"), File: \"%s\", Line: %d\n", conditionName, fileName, lineNumber);
	abort();
}
#endif

/* ---------- CPU and dispatch ---------- */

void
fz_init(void)
{
	static bool done;

	if (done)
		return;
	done = true;
	kt_cpu_init();
	sift_col_kernels_init();
	sift_cc_kernels_init();
	sift_shred_init();
}

int
fz_levels(SiftSimdLevel lv[3])
{
	int			n = 0;

	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512; l++)
		if (!n || sift_cc_kernels_for((SiftSimdLevel) l) != sift_cc_kernels_for(lv[n - 1]) ||
			sift_col_kernels_for((SiftSimdLevel) l) != sift_col_kernels_for(lv[n - 1]) ||
			sift_shred_kernels_for((SiftSimdLevel) l) != sift_shred_kernels_for(lv[n - 1]))
			lv[n++] = (SiftSimdLevel) l;
	return n;
}

void
fz_use_level(SiftSimdLevel level)
{
	sift_cck = sift_cc_kernels_for(level);
	sift_colk = sift_col_kernels_for(level);
	sift_shred_kern = sift_shred_kernels_for(level);
}

bool
fz_skip(const char *name)
{
	const char *l = getenv("FZ_SKIP"),
			   *p;
	size_t		n = strlen(name);

	for (p = l; p && *p;)
	{
		const char *e = strchr(p, ',');
		size_t		len = e ? (size_t) (e - p) : strlen(p);

		if (len == n && !memcmp(p, name, n))
			return true;
		p = e ? e + 1 : p + len;
	}
	return false;
}

uint8 *
fz_copy(const uint8 *data, size_t len, size_t pad, size_t align)
{
	uint8	   *p = aligned_alloc(align, (len + pad + align - 1) & ~(align - 1));

	if (!p)
		abort();
	memcpy(p, data, len);
	memset(p + len, 0, pad);
	return p;
}

/* ---------- symbols the linked sources mention and the targets never reach ---------- */

#define UNREACHED(name) fz_bad("%s reached: the fuzz target links no storage layer", name)

BufFile *
BufFileCreateTemp(bool interXact)
{
	UNREACHED("BufFileCreateTemp");
}

void
BufFileClose(BufFile *f)
{
	UNREACHED("BufFileClose");
}

void
BufFileWrite(BufFile *f, const void *p, size_t n)
{
	UNREACHED("BufFileWrite");
}

uint64
sift_sw_tell(const SiftStreamWriter *sw)
{
	UNREACHED("sift_sw_tell");
}

void
sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len)
{
	UNREACHED("sift_sw_write");
}

void
sift_sw_write_file(SiftStreamWriter *sw, BufFile *file, uint64 len)
{
	UNREACHED("sift_sw_write_file");
}

/* The stream a target reads: one buffer, bounds-checked as the real reader checks a segment (sift_sr_get is the only place that knows the size) */
static const uint8 *fz_stream_buf;
static uint64 fz_stream_len;

void
fz_stream_set(const uint8 *buf, uint64 len)
{
	fz_stream_buf = buf;
	fz_stream_len = len;
}

const void *
sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch)
{
	if (off > fz_stream_len || len > fz_stream_len - off)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("sift: read of %zu bytes at %llu is outside the stream of %llu",
																	len, (unsigned long long) off, (unsigned long long) fz_stream_len)));
	return fz_stream_buf + off;
}

void
sift_sr_release(SiftStreamReader *sr)
{
}

void
sift_sr_unlock(SiftStreamReader *sr)
{
}

/* sift_analyze.c wants the database encoding and the index options of the backend */
int
GetDatabaseEncoding(void)
{
	return PG_UTF8;
}

const char *
GetDatabaseEncodingName(void)
{
	return "UTF8";
}

const SiftOptions *
sift_index_options(Relation index)
{
	UNREACHED("sift_index_options");
}

const char *
sift_options_stemmer(const SiftOptions *opts)
{
	UNREACHED("sift_options_stemmer");
}

const char *
sift_options_fields(const SiftOptions *opts)
{
	UNREACHED("sift_options_fields");
}
