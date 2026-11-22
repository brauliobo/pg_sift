/*
 * core/format_ux.c - see format_ux.h
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/format_ux.h"

typedef struct Out
{
	char	   *buf;
	size_t		len;
	size_t		n;
} Out;

static void put(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
put(Out *o, const char *fmt, ...)
{
	va_list		ap;
	int			w;

	if (o->n + 1 >= o->len)
		return;
	va_start(ap, fmt);
	w = vsnprintf(o->buf + o->n, o->len - o->n, fmt, ap);
	va_end(ap);
	o->n = w < 0 ? o->n : (size_t) w >= o->len - o->n ? o->len - 1 : o->n + (size_t) w;
}

static Out
start(char *buf, size_t len)
{
	if (len > 0)
		buf[0] = '\0';
	return (Out) {buf, len, 0};
}

static const char *
count(const uint64_t n, char *q)
{
	return sift_fmt_count(q, SIFT_UX_QUANTITY, n);
}

const char *
sift_fmt_duration(char *buf, size_t len, int64_t usecs)
{
	Out			o = start(buf, len);
	double		ms = (double) usecs / 1000.0;

	if (ms < 1000.0)
		put(&o, "%.0f ms", ms < 1.0 ? 1.0 : ms);
	else if (ms < 60000.0)
		put(&o, "%.2f s", ms / 1000.0);
	else
	{
		int64_t		mins = (int64_t) (ms / 60000.0);

		put(&o, "%lldm %.1fs", (long long) mins, (ms - (double) mins * 60000.0) / 1000.0);
	}
	return buf;
}

const char *
sift_fmt_bytes(char *buf, size_t len, uint64_t bytes)
{
	Out			o = start(buf, len);

	if (bytes < 1024)
		put(&o, "%llu B", (unsigned long long) bytes);
	else if (bytes < UINT64_C(1024) * 1024)
		put(&o, "%.1f KB", (double) bytes / 1024.0);
	else if (bytes < UINT64_C(1024) * 1024 * 1024)
		put(&o, "%.1f MB", (double) bytes / (1024.0 * 1024.0));
	else
		put(&o, "%.2f GB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
	return buf;
}

const char *
sift_fmt_count(char *buf, size_t len, uint64_t n)
{
	char		digits[24];
	Out			o = start(buf, len);
	int			nd = snprintf(digits, sizeof(digits), "%llu", (unsigned long long) n);

	for (int i = 0; i < nd; i++)
	{
		if (i > 0 && (nd - i) % 3 == 0)
			put(&o, ",");
		put(&o, "%c", digits[i]);
	}
	return buf;
}

const char *
sift_fmt_ratio(char *buf, size_t len, uint64_t heap_bytes, uint64_t bytes)
{
	Out			o = start(buf, len);

	if (heap_bytes >= SIFT_UX_RATIO_FLOOR && bytes > 0)
		put(&o, "%.1fx vs heap", (double) heap_bytes / (double) bytes);
	return buf;
}

const char *
sift_fmt_size_summary(char *buf, size_t len, uint64_t bytes, uint64_t rows, uint64_t heap_bytes)
{
	Out			o = start(buf, len);
	char		q[SIFT_UX_QUANTITY];

	put(&o, "%s", sift_fmt_bytes(q, sizeof(q), bytes));
	if (rows > 0)
		put(&o, ", %.1f B/row", (double) bytes / (double) rows);
	if (sift_fmt_ratio(q, sizeof(q), heap_bytes, bytes)[0] != '\0')
		put(&o, ", %s", q);
	return buf;
}

#ifdef SIFT_TESTING
bool		sift_test_quiet_build_notice;
#define TOOK_LONG(us)	(!sift_test_quiet_build_notice && (us) >= SIFT_UX_QUICK_US)
#else
#define TOOK_LONG(us)	((us) >= SIFT_UX_QUICK_US)
#endif

bool
sift_ux_should_emit_build(int64_t elapsed_us, uint64_t rows, bool chose_by_itself, bool changed_format)
{
	return TOOK_LONG(elapsed_us) || rows >= SIFT_UX_SMALL_ROWS || chose_by_itself || changed_format;
}

bool
sift_ux_should_emit_timing(int64_t elapsed_us)
{
	return TOOK_LONG(elapsed_us);
}

static const char *
noun_of(const SiftUxOp *op)
{
	return op->noun ? op->noun : "rows";
}

const char *
sift_ux_build_line(char *buf, size_t len, const SiftUxOp *op)
{
	Out			o = start(buf, len);
	char		n[SIFT_UX_QUANTITY], t[SIFT_UX_QUANTITY], s[SIFT_UX_QUANTITY], z[SIFT_UX_QUANTITY];

	put(&o, "%s \"%s\": ", op->am, op->name);
	if (op->format_before)
		put(&o, "upgraded Format %s -> %s (%s %s, %s -> %s) in %s", op->format_before, op->format_after, count(op->rows, n), noun_of(op),
			sift_fmt_bytes(s, sizeof(s), op->bytes_before), sift_fmt_bytes(z, sizeof(z), op->bytes), sift_fmt_duration(t, sizeof(t), op->elapsed_us));
	else
		put(&o, "built %s %s in %s (%s%s%s)", count(op->rows, n), noun_of(op), sift_fmt_duration(t, sizeof(t), op->elapsed_us),
			sift_fmt_size_summary(s, sizeof(s), op->bytes, op->rows, op->heap_bytes), op->extra ? ", " : "", op->extra ? op->extra : "");
	return buf;
}

static void
put_parts(Out *o, const SiftUxOp *op, const char *lead)
{
	char		g[SIFT_UX_QUANTITY];

	if (op->parts_before > 0 && op->parts_after > 0)
		put(o, "%scompacted %llu->%llu parts", lead, (unsigned long long) op->parts_before, (unsigned long long) op->parts_after);
	if (op->granules > 0)
		put(o, " (%s granules)", count(op->granules, g));
}

const char *
sift_ux_vacuum_line(char *buf, size_t len, const SiftUxOp *op)
{
	Out			o = start(buf, len);
	char		n[SIFT_UX_QUANTITY], s[SIFT_UX_QUANTITY], t[SIFT_UX_QUANTITY];

	put(&o, "%s \"%s\": removed %s dead %s", op->am, op->name, count(op->dead_rows, n), noun_of(op));
	put_parts(&o, op, ", ");
	put(&o, ", size %s", sift_fmt_bytes(s, sizeof(s), op->bytes));
	if (op->shrink_deferred)
		put(&o, " (tail shrink deferred: %s, %s free blocks remain)", op->shrink_deferred, count(op->free_blocks, n));
	else if (op->bytes_before > op->bytes)
		put(&o, " (reclaimed %s, %llu->%llu pages)", sift_fmt_bytes(s, sizeof(s), op->bytes_before - op->bytes),
			(unsigned long long) op->pages_before, (unsigned long long) op->pages_after);
	put(&o, " in %s", sift_fmt_duration(t, sizeof(t), op->elapsed_us));
	return buf;
}

const char *
sift_ux_maintain_line(char *buf, size_t len, const SiftUxOp *op)
{
	Out			o = start(buf, len);
	char		a[SIFT_UX_QUANTITY], b[SIFT_UX_QUANTITY], c[SIFT_UX_QUANTITY], t[SIFT_UX_QUANTITY];

	put(&o, "sift.maintain(\"%s\"): ", op->name);
	if (op->parts_before == op->parts_after && op->bytes_before <= op->bytes)
		put(&o, "already compact (0 parts changed)");
	else
	{
		put_parts(&o, op, "");
		put(&o, ", reclaimed %s (%s -> %s) in %s", sift_fmt_bytes(a, sizeof(a), op->bytes_before > op->bytes ? op->bytes_before - op->bytes : 0),
			sift_fmt_bytes(b, sizeof(b), op->bytes_before), sift_fmt_bytes(c, sizeof(c), op->bytes), sift_fmt_duration(t, sizeof(t), op->elapsed_us));
	}
	return buf;
}

const char *
sift_ux_upgrade_line(char *buf, size_t len, const SiftUxOp *op)
{
	Out			o = start(buf, len);
	char		n[SIFT_UX_QUANTITY], a[SIFT_UX_QUANTITY], b[SIFT_UX_QUANTITY], t[SIFT_UX_QUANTITY];

	put(&o, "%s \"%s\": upgraded Format %s -> %s (%s %s", op->am, op->name, op->format_before, op->format_after, count(op->rows, n), noun_of(op));
	if (op->parts_after > 0)
		put(&o, ", %llu parts", (unsigned long long) op->parts_after);
	put(&o, ", %s -> %s", sift_fmt_bytes(a, sizeof(a), op->bytes_before), sift_fmt_bytes(b, sizeof(b), op->bytes));
	if (op->bytes > 0 && op->bytes_before > op->bytes)
		put(&o, ", %.1fx smaller", (double) op->bytes_before / (double) op->bytes);
	put(&o, ") in %s", sift_fmt_duration(t, sizeof(t), op->elapsed_us));
	return buf;
}

const char *
sift_ux_check_line(char *buf, size_t len, const SiftUxOp *op)
{
	Out			o = start(buf, len);
	char		n[SIFT_UX_QUANTITY], t[SIFT_UX_QUANTITY];

	put(&o, "sift.check(\"%s\"): ok (Format %s", op->name, op->format_after);
	if (op->parts_after > 0)
		put(&o, ", %llu parts", (unsigned long long) op->parts_after);
	if (op->granules > 0)
		put(&o, ", %s granules", count(op->granules, n));
	put(&o, ", %s %s verified in %s)", count(op->rows, n), noun_of(op), sift_fmt_duration(t, sizeof(t), op->elapsed_us));
	return buf;
}
