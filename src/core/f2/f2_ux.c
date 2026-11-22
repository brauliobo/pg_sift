/*
 * core/f2/f2_ux.c - see f2_ux.h
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/f2/f2_ux.h"

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

static void
put_item(Out *o, uint64_t n, const char *what)
{
	if (n > 0)
		put(o, "%s%llu %s", o->n > 0 ? ", " : "", (unsigned long long) n, what);
}

const char *
sift_f2_ux_codecs(char *buf, size_t len, const SiftF2Codecs *c)
{
	Out			o = start(buf, len);

	put_item(&o, c->constant, "const");
	put_item(&o, c->delta, "delta");
	put_item(&o, c->packed, "packed");
	put_item(&o, c->inlined, "inline");
	put_item(&o, c->coded, "coded");
	return buf;
}

const char *
sift_f2_ux_build_extra(char *buf, size_t len, const SiftF2Census *c)
{
	Out			o = start(buf, len);
	char		n[SIFT_UX_QUANTITY], k[SIFT_UX_QUANTITY];

	put(&o, "%s parts, %s granules", sift_fmt_count(n, sizeof(n), c->parts), sift_fmt_count(k, sizeof(k), c->granules));
	if (sift_f2_ux_codecs(k, sizeof(k), &c->codecs)[0] != '\0')
		put(&o, ", codecs: %s", k);
	return buf;
}

const char *
sift_f2_ux_choices(char *buf, size_t len, const SiftF2Choices *c)
{
	Out			o = start(buf, len);

	put(&o, "%s%s%s", c->key_order ? "key_order" : "", c->paths ? (c->key_order ? ", paths" : "paths") : "",
		c->scope ? (c->key_order || c->paths ? ", scope" : "scope") : "");
	return buf;
}

bool
sift_f2_ux_chose(const SiftF2Choices *c)
{
	return c->key_order || c->paths || c->scope;
}

bool
sift_f2_ux_log_due(int64_t elapsed_us, int log_min_ms)
{
	return log_min_ms >= 0 && elapsed_us / 1000 >= log_min_ms;
}

static void
index_op(SiftUxOp *op, const SiftF2IndexFacts *v)
{
	*op = (SiftUxOp) {.am = "sift_col",.name = v->name,.elapsed_us = v->elapsed_us,.dead_rows = v->dead_rows,
		.parts_before = v->before.parts,.parts_after = v->after.parts,.granules = v->after.granules,
		.pages_before = v->pages_before,.pages_after = v->pages_after,
		.bytes_before = v->pages_before * v->block_size,.bytes = v->pages_after * v->block_size,
		.shrink_deferred = v->shrink_deferred ? "a lock is held on the index" : NULL,.free_blocks = v->free_blocks};
}

void
sift_f2_ux_vacuum_op(SiftUxOp *op, const SiftF2IndexFacts *v)
{
	index_op(op, v);
	if (v->before.parts == v->after.parts)
		op->parts_before = 0;
}

void
sift_f2_ux_maintain_op(SiftUxOp *op, const SiftF2IndexFacts *v)
{
	index_op(op, v);
}

const char *
sift_f2_ux_table_vacuum_line(char *buf, size_t len, const SiftF2TableFacts *t)
{
	Out			o = start(buf, len);
	char		a[SIFT_UX_QUANTITY], b[SIFT_UX_QUANTITY], c[SIFT_UX_QUANTITY], d[SIFT_UX_QUANTITY];

	put(&o, "sift_table \"%s\": removed %s rows (%u %s), %s live, %s dead-not-removable; parts %llu->%llu, size %s", t->name,
		sift_fmt_count(a, sizeof(a), t->removed), t->batches, t->batches == 1 ? "batch" : "batches", sift_fmt_count(b, sizeof(b), t->live),
		sift_fmt_count(c, sizeof(c), t->dead_not_removable), (unsigned long long) t->parts_before, (unsigned long long) t->parts_after,
		sift_fmt_bytes(d, sizeof(d), t->bytes_after));
	if (t->bytes_before > t->bytes_after)
		put(&o, " (reclaimed %s)", sift_fmt_bytes(a, sizeof(a), t->bytes_before - t->bytes_after));
	put(&o, " in %s", sift_fmt_duration(b, sizeof(b), t->elapsed_us));
	return buf;
}
