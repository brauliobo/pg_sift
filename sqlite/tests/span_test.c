/*
 * span_test.c - unit and adversarial tests of src/sift_span.h (docs/SAFETY.md). No host, no database: the header is C library only.
 * Built by the portable flags of sqlite/Makefile (make span-test, make SIMD=none B=build-scalar span-test, make span-test-asan),
 * run by test.sh. The failure hook is the test's own: it longjmps when a check is expected to fail and aborts when it is not, so
 * a check that fires by mistake or one that does not fire both fail the run.
 *
 *   - table of boundaries: every operation at len - 1, len, len + 1 and at offsets and counts that wrap size_t
 *   - the buffers are exact malloc blocks (ASan sees any over-read) and a block that ends at a PROT_NONE page (a read past the
 *     end is a SIGSEGV even without a sanitizer)
 *   - the varint: every length, the 64 bit extremes, overlong and truncated forms, against the encoder of sift_varint.h
 *   - differential: random operation sequences on random buffers against a model that does its arithmetic in 128 bits
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sift_span.h"
#include "sift_varint.h"

static jmp_buf	fail_jmp;
static int		expecting;
static unsigned long fails_seen,
			checks;

void
sift_span_fail(const char *what, size_t need, size_t have)
{
	if (!expecting)
	{
		fprintf(stderr, "span_test: unexpected failure: %s needs %zu, %zu left\n", what, need, have);
		abort();
	}
	fails_seen++;
	longjmp(fail_jmp, 1);
}

#define MUST_FAIL(stmt) \
	do { \
		volatile int failed_ = 0; \
		checks++; \
		expecting = 1; \
		if (setjmp(fail_jmp) == 0) { stmt; } else failed_ = 1; \
		expecting = 0; \
		if (!failed_) { fprintf(stderr, "span_test: %s:%d: did not fail: %s\n", __FILE__, __LINE__, #stmt); abort(); } \
	} while (0)

#define MUST_PASS(stmt) do { checks++; stmt; } while (0)

#define CHECK(cond) \
	do { checks++; if (!(cond)) { fprintf(stderr, "span_test: %s:%d: %s\n", __FILE__, __LINE__, #cond); abort(); } } while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t
rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

/* n bytes of random content in an exact malloc block */
static uint8_t *
heap_bytes(size_t n)
{
	uint8_t    *p = malloc(n ? n : 1);

	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t) rnd();
	return p;
}

/* n bytes that end exactly at a PROT_NONE page */
static uint8_t *
guard_bytes(size_t n, void **base, size_t *maplen)
{
	size_t		pg = (size_t) sysconf(_SC_PAGESIZE),
				pages = (n + pg - 1) / pg + 1;
	uint8_t    *m = mmap(NULL, pages * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint8_t    *p;

	CHECK(m != MAP_FAILED);
	CHECK(mprotect(m + (pages - 1) * pg, pg, PROT_NONE) == 0);
	p = m + (pages - 1) * pg - n;
	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t) rnd();
	*base = m;
	*maplen = pages * pg;
	return p;
}

static void
test_basics(void)
{
	static const uint8_t b[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	SiftSpan	s = sift_span_make(b, sizeof(b));

	CHECK(sift_span_remaining(s) == 16 && !sift_span_empty(s));
	CHECK(sift_span_u8_at(s, 15) == 16);
	CHECK(sift_span_u16le_at(s, 0) == 0x0201);
	CHECK(sift_span_u32le_at(s, 1) == 0x05040302u);
	CHECK(sift_span_u64le_at(s, 8) == UINT64_C(0x100f0e0d0c0b0a09));
	CHECK(sift_span_remaining(s) == 16);	/* *_at does not consume */

	CHECK(sift_span_read_u8(&s) == 1);
	CHECK(sift_span_read_u16le(&s) == 0x0302);
	CHECK(sift_span_read_u32le(&s) == 0x07060504u);
	CHECK(sift_span_read_u64le(&s) == UINT64_C(0x0f0e0d0c0b0a0908));
	CHECK(sift_span_remaining(s) == 1 && s.ptr == b + 15);
	CHECK(sift_span_read_u8(&s) == 16 && sift_span_empty(s));

	s = sift_span_make(b, sizeof(b));
	SiftSpan	t = sift_span_take(&s, 6);
	CHECK(t.ptr == b && t.len == 6 && s.ptr == b + 6 && s.len == 10);
	SiftSpan	u = sift_span_sub(s, 2, 8);
	CHECK(u.ptr == b + 8 && u.len == 8);
	u = sift_span_array(s, 2, 2, 4);
	CHECK(u.ptr == b + 8 && u.len == 8);
	sift_span_skip(&s, 10);
	CHECK(sift_span_empty(s));
	{
		uint8_t		out[4];
		SiftSpan	r = sift_span_make(b, sizeof(b));

		sift_span_read_bytes(&r, out, 4);
		CHECK(out[3] == 4 && r.len == 12);
	}
	CHECK(sift_span_is_aligned(sift_span_make(b, 4), 1));
}

/* every operation at the edge of an 8 byte span, and far past it */
static void
test_boundaries(void)
{
	uint8_t    *b = heap_bytes(8);
	SiftSpan	s = sift_span_make(b, 8),
				r,
				e = sift_span_make(b + 8, 0);

	MUST_PASS(sift_span_u8_at(s, 7));
	MUST_FAIL(sift_span_u8_at(s, 8));
	MUST_PASS(sift_span_u16le_at(s, 6));
	MUST_FAIL(sift_span_u16le_at(s, 7));
	MUST_PASS(sift_span_u32le_at(s, 4));
	MUST_FAIL(sift_span_u32le_at(s, 5));
	MUST_PASS(sift_span_u64le_at(s, 0));
	MUST_FAIL(sift_span_u64le_at(s, 1));
	MUST_FAIL(sift_span_u8_at(s, SIZE_MAX));
	MUST_FAIL(sift_span_u16le_at(s, SIZE_MAX));
	MUST_FAIL(sift_span_u32le_at(s, SIZE_MAX - 1));
	MUST_FAIL(sift_span_u64le_at(s, SIZE_MAX - 3));
	MUST_FAIL(sift_span_u64le_at(s, SIZE_MAX - 7));	/* off + 8 wraps to 0 */

	MUST_PASS(r = sift_span_sub(s, 0, 8));
	MUST_PASS(r = sift_span_sub(s, 8, 0));		/* the empty span at the end */
	MUST_FAIL(r = sift_span_sub(s, 9, 0));
	MUST_FAIL(r = sift_span_sub(s, 0, 9));
	MUST_FAIL(r = sift_span_sub(s, 4, 5));
	MUST_FAIL(r = sift_span_sub(s, 1, SIZE_MAX));		/* 1 + SIZE_MAX wraps to 0 */
	MUST_FAIL(r = sift_span_sub(s, SIZE_MAX, 1));
	MUST_FAIL(r = sift_span_sub(s, SIZE_MAX, 2));		/* wraps to 1 */
	MUST_FAIL(r = sift_span_sub(s, SIZE_MAX - 3, 8));	/* wraps to 4 */
	MUST_FAIL(r = sift_span_sub(s, 8, SIZE_MAX));

	MUST_PASS(r = sift_span_array(s, 0, 2, 4));
	MUST_PASS(r = sift_span_array(s, 0, 0, 4));
	MUST_PASS(r = sift_span_array(s, 8, 5, 0));		/* stride 0: zero bytes whatever the count */
	MUST_FAIL(r = sift_span_array(s, 0, 3, 4));
	MUST_FAIL(r = sift_span_array(s, 0, SIZE_MAX, 8));	/* count * stride wraps to a small number */
	MUST_FAIL(r = sift_span_array(s, 0, SIZE_MAX / 2 + 1, 2));	/* wraps to exactly 0 */
	MUST_FAIL(r = sift_span_array(s, 0, (SIZE_MAX >> 1) + 2, 2));	/* wraps to 2 */
	MUST_FAIL(r = sift_span_array(s, 0, UINT32_MAX, UINT32_MAX));
	MUST_FAIL(r = sift_span_array(s, 1, 1, 8));
	(void) r;

	{
		SiftSpan	t = s;

		MUST_FAIL(sift_span_skip(&t, 9));
		CHECK(t.len == 8);							/* a failed skip changes nothing */
		MUST_FAIL(sift_span_skip(&t, SIZE_MAX));
		MUST_PASS(sift_span_skip(&t, 8));
		MUST_PASS(sift_span_skip(&t, 0));
		MUST_FAIL(sift_span_skip(&t, 1));
		t = s;
		MUST_FAIL(r = sift_span_take(&t, 9));
		CHECK(t.len == 8 && t.ptr == b);
		MUST_FAIL(r = sift_span_take(&t, SIZE_MAX));
		MUST_PASS(r = sift_span_take(&t, 8));
		CHECK(t.len == 0 && r.len == 8);
		MUST_PASS(r = sift_span_take(&t, 0));
		MUST_FAIL(r = sift_span_take(&t, 1));
	}
	/* a short read leaves the span alone and a read of the last bytes empties it */
	{
		SiftSpan	t = sift_span_make(b, 3);

		MUST_FAIL(sift_span_read_u32le(&t));
		CHECK(t.len == 3 && t.ptr == b);
		MUST_PASS(sift_span_read_u16le(&t));
		MUST_FAIL(sift_span_read_u16le(&t));
		CHECK(t.len == 1);
		MUST_PASS(sift_span_read_u8(&t));
		MUST_FAIL(sift_span_read_u8(&t));
		MUST_FAIL(sift_span_read_u64le(&e));
		MUST_FAIL(sift_span_read_varint(&e));
		MUST_FAIL(sift_span_read_bytes(&t, b, 1));
	}
	/* the try forms say the same without calling the hook, and leave *out alone */
	{
		SiftSpan	out = {(const uint8_t *) 1, 77},
					t = s;
		uint32_t	v = 5;

		expecting = 0;							/* a call of the hook would abort */
		CHECK(!sift_span_try_sub(s, 9, 0, &out) && out.len == 77 && out.ptr == (const uint8_t *) 1);
		CHECK(!sift_span_try_sub(s, 1, SIZE_MAX, &out) && out.len == 77);
		CHECK(!sift_span_try_array(s, 0, SIZE_MAX, 8, &out) && out.len == 77);
		CHECK(sift_span_try_array(s, 0, 2, 4, &out) && out.len == 8);
		CHECK(!sift_span_try_take(&t, 9, &out) && t.len == 8);
		CHECK(!sift_span_try_u32le_at(s, 5, &v) && v == 5);
		CHECK(!sift_span_try_read_u32le(&e, &v) && v == 5);
		CHECK(sift_span_try_read_u32le(&t, &v) && t.len == 4);
	}
	free(b);
}

/* a NULL pointer with a length is an empty span: nothing is read from it */
static void
test_null(void)
{
	SiftSpan	s = sift_span_make(NULL, 1000),
				r;

	CHECK(sift_span_remaining(s) == 0 && sift_span_empty(s));
	MUST_FAIL(sift_span_u8_at(s, 0));
	MUST_FAIL(sift_span_read_u8(&s));
	MUST_PASS(r = sift_span_sub(s, 0, 0));
	MUST_FAIL(r = sift_span_sub(s, 0, 1));
	(void) r;
}

static void
varint_ok(const uint8_t *buf, size_t len, uint64_t want, size_t used)
{
	SiftSpan	s = sift_span_make(buf, len);
	uint64_t	v = ~want;

	CHECK(sift_span_try_read_varint(&s, &v) && v == want && s.len == len - used);
}

static void
test_varint(void)
{
	uint8_t		b[16];
	uint64_t	vals[] = {0, 1, 127, 128, 255, 16383, 16384, UINT32_MAX, UINT64_C(1) << 32, (UINT64_C(1) << 63) - 1,
	UINT64_C(1) << 63, UINT64_MAX};

	for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
	{
		size_t		n = (size_t) (sift_put_varint(b, vals[i]) - b);
		SiftSpan	s = sift_span_make(b, n);

		varint_ok(b, n, vals[i], n);
		CHECK(sift_span_read_varint(&s) == vals[i] && sift_span_empty(s));
		for (size_t cut = 0; cut < n; cut++)		/* every truncation fails and changes nothing */
		{
			SiftSpan	t = sift_span_make(b, cut);
			uint64_t	v;

			CHECK(!sift_span_try_read_varint(&t, &v) && t.len == cut);
		}
	}
	/* random values round trip, with the encoder at the end of a guard page */
	for (int i = 0; i < 20000; i++)
	{
		uint64_t	v = rnd() >> (rnd() % 64);
		size_t		n = (size_t) (sift_put_varint(b, v) - b);
		void	   *base;
		size_t		maplen;
		uint8_t    *g = guard_bytes(n, &base, &maplen);
		SiftSpan	s;

		memcpy(g, b, n);
		s = sift_span_make(g, n);
		CHECK(sift_span_read_varint(&s) == v && s.len == 0);
		munmap(base, maplen);
	}
	/* ten bytes that overflow 64 bits, eleven bytes, a stored zero tail: corrupt or at least bounded */
	memset(b, 0xff, sizeof(b));
	{
		SiftSpan	s = sift_span_make(b, 16);
		uint64_t	v;

		CHECK(!sift_span_try_read_varint(&s, &v) && s.len == 16);	/* tenth byte 0xff: bit 64 and up set */
		b[9] = 0x02;
		CHECK(!sift_span_try_read_varint(&s, &v));
		b[9] = 0x01;
		varint_ok(b, 16, UINT64_MAX, 10);
		b[9] = 0x80;
		CHECK(!sift_span_try_read_varint(&s, &v));					/* continuation past ten bytes */
		memset(b, 0x80, sizeof(b));
		CHECK(!sift_span_try_read_varint(&s, &v));					/* all continuation: runs to the end, then past ten */
		b[0] = 0x80, b[1] = 0x00;
		varint_ok(b, 16, 0, 2);									/* non-canonical zero is accepted, as before */
	}
	{
		uint8_t		big[10];
		SiftSpan	s;
		uint32_t	v32 = 9;

		sift_put_varint(big, UINT64_C(1) << 32);
		s = sift_span_make(big, sizeof(big));
		CHECK(!sift_span_try_read_varint32(&s, &v32) && v32 == 9 && s.len == 10);
		MUST_FAIL(sift_span_read_varint32(&s));
		sift_put_varint(big, UINT32_MAX);
		s = sift_span_make(big, sizeof(big));
		CHECK(sift_span_read_varint32(&s) == UINT32_MAX && s.len == 5);
	}
}

/* the model: the same questions in 128 bit arithmetic, on the plain length */
#ifdef __SIZEOF_INT128__
typedef unsigned __int128 wide;
#else
typedef unsigned long long wide;
#endif

static bool
model_fits(size_t len, size_t off, size_t n)
{
	return (wide) off + (wide) n <= (wide) len;
}

static size_t
pick(size_t len)
{
	switch (rnd() % 8)
	{
		case 0: return SIZE_MAX - rnd() % 16;
		case 1: return SIZE_MAX / 2 + rnd() % 3;
		case 2: return len + rnd() % 3;
		case 3: return len ? len - 1 - rnd() % len : 0;
		case 4: return (size_t) (UINT32_MAX - rnd() % 3);
		default: return rnd() % (len + 2);
	}
}

static void
test_differential(void)
{
	for (int it = 0; it < 200000; it++)
	{
		size_t		len = rnd() % 40;
		uint8_t    *b = heap_bytes(len);
		SiftSpan	s = sift_span_make(b, len),
					r;
		size_t		off = pick(len),
					n = pick(len),
					stride = rnd() % 9;
		uint32_t	v32 = 0;
		uint64_t	v64 = 0;

		CHECK(sift_span_try_sub(s, off, n, &r) == model_fits(len, off, n));
		if (model_fits(len, off, n))
			CHECK(r.ptr == b + off && r.len == n);
		{
			wide		total = (wide) n * stride;
			bool		want = total <= (wide) SIZE_MAX && model_fits(len, off, (size_t) total);

			CHECK(sift_span_try_array(s, off, n, stride, &r) == want);
			if (want)
				CHECK(r.len == (size_t) total && r.ptr == b + off);
		}
		CHECK(sift_span_try_u32le_at(s, off, &v32) == model_fits(len, off, 4));
		if (model_fits(len, off, 4))
			CHECK(v32 == ((uint32_t) b[off] | (uint32_t) b[off + 1] << 8 | (uint32_t) b[off + 2] << 16 | (uint32_t) b[off + 3] << 24));
		CHECK(sift_span_try_u64le_at(s, off, &v64) == model_fits(len, off, 8));
		{
			SiftSpan	t = s;
			bool		ok = sift_span_try_skip(&t, n);

			CHECK(ok == (n <= len) && t.len == (ok ? len - n : len));
		}
		/* a random program of consuming reads: the span ends where the model says, never past the block */
		{
			SiftSpan	t = s;
			size_t		pos = 0;

			for (int k = 0; k < 8; k++)
			{
				bool		ok;
				size_t		w = (size_t) 1 << (rnd() % 4);

				switch (w)
				{
					case 1: { uint8_t x; ok = sift_span_try_read_u8(&t, &x); break; }
					case 2: { uint16_t x; ok = sift_span_try_read_u16le(&t, &x); break; }
					case 4: { uint32_t x; ok = sift_span_try_read_u32le(&t, &x); break; }
					default: { uint64_t x; ok = sift_span_try_read_u64le(&t, &x); break; }
				}
				CHECK(ok == model_fits(len, pos, w));
				if (ok)
					pos += w;
				CHECK(t.ptr == b + pos && t.len == len - pos);
			}
		}
		free(b);
	}
}

static void
u32_must_fail(SiftSpan s, size_t off)
{
	MUST_FAIL(sift_span_u32le_at(s, off));
}

/* the last byte of the block is the last byte of a mapping: any read past it is a SIGSEGV */
static void
guard_case(size_t len)
{
	{
		void	   *base;
		size_t		maplen;
		uint8_t    *g = guard_bytes(len, &base, &maplen);
		SiftSpan	s = sift_span_make(g, len);
		uint64_t	sum = 0;

		for (size_t off = 0; off < len + 12; off++)
		{
			uint8_t		a;
			uint16_t	b;
			uint32_t	c;
			uint64_t	d;

			sum += sift_span_try_u8_at(s, off, &a) ? a : 0;
			sum += sift_span_try_u16le_at(s, off, &b) ? b : 0;
			sum += sift_span_try_u32le_at(s, off, &c) ? c : 0;
			sum += sift_span_try_u64le_at(s, off, &d) ? d : 0;
			u32_must_fail(s, len + off + 1);
		}
		{
			SiftSpan	t = s;
			uint8_t		x;

			while (sift_span_try_read_u8(&t, &x))
				sum += x;
			CHECK(sift_span_empty(t));
			MUST_FAIL(sift_span_read_u8(&t));
		}
		{
			uint8_t     copy[96];
			SiftSpan    t = s;

			MUST_PASS(sift_span_read_bytes(&t, copy, len));
			MUST_FAIL(sift_span_read_bytes(&t, copy, 1));
		}
		(void) sum;
		munmap(base, maplen);
	}
}

static void
test_guard(void)
{
	for (size_t len = 0; len <= 80; len++)
		guard_case(len);
}

/* a sub-span is a proof: nothing derived from it reaches outside it */
static void
test_nesting(void)
{
	uint8_t    *b = heap_bytes(64);
	SiftSpan	whole = sift_span_make(b, 64),
				mid = sift_span_sub(whole, 16, 16),
				r;

	MUST_FAIL(sift_span_u8_at(mid, 16));			/* byte 32 of the whole, outside mid */
	MUST_FAIL(r = sift_span_sub(mid, 8, 9));
	MUST_PASS(r = sift_span_sub(mid, 8, 8));
	CHECK(r.ptr == b + 24);
	MUST_FAIL(sift_span_u64le_at(r, 1));
	MUST_FAIL(r = sift_span_array(mid, 0, 5, 4));
	CHECK(sift_span_u8_at(mid, 0) == b[16] && sift_span_u8_at(mid, 15) == b[31]);
	(void) r;
	free(b);
}

int
main(void)
{
	test_basics();
	test_boundaries();
	test_null();
	test_varint();
	test_nesting();
	test_guard();
	test_differential();
	printf("span_test: ok, %lu checks, %lu failures raised on purpose\n", checks, fails_seen);
	return 0;
}
