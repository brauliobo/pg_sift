/*
 * core/span.h - a bounds-checked view of bytes, for every decoder that reads what a page, a segment or a chunk holds (docs/SAFETY.md).
 *
 * The bytes of a segment are untrusted: a torn write, a bit flip or a crafted file can make any stored length, count, offset or code
 * wrong. A decoder that adds a stored offset to a pointer reads wherever the number says. A SiftSpan is {ptr, len}; every way to
 * narrow it or to read from it checks against len first, in a form that cannot wrap (off <= len && n <= len - off, never off + n),
 * and a failed check does not return a value. The checked operations call sift_span_fail(), which does not return: the host raises
 * ERRCODE_DATA_CORRUPTED (src/core/span.c; ereport(ERROR) in PostgreSQL, the error frame of the SQLite host), a test aborts or
 * longjmps (-DSIFT_SPAN_ABORT, or its own definition). The sift_span_try_* operations are the same checks that return false, for a
 * decoder that reports through a status code (sift_blob_check) or that must clean up first; no check is ever skipped by a build flag.
 *
 * Reading consumes from the front (SiftSpan *, like a cursor); the *_at forms read at an offset and leave the span alone; sub, take and
 * array make a smaller span, whose len is the proof that the bytes are there. A decoder gets the span of its blob once, from the one place
 * that knows the real size (the stream reader, the page, the allocation), and passes spans down; a raw pointer leaves a span only through
 * sift_span_ptr() after the checks, to be handed to a kernel together with a count the span has already vouched for.
 *
 * Little endian by construction (shifts of bytes, no casts: no alignment or aliasing assumption, and the compilers fold each read into one
 * load on x86-64 and aarch64). Header only, host independent: the C library and nothing else, usable from the portable core
 * (src/portable.list). Cost: one compare and a never-taken branch per operation; the checks of a loop whose bound the compiler can
 * see are hoisted (measured in docs/SAFETY.md).
 */
#ifndef SIFT_SPAN_H
#define SIFT_SPAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define SIFT_SPAN_NORETURN	__attribute__((noreturn))
#define SIFT_SPAN_UNLIKELY(x)	__builtin_expect(!!(x), 0)
#else
#define SIFT_SPAN_NORETURN	_Noreturn
#define SIFT_SPAN_UNLIKELY(x)	(x)
#endif

typedef struct SiftSpan
{
	const uint8_t *ptr;
	size_t		len;
} SiftSpan;

/*
 * The failure hook: what was asked ("u32", "sub", ...), how many bytes it needed and how many the span had left. Does not return.
 * Defined by the host (src/core/span.c: ERRCODE_DATA_CORRUPTED), or here for standalone tools and tests with -DSIFT_SPAN_ABORT, or by a
 * test that wants to catch the failure (the test file defines it and longjmps).
 */
#ifdef SIFT_SPAN_ABORT
#include <stdio.h>
#include <stdlib.h>
static inline void SIFT_SPAN_NORETURN
sift_span_fail(const char *what, size_t need, size_t have)
{
	fprintf(stderr, "sift_span: %s needs %zu bytes, %zu left\n", what, need, have);
	abort();
}
#else
extern void sift_span_fail(const char *what, size_t need, size_t have) SIFT_SPAN_NORETURN;
#endif

/* A NULL pointer is an empty span whatever n says: every read then fails closed (and no pointer arithmetic is done on NULL) */
static const uint8_t sift_span_nil_[1];

static inline SiftSpan
sift_span_make(const void *p, size_t n)
{
	SiftSpan	s = {p ? (const uint8_t *) p : sift_span_nil_, p ? n : 0};

	return s;
}

static inline const uint8_t *
sift_span_ptr(SiftSpan s)
{
	return s.ptr;
}

static inline size_t
sift_span_remaining(SiftSpan s)
{
	return s.len;
}

static inline bool
sift_span_empty(SiftSpan s)
{
	return s.len == 0;
}

/* The span can be read as an array of elements of the given power-of-two size without unaligned access (for kernels that load wide) */
static inline bool
sift_span_is_aligned(SiftSpan s, size_t align)
{
	return ((uintptr_t) s.ptr & (align - 1)) == 0;
}

/* bytes [off, off + n) of s; the comparison cannot wrap */
static inline bool
sift_span_try_sub(SiftSpan s, size_t off, size_t n, SiftSpan *out)
{
	if (SIFT_SPAN_UNLIKELY(off > s.len || n > s.len - off))
		return false;
	out->ptr = s.ptr + off;
	out->len = n;
	return true;
}

/* count elements of stride bytes at off; count * stride is checked for overflow first */
static inline bool
sift_span_try_array(SiftSpan s, size_t off, size_t count, size_t stride, SiftSpan *out)
{
	if (SIFT_SPAN_UNLIKELY(stride != 0 && count > SIZE_MAX / stride))
		return false;
	return sift_span_try_sub(s, off, count * stride, out);
}

/* drops the first n bytes */
static inline bool
sift_span_try_skip(SiftSpan *s, size_t n)
{
	if (SIFT_SPAN_UNLIKELY(n > s->len))
		return false;
	s->ptr += n;
	s->len -= n;
	return true;
}

/* the first n bytes as a span, and drops them */
static inline bool
sift_span_try_take(SiftSpan *s, size_t n, SiftSpan *out)
{
	if (SIFT_SPAN_UNLIKELY(n > s->len))
		return false;
	out->ptr = s->ptr;
	out->len = n;
	s->ptr += n;
	s->len -= n;
	return true;
}

static inline bool
sift_span_try_u8_at(SiftSpan s, size_t off, uint8_t *v)
{
	if (SIFT_SPAN_UNLIKELY(off >= s.len))
		return false;
	*v = s.ptr[off];
	return true;
}

static inline bool
sift_span_try_u16le_at(SiftSpan s, size_t off, uint16_t *v)
{
	if (SIFT_SPAN_UNLIKELY(off > s.len || s.len - off < 2))
		return false;
	*v = (uint16_t) (s.ptr[off] | (uint16_t) s.ptr[off + 1] << 8);
	return true;
}

/* the load itself, after the check */
static inline uint32_t
sift_span_ld32le_(const uint8_t *p)
{
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static inline bool
sift_span_try_u32le_at(SiftSpan s, size_t off, uint32_t *v)
{
	if (SIFT_SPAN_UNLIKELY(off > s.len || s.len - off < 4))
		return false;
	*v = sift_span_ld32le_(s.ptr + off);
	return true;
}

static inline bool
sift_span_try_u64le_at(SiftSpan s, size_t off, uint64_t *v)
{
	if (SIFT_SPAN_UNLIKELY(off > s.len || s.len - off < 8))
		return false;
	*v = (uint64_t) sift_span_ld32le_(s.ptr + off) | (uint64_t) sift_span_ld32le_(s.ptr + off + 4) << 32;
	return true;
}

static inline bool
sift_span_try_read_u8(SiftSpan *s, uint8_t *v)
{
	return sift_span_try_u8_at(*s, 0, v) && sift_span_try_skip(s, 1);
}

static inline bool
sift_span_try_read_u16le(SiftSpan *s, uint16_t *v)
{
	return sift_span_try_u16le_at(*s, 0, v) && sift_span_try_skip(s, 2);
}

static inline bool
sift_span_try_read_u32le(SiftSpan *s, uint32_t *v)
{
	return sift_span_try_u32le_at(*s, 0, v) && sift_span_try_skip(s, 4);
}

static inline bool
sift_span_try_read_u64le(SiftSpan *s, uint64_t *v)
{
	return sift_span_try_u64le_at(*s, 0, v) && sift_span_try_skip(s, 8);
}

/*
 * LEB128 as core/varint.h writes it. At most 10 bytes, and the tenth may only carry bit 63: a longer or overflowing encoding is
 * corrupt (sift_get_varint_upto drops those bits silently). Non-canonical forms (a zero final group) are accepted, as they always were.
 * On failure *s is unchanged.
 */
static inline bool
sift_span_try_read_varint(SiftSpan *s, uint64_t *v)
{
	uint64_t	x = 0;

	for (size_t i = 0; i < s->len && i < 10; i++)
	{
		uint8_t		b = s->ptr[i];

		if (SIFT_SPAN_UNLIKELY(i == 9 && b > 1))
			return false;
		x |= (uint64_t) (b & 0x7f) << (7 * i);
		if (!(b & 0x80))
		{
			*v = x;
			s->ptr += i + 1;
			s->len -= i + 1;
			return true;
		}
	}
	return false;
}

/* A varint that must fit 32 bits (counts, lengths, deltas of a u32 domain) */
static inline bool
sift_span_try_read_varint32(SiftSpan *s, uint32_t *v)
{
	SiftSpan	t = *s;
	uint64_t	x;

	if (!sift_span_try_read_varint(&t, &x) || x > UINT32_MAX)
		return false;
	*v = (uint32_t) x;
	*s = t;
	return true;
}

/* copies n bytes out and consumes them */
static inline bool
sift_span_try_read_bytes(SiftSpan *s, void *dst, size_t n)
{
	if (SIFT_SPAN_UNLIKELY(n > s->len))
		return false;
	if (n)
		memcpy(dst, s->ptr, n);
	s->ptr += n;
	s->len -= n;
	return true;
}

#define SIFT_SPAN_CHECKED(what, need, have, expr) \
	do { if (SIFT_SPAN_UNLIKELY(!(expr))) sift_span_fail((what), (need), (have)); } while (0)

static inline SiftSpan
sift_span_sub(SiftSpan s, size_t off, size_t n)
{
	SiftSpan	r;

	SIFT_SPAN_CHECKED("sub", n, off > s.len ? 0 : s.len - off, sift_span_try_sub(s, off, n, &r));
	return r;
}

/* count elements of stride bytes at off */
static inline SiftSpan
sift_span_array(SiftSpan s, size_t off, size_t count, size_t stride)
{
	SiftSpan	r;

	SIFT_SPAN_CHECKED("array", count, off > s.len ? 0 : s.len - off, sift_span_try_array(s, off, count, stride, &r));
	return r;
}

static inline void
sift_span_skip(SiftSpan *s, size_t n)
{
	SIFT_SPAN_CHECKED("skip", n, s->len, sift_span_try_skip(s, n));
}

static inline SiftSpan
sift_span_take(SiftSpan *s, size_t n)
{
	SiftSpan	r;

	SIFT_SPAN_CHECKED("take", n, s->len, sift_span_try_take(s, n, &r));
	return r;
}

static inline void
sift_span_read_bytes(SiftSpan *s, void *dst, size_t n)
{
	SIFT_SPAN_CHECKED("read_bytes", n, s->len, sift_span_try_read_bytes(s, dst, n));
}

#define SIFT_SPAN_DEFINE_READS(name, type, size) \
static inline type \
sift_span_##name##_at(SiftSpan s, size_t off) \
{ \
	type		v; \
	SIFT_SPAN_CHECKED(#name, (size), off > s.len ? 0 : s.len - off, sift_span_try_##name##_at(s, off, &v)); \
	return v; \
} \
static inline type \
sift_span_read_##name(SiftSpan *s) \
{ \
	type		v; \
	SIFT_SPAN_CHECKED(#name, (size), s->len, sift_span_try_read_##name(s, &v)); \
	return v; \
}

SIFT_SPAN_DEFINE_READS(u8, uint8_t, 1)
SIFT_SPAN_DEFINE_READS(u16le, uint16_t, 2)
SIFT_SPAN_DEFINE_READS(u32le, uint32_t, 4)
SIFT_SPAN_DEFINE_READS(u64le, uint64_t, 8)

static inline uint64_t
sift_span_read_varint(SiftSpan *s)
{
	uint64_t	v;

	SIFT_SPAN_CHECKED("varint", 1, s->len, sift_span_try_read_varint(s, &v));
	return v;
}

static inline uint32_t
sift_span_read_varint32(SiftSpan *s)
{
	uint32_t	v;

	SIFT_SPAN_CHECKED("varint32", 1, s->len, sift_span_try_read_varint32(s, &v));
	return v;
}

#endif							/* SIFT_SPAN_H */
