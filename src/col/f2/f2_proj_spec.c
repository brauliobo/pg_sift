/*
 * col/f2/f2_proj_spec.c - the text of a projection: the grammar of sift.create_projection, its canonical form, its hash, the columns of the parts it writes and date_trunc over the
 * timestamps its keys may hold.
 *
 *   ORDER BY col [ASC | DESC] [NULLS FIRST | LAST] {, ...} [INCLUDE (col {, ...})] [WHERE pred {AND pred}]
 *   GROUP BY key {, ...} MEASURES measure {, ...} [WHERE pred {AND pred}]
 *     key: col | date_trunc('unit', col)    measure: count(*) | count(col) | count(DISTINCT col) | sum(col) | avg(col) | min(col) | max(col), each [FILTER (WHERE pred {AND pred})]
 *     pred: col IS [NOT] NULL | col (= <> < <= > >=) literal | col IN (literal {, ...}) | [NOT] boolean_col
 */
#include "core/host.h"

#include <ctype.h>
#include <stdarg.h>

#include "col/f2/f2_proj.h"
#include "core/f2/meta.h"

typedef struct Parser
{
	const char *p;
	const SiftProjColumns *cols;
	char	   *err;
	size_t		errlen;
	bool		failed;
} Parser;

static void
fail(Parser *ps, const char *fmt, const char *arg)
{
	if (!ps->failed)
		snprintf(ps->err, ps->errlen, fmt, arg);
	ps->failed = true;
}

static void
skip_ws(Parser *ps)
{
	while (isspace((unsigned char) *ps->p))
		ps->p++;
}

static bool
is_word_char(char c)
{
	return isalnum((unsigned char) c) || c == '_';
}

/* The word at the cursor, folded to lower case, without consuming it; "" at the end or at a symbol */
static int
peek_word(Parser *ps, char *out, size_t n)
{
	const char *q;
	size_t		len = 0;

	skip_ws(ps);
	q = ps->p;
	while (is_word_char(*q) && len + 1 < n)
		out[len++] = (char) tolower((unsigned char) *q++);
	out[len] = '\0';
	return (int) len;
}

static bool
accept_word(Parser *ps, const char *kw)
{
	char		w[64];
	int			len = peek_word(ps, w, sizeof(w));

	if (len == 0 || strcmp(w, kw) != 0)
		return false;
	ps->p += len;
	return true;
}

static bool
accept_sym(Parser *ps, char c)
{
	skip_ws(ps);
	if (*ps->p != c)
		return false;
	ps->p++;
	return true;
}

static void
expect_sym(Parser *ps, char c)
{
	if (!accept_sym(ps, c))
	{
		char		s[2] = {c, '\0'};

		fail(ps, "expected \"%s\"", s);
	}
}

static void
expect_word(Parser *ps, const char *kw)
{
	if (!accept_word(ps, kw))
		fail(ps, "expected %s", kw);
}

/* A column reference: a word (folded to lower case) or a name in double quotes (exact); the column of the relation */
static int
parse_colref(Parser *ps)
{
	char		name[128];
	size_t		len = 0;

	skip_ws(ps);
	if (*ps->p == '"')
	{
		ps->p++;
		while (*ps->p != '"' && *ps->p != '\0' && len + 1 < sizeof(name))
			name[len++] = *ps->p++;
		if (*ps->p != '"')
		{
			fail(ps, "%s", "an unterminated quoted name");
			return -1;
		}
		ps->p++;
	}
	else
	{
		while (is_word_char(*ps->p) && len + 1 < sizeof(name))
			name[len++] = (char) tolower((unsigned char) *ps->p++);
		if (len == 0)
		{
			fail(ps, "%s", "expected a column name");
			return -1;
		}
	}
	name[len] = '\0';
	for (int i = 0; i < ps->cols->n; i++)
		if (strcmp(ps->cols->col[i].name, name) == 0)
		{
			if (!ps->cols->col[i].usable)
			{
				fail(ps, "column \"%s\" cannot be in a projection (an array of objects or a key of a jsonb document)", name);
				return -1;
			}
			return i;
		}
	fail(ps, "column \"%s\" does not exist", name);
	return -1;
}

#define USECS_PER_DAY	INT64CONST(86400000000)
#define DAYS_1970_TO_2000	10957

static int64 days_from_civil(int64 y, int m, int d);

/* ---- the constants of a predicate ---- */

static bool
read_digits(const char **p, int n, int64 *out)
{
	*out = 0;
	for (int i = 0; i < n; i++, (*p)++)
	{
		if (!isdigit((unsigned char) **p))
			return false;
		*out = *out * 10 + (**p - '0');
	}
	return true;
}

/* 'YYYY-MM-DD' of a date, or 'YYYY-MM-DD HH:MM[:SS[.ffffff]]' of a timestamp, as days or microseconds since 2000-01-01 */
static bool
time_literal(const char *p, SiftProjTime kind, int64 *out)
{
	int64		y, mo, d, hh = 0, mi = 0, ss = 0, us = 0;

	if (!read_digits(&p, 4, &y) || *p++ != '-' || !read_digits(&p, 2, &mo) || *p++ != '-' || !read_digits(&p, 2, &d) || mo < 1 || mo > 12 || d < 1 || d > 31)
		return false;
	*out = days_from_civil(y, (int) mo, (int) d) - DAYS_1970_TO_2000;
	if (kind == SPT_DATE)
		return *p == '\0';
	if (*p == ' ' || *p == 'T')
	{
		p++;
		if (!read_digits(&p, 2, &hh) || *p++ != ':' || !read_digits(&p, 2, &mi))
			return false;
		if (*p == ':' && (p++, !read_digits(&p, 2, &ss)))
			return false;
		if (*p == '.')
		{
			int			digits = 0;

			for (p++; isdigit((unsigned char) *p) && digits < 6; p++, digits++)
				us = us * 10 + (*p - '0');
			for (; digits < 6; digits++)
				us *= 10;
		}
	}
	*out = *out * USECS_PER_DAY + ((hh * 60 + mi) * 60 + ss) * 1000000 + us;
	return *p == '\0';
}

/* An integer or decimal literal as the integer a cell of the column holds: a numeric is scaled, anything else takes whole numbers only */
static bool
number_literal(const char *t, const SiftProjColumn *col, int64 *out)
{
	bool		neg = *t == '-';
	int64		v = 0;
	int			digits = 0,
				frac = -1;
	bool		dot;

	if (neg || *t == '+')
		t++;
	for (; isdigit((unsigned char) *t) || (*t == '.' && frac < 0); t++)
	{
		if (*t == '.')
		{
			frac = 0;
			continue;
		}
		if (++digits > 18 || (frac >= 0 && ++frac > col->scale))
			return false;
		v = v * 10 + (*t - '0');
	}
	dot = frac >= 0;
	if (col->numeric)
		for (frac = Max(frac, 0); frac < col->scale; frac++)
			v *= 10;
	*out = neg ? -v : v;
	return digits > 0 && *t == '\0' && (col->numeric || !dot);
}

/* The next constant of a predicate on col, into slot i of p */
static void
parse_const(Parser *ps, const SiftProjColumn *col, SiftProjSpecPred *p, int i)
{
	char		t[64];
	size_t		len = 0;
	bool		quoted;

	skip_ws(ps);
	quoted = *ps->p == '\'';
	ps->p += quoted;
	while (*ps->p != '\0' && len + 1 < sizeof(t) && (quoted ? *ps->p != '\'' : (is_word_char(*ps->p) || *ps->p == '-' || *ps->p == '+' || *ps->p == '.')))
		t[len++] = *ps->p++;
	t[len] = '\0';
	if (quoted && *ps->p != '\'')
	{
		fail(ps, "%s", "an unterminated constant");
		return;
	}
	ps->p += quoted;
	if (col->kind == SIFT_CELL_STR && (!quoted || len > SIFT_PROJ_PRED_BYTES))
		fail(ps, "%s", "a text constant of a projection is in quotes and at most 24 bytes");
	else if (col->kind == SIFT_CELL_STR)
	{
		p->bytes = true;
		p->slen[i] = (uint8) len;
		memcpy(p->sval[i], t, len);
	}
	else if (col->kind != SIFT_CELL_INT)
		fail(ps, "column \"%s\" is a floating point column: a projection cannot test it exactly", col->name);
	else if (col->time == SPT_DATE || col->time == SPT_TIMESTAMP)
	{
		if (!quoted || !time_literal(t, (SiftProjTime) col->time, &p->ival[i]))
			fail(ps, "\"%s\" is not a constant of a date or timestamp column", t);
	}
	else if (col->boolean)
	{
		if (strcmp(t, "true") != 0 && strcmp(t, "false") != 0)
			fail(ps, "\"%s\" is not true or false", t);
		p->ival[i] = t[0] == 't';
	}
	else if (!number_literal(t, col, &p->ival[i]))
		fail(ps, "\"%s\" is not a number the column holds exactly", t);
}

static bool
next_is(Parser *ps, const char *kw)
{
	char		w[16];

	return peek_word(ps, w, sizeof(w)) > 0 && strcmp(w, kw) == 0;
}

/* One predicate: col IS [NOT] NULL, col op constant, col IN (constants), or a boolean column on its own or after NOT */
static void
parse_pred(Parser *ps, SiftProjSpecPred *p)
{
	static const struct
	{
		const char *text;
		SiftProjOp	op;
	}			ops[] = {{"<=", SPO_LE}, {">=", SPO_GE}, {"<>", SPO_NE}, {"!=", SPO_NE}, {"<", SPO_LT}, {">", SPO_GT}, {"=", SPO_EQ}};
	bool		neg = accept_word(ps, "not");
	int			leaf = parse_colref(ps),
				k = 0;
	const SiftProjColumn *col;

	memset(p, 0, sizeof(*p));
	if (ps->failed)
		return;
	col = &ps->cols->col[leaf];
	p->leaf = (int16) leaf;
	p->nvals = 1;
	skip_ws(ps);
	while (k < lengthof(ops) && strncmp(ps->p, ops[k].text, strlen(ops[k].text)) != 0)
		k++;
	if (col->boolean && k == lengthof(ops) && !next_is(ps, "is") && !next_is(ps, "in"))
	{
		p->op = SPO_EQ;
		p->ival[0] = !neg;
	}
	else if (accept_word(ps, "is"))
	{
		p->nvals = 0;
		p->op = accept_word(ps, "not") ? SPO_IS_NOT_NULL : SPO_IS_NULL;
		expect_word(ps, "null");
	}
	else if (accept_word(ps, "in"))
	{
		p->op = SPO_IN;
		p->nvals = 0;
		expect_sym(ps, '(');
		do
		{
			if (p->nvals == SIFT_PROJ_PRED_VALS)
				fail(ps, "%s", "an IN list of a projection holds at most 4 constants");
			else
				parse_const(ps, col, p, p->nvals++);
		} while (!ps->failed && accept_sym(ps, ','));
		expect_sym(ps, ')');
	}
	else if (k == lengthof(ops))
		fail(ps, "%s", "expected a comparison, IN or IS");
	else
	{
		ps->p += strlen(ops[k].text);
		p->op = ops[k].op;
		parse_const(ps, col, p, 0);
	}
}

static void
parse_preds(Parser *ps, SiftProjSpecPred *out, int *n, int max)
{
	do
	{
		if (*n == max)
			fail(ps, "%s", "too many predicates");
		else
			parse_pred(ps, &out[(*n)++]);
	} while (!ps->failed && accept_word(ps, "and"));
}

static void
parse_where(Parser *ps, SiftProjSpec *s)
{
	if (!ps->failed && accept_word(ps, "where"))
		parse_preds(ps, s->where, &s->nwhere, SIFT_PROJ_WHERE_MAX);
}

static const char *const unit_name[] = {"", "minute", "hour", "day", "week", "month", "quarter", "year"};

static SiftProjUnit
parse_unit(Parser *ps)
{
	char		u[32];
	size_t		len = 0;

	skip_ws(ps);
	if (*ps->p != '\'')
	{
		fail(ps, "%s", "expected a unit in quotes");
		return SPU_NONE;
	}
	ps->p++;
	while (*ps->p != '\'' && *ps->p != '\0' && len + 1 < sizeof(u))
		u[len++] = (char) tolower((unsigned char) *ps->p++);
	u[len] = '\0';
	if (*ps->p != '\'')
		fail(ps, "%s", "an unterminated unit");
	else
		ps->p++;
	for (int i = SPU_MINUTE; i <= SPU_YEAR; i++)
		if (strcmp(u, unit_name[i]) == 0)
			return (SiftProjUnit) i;
	fail(ps, "date_trunc unit \"%s\" is not supported (minute, hour, day, week, month, quarter, year)", u);
	return SPU_NONE;
}

static void
add_key(Parser *ps, SiftProjSpec *s, SiftProjKey k)
{
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].leaf == k.leaf && s->key[i].fn == k.fn && s->key[i].unit == k.unit)
		{
			fail(ps, "column \"%s\" is listed twice", ps->cols->col[k.leaf].name);
			return;
		}
	if (s->nkeys >= SIFT_PROJ_MAX_COLS)
		fail(ps, "%s", "too many columns");
	else
		s->key[s->nkeys++] = k;
}

static void
parse_sort(Parser *ps, SiftProjSpec *s)
{
	s->kind = SIFT_PROJ_SORT;
	s->all_columns = true;
	do
	{
		SiftProjKey k = {.leaf = (int16) parse_colref(ps)};

		if (ps->failed)
			return;
		if (accept_word(ps, "desc"))
			k.desc = k.nulls_first = true;
		else
			(void) accept_word(ps, "asc");
		if (accept_word(ps, "nulls"))
		{
			if (accept_word(ps, "first"))
				k.nulls_first = true;
			else if (accept_word(ps, "last"))
				k.nulls_first = false;
			else
				fail(ps, "%s", "expected FIRST or LAST");
		}
		add_key(ps, s, k);
	} while (!ps->failed && accept_sym(ps, ','));
	if (!ps->failed && accept_word(ps, "include"))
	{
		s->all_columns = false;
		expect_sym(ps, '(');
		do
		{
			int			c = parse_colref(ps);
			bool		dup = false;

			if (ps->failed)
				return;
			for (int i = 0; i < s->nkeys; i++)
				dup |= s->key[i].leaf == c;
			for (int i = 0; i < s->ninclude; i++)
				dup |= s->include[i] == c;
			if (!dup && s->ninclude < SIFT_PROJ_MAX_COLS)
				s->include[s->ninclude++] = (int16) c;
		} while (!ps->failed && accept_sym(ps, ','));
		expect_sym(ps, ')');
	}
	parse_where(ps, s);
}

bool
sift_proj_spec_pred_equal(const SiftProjSpecPred *a, const SiftProjSpecPred *b)
{
	if (a->leaf != b->leaf || a->op != b->op || a->bytes != b->bytes || a->nvals != b->nvals)
		return false;
	for (int i = 0; i < a->nvals; i++)
		if (a->bytes ? a->slen[i] != b->slen[i] || memcmp(a->sval[i], b->sval[i], a->slen[i]) != 0 : a->ival[i] != b->ival[i])
			return false;
	return true;
}

static bool
same_filter(const SiftProjMeasure *a, const SiftProjMeasure *b)
{
	if (a->nfilter != b->nfilter)
		return false;
	for (int i = 0; i < a->nfilter; i++)
		if (!sift_proj_spec_pred_equal(&a->filter[i], &b->filter[i]))
			return false;
	return true;
}

/* count(DISTINCT col): the column is kept as the last group key, and only one column can be (the groups would multiply) */
static void
distinct_key(Parser *ps, SiftProjSpec *s, int leaf)
{
	bool		keyed = false;

	if (s->distinct_leaf >= 0 && s->distinct_leaf != leaf)
		fail(ps, "%s", "one count(DISTINCT) column per projection: list the others in GROUP BY");
	for (int i = 0; i < s->nkeys; i++)
		keyed |= s->key[i].leaf == leaf && s->key[i].fn == SPF_COLUMN;
	if (!ps->failed && !keyed)
		add_key(ps, s, (SiftProjKey) {.leaf = (int16) leaf,.fn = SPF_COLUMN});
	s->distinct_leaf = (int16) leaf;
}

static void
parse_group(Parser *ps, SiftProjSpec *s)
{
	s->kind = SIFT_PROJ_AGG;
	do
	{
		SiftProjKey k = {.fn = SPF_COLUMN};
		char		w[64];
		const char *save = ps->p;

		if (peek_word(ps, w, sizeof(w)) > 0 && strcmp(w, "date_trunc") == 0)
		{
			ps->p += strlen("date_trunc");
			if (accept_sym(ps, '('))
			{
				k.fn = SPF_TRUNC;
				k.unit = parse_unit(ps);
				expect_sym(ps, ',');
			}
			else
				ps->p = save;
		}
		k.leaf = (int16) parse_colref(ps);
		if (ps->failed)
			return;
		if (k.fn == SPF_TRUNC)
		{
			expect_sym(ps, ')');
			if (ps->cols->col[k.leaf].time == SPT_DATE && k.unit < SPU_DAY)
				fail(ps, "date column \"%s\" has day granularity: use day, week, month, quarter or year", ps->cols->col[k.leaf].name);
			else if (ps->cols->col[k.leaf].time != SPT_TIMESTAMP && ps->cols->col[k.leaf].time != SPT_DATE)
				fail(ps, "date_trunc of \"%s\" is not supported: only date and timestamp without time zone are exact (timestamptz depends on the session time zone)", ps->cols->col[k.leaf].name);
		}
		else if (ps->cols->col[k.leaf].kind == SIFT_CELL_F8)
			fail(ps, "group column \"%s\" is a floating point column, whose groups the projection cannot keep exactly", ps->cols->col[k.leaf].name);
		add_key(ps, s, k);
	} while (!ps->failed && accept_sym(ps, ','));
	expect_word(ps, "measures");
	do
	{
		SiftProjMeasure m = {.leaf = -1};
		char		w[64];
		SiftCellKind kind;

		if (peek_word(ps, w, sizeof(w)) == 0)
		{
			fail(ps, "%s", "expected a measure");
			return;
		}
		ps->p += strlen(w);
		if (strcmp(w, "count") == 0)
			m.fn = SPM_COUNT;
		else if (strcmp(w, "sum") == 0)
			m.fn = SPM_SUM;
		else if (strcmp(w, "avg") == 0)
			m.fn = SPM_AVG;
		else if (strcmp(w, "min") == 0)
			m.fn = SPM_MIN;
		else if (strcmp(w, "max") == 0)
			m.fn = SPM_MAX;
		else
		{
			fail(ps, "measure \"%s\" is not supported (count, count(DISTINCT), sum, avg, min, max)", w);
			return;
		}
		expect_sym(ps, '(');
		if (m.fn == SPM_COUNT && accept_sym(ps, '*'))
			m.fn = SPM_COUNT_STAR;
		else
		{
			if (m.fn == SPM_COUNT && accept_word(ps, "distinct"))
				m.fn = SPM_COUNT_DISTINCT;
			m.leaf = (int16) parse_colref(ps);
		}
		expect_sym(ps, ')');
		if (!ps->failed && accept_word(ps, "filter"))
		{
			expect_sym(ps, '(');
			expect_word(ps, "where");
			parse_preds(ps, m.filter, &m.nfilter, SIFT_PROJ_FILTER_MAX);
			expect_sym(ps, ')');
		}
		if (!ps->failed && m.fn == SPM_COUNT_DISTINCT)
			distinct_key(ps, s, m.leaf);
		if (ps->failed)
			return;
		kind = m.leaf >= 0 ? ps->cols->col[m.leaf].kind : SIFT_CELL_INT;
		if ((m.fn == SPM_SUM || m.fn == SPM_AVG) && kind != SIFT_CELL_INT)
			fail(ps, "%s", "sum and avg are kept for integer, date, timestamp and numeric columns only: a sum of floating point values depends on the order of the rows");
		else if ((m.fn == SPM_MIN || m.fn == SPM_MAX) && kind == SIFT_CELL_STR)
			fail(ps, "%s", "min and max of a text column are not kept: they depend on the collation");
		for (int i = 0; i < s->nmeas; i++)
			if (s->meas[i].fn == m.fn && s->meas[i].leaf == m.leaf && same_filter(&s->meas[i], &m))
				fail(ps, "%s", "a measure is listed twice");
		if (!ps->failed && s->nmeas < SIFT_PROJ_MAX_COLS)
			s->meas[s->nmeas++] = m;
	} while (!ps->failed && accept_sym(ps, ','));
	parse_where(ps, s);
}

bool
sift_proj_spec_parse(const char *text, const SiftProjColumns *cols, SiftProjSpec *out, char *err, size_t errlen)
{
	Parser		ps = {.p = text,.cols = cols,.err = err,.errlen = errlen};

	memset(out, 0, sizeof(*out));
	out->distinct_leaf = -1;
	if (strlen(text) >= SIFT_PROJ_SPEC_MAX)
		fail(&ps, "%s", "the spec is too long");
	else if (accept_word(&ps, "order"))
	{
		expect_word(&ps, "by");
		parse_sort(&ps, out);
	}
	else if (accept_word(&ps, "group"))
	{
		expect_word(&ps, "by");
		parse_group(&ps, out);
	}
	else
		fail(&ps, "%s", "a spec starts with ORDER BY or GROUP BY");
	skip_ws(&ps);
	if (!ps.failed && *ps.p != '\0')
		fail(&ps, "unexpected text \"%s\"", ps.p);
	return !ps.failed;
}

static bool
needs_quotes(const char *name)
{
	if (!(islower((unsigned char) name[0]) || name[0] == '_'))
		return true;
	for (const char *p = name; *p != '\0'; p++)
		if (!(islower((unsigned char) *p) || isdigit((unsigned char) *p) || *p == '_'))
			return true;
	return false;
}

typedef struct Buf
{
	char	   *p;
	size_t		n;
} Buf;

static void bput(Buf *b, const char *fmt,...) pg_attribute_printf(2, 3);

static void
bput(Buf *b, const char *fmt, ...)
{
	va_list		ap;
	int			w;

	va_start(ap, fmt);
	w = vsnprintf(b->p + b->n, SIFT_PROJ_SPEC_MAX - b->n, fmt, ap);
	va_end(ap);
	b->n = Min(b->n + (size_t) w, SIFT_PROJ_SPEC_MAX - 1);
}

static void
put_name(Buf *b, const char *name)
{
	bput(b, needs_quotes(name) ? "\"%s\"" : "%s", name);
}

static void
put_key(Buf *b, const SiftProjSpec *s, const SiftProjColumns *cols, const SiftProjKey *k)
{
	if (k->fn == SPF_TRUNC)
		bput(b, "date_trunc('%s', ", unit_name[k->unit]);
	put_name(b, cols->col[k->leaf].name);
	if (k->fn == SPF_TRUNC)
		bput(b, ")");
	if (s->kind != SIFT_PROJ_SORT)
		return;
	if (k->desc)
		bput(b, " DESC");
	if (k->nulls_first != k->desc)
		bput(b, k->nulls_first ? " NULLS FIRST" : " NULLS LAST");
}

static const char *const measure_name[] = {"count", "count", "sum", "avg", "min", "max", "count"};

static void civil_from_days(int64 z, int64 *y, int *m, int *d);

static void
put_literal(Buf *b, const SiftProjColumn *col, const SiftProjSpecPred *p, int i)
{
	int64		v = p->ival[i],
				y;
	int			m, d;

	if (p->bytes)
		bput(b, "'%.*s'", (int) p->slen[i], p->sval[i]);
	else if (col->time == SPT_DATE || col->time == SPT_TIMESTAMP)
	{
		int64		days = col->time == SPT_DATE ? v : v / USECS_PER_DAY - (v % USECS_PER_DAY < 0),
					rem = col->time == SPT_DATE ? 0 : v - days * USECS_PER_DAY;

		civil_from_days(days + DAYS_1970_TO_2000, &y, &m, &d);
		bput(b, "'%04lld-%02d-%02d", (long long) y, m, d);
		if (col->time == SPT_TIMESTAMP)
			bput(b, " %02lld:%02lld:%02lld", (long long) (rem / 3600000000), (long long) (rem / 60000000 % 60), (long long) (rem / 1000000 % 60));
		if (col->time == SPT_TIMESTAMP && rem % 1000000 != 0)
			bput(b, ".%06lld", (long long) (rem % 1000000));
		bput(b, "'");
	}
	else if (col->boolean)
		bput(b, v != 0 ? "true" : "false");
	else if (col->numeric && col->scale > 0)
	{
		int64		unit = 1;

		for (int k = 0; k < col->scale; k++)
			unit *= 10;
		bput(b, "%s%lld.%0*lld", v < 0 ? "-" : "", (long long) (v < 0 ? -(v / unit) : v / unit), col->scale, (long long) (v < 0 ? -(v % unit) : v % unit));
	}
	else
		bput(b, "%lld", (long long) v);
}

static void
put_preds(Buf *b, const SiftProjColumns *cols, const SiftProjSpecPred *p, int n)
{
	static const char *const op_text[] = {"<", "<=", "=", ">=", ">", "<>"};

	for (int i = 0; i < n; i++)
	{
		const SiftProjColumn *col = &cols->col[p[i].leaf];

		if (i > 0)
			bput(b, " AND ");
		put_name(b, col->name);
		if (p[i].op == SPO_IS_NULL || p[i].op == SPO_IS_NOT_NULL)
			bput(b, p[i].op == SPO_IS_NULL ? " IS NULL" : " IS NOT NULL");
		else if (p[i].op == SPO_IN)
		{
			bput(b, " IN (");
			for (int k = 0; k < p[i].nvals; k++)
			{
				bput(b, k > 0 ? ", " : "");
				put_literal(b, col, &p[i], k);
			}
			bput(b, ")");
		}
		else
		{
			bput(b, " %s ", op_text[p[i].op]);
			put_literal(b, col, &p[i], 0);
		}
	}
}

static void
put_where(Buf *b, const SiftProjSpec *s, const SiftProjColumns *cols)
{
	if (s->nwhere == 0)
		return;
	bput(b, " WHERE ");
	put_preds(b, cols, s->where, s->nwhere);
}

void
sift_proj_spec_canonical(const SiftProjSpec *s, const SiftProjColumns *cols, char out[SIFT_PROJ_SPEC_MAX])
{
	Buf			b = {.p = out};

	out[0] = '\0';
	bput(&b, s->kind == SIFT_PROJ_SORT ? "ORDER BY " : "GROUP BY ");
	for (int i = 0; i < s->nkeys; i++)
	{
		if (i > 0)
			bput(&b, ", ");
		put_key(&b, s, cols, &s->key[i]);
	}
	if (s->kind == SIFT_PROJ_SORT && !s->all_columns)
	{
		bput(&b, " INCLUDE (");
		for (int i = 0; i < s->ninclude; i++)
		{
			if (i > 0)
				bput(&b, ", ");
			put_name(&b, cols->col[s->include[i]].name);
		}
		bput(&b, ")");
	}
	if (s->kind != SIFT_PROJ_AGG)
	{
		put_where(&b, s, cols);
		return;
	}
	bput(&b, " MEASURES ");
	for (int i = 0; i < s->nmeas; i++)
	{
		if (i > 0)
			bput(&b, ", ");
		bput(&b, "%s(", measure_name[s->meas[i].fn]);
		if (s->meas[i].fn == SPM_COUNT_STAR)
			bput(&b, "*");
		else
		{
			bput(&b, s->meas[i].fn == SPM_COUNT_DISTINCT ? "DISTINCT " : "");
			put_name(&b, cols->col[s->meas[i].leaf].name);
		}
		bput(&b, ")");
		if (s->meas[i].nfilter > 0)
		{
			bput(&b, " FILTER (WHERE ");
			put_preds(&b, cols, s->meas[i].filter, s->meas[i].nfilter);
			bput(&b, ")");
		}
	}
	put_where(&b, s, cols);
}

uint32
sift_proj_spec_hash(const char *canonical)
{
	return sift_crc32c(canonical, strlen(canonical));
}

uint32
sift_proj_spec_flags(const SiftProjSpec *s)
{
	uint32		flags = (s->nwhere > 0 ? SIFT_PROJREF_FLAG_PARTIAL : 0) | (s->distinct_leaf >= 0 ? SIFT_PROJREF_FLAG_DISTINCT : 0);

	for (int m = 0; m < s->nmeas; m++)
		flags |= s->meas[m].nfilter > 0 ? SIFT_PROJREF_FLAG_FILTERED : 0;
	return flags;
}

bool
sift_proj_spec_invertible(const SiftProjSpec *s)
{
	for (int i = 0; i < s->nmeas; i++)
		if (s->meas[i].fn == SPM_MIN || s->meas[i].fn == SPM_MAX)
			return false;
	return true;
}

int
sift_proj_layout(const SiftProjSpec *s, const SiftProjColumns *cols, SiftProjLayoutCol *out)
{
	int			n = 0;

	for (int i = 0; i < s->nkeys; i++)
		out[n++] = (SiftProjLayoutCol) {SPR_KEY, s->key[i].fn == SPF_TRUNC ? SIFT_CELL_INT : cols->col[s->key[i].leaf].kind, s->key[i].leaf, -1};
	if (s->kind == SIFT_PROJ_SORT)
	{
		for (int c = 0; c < cols->n; c++)
		{
			bool		keep = cols->col[c].usable && s->all_columns;

			for (int i = 0; i < s->nkeys; i++)
				keep &= s->key[i].leaf != c;
			for (int i = 0; i < s->ninclude; i++)
				keep |= s->include[i] == c;
			if (keep)
				out[n++] = (SiftProjLayoutCol) {SPR_INCLUDE, cols->col[c].kind, (int16) c, -1};
		}
		return n;
	}
	out[n++] = (SiftProjLayoutCol) {SPR_ROWS, SIFT_CELL_INT, -1, -1};
	for (int m = 0; m < s->nmeas; m++)
	{
		const SiftProjMeasure *me = &s->meas[m];
		SiftCellKind k = me->leaf >= 0 ? cols->col[me->leaf].kind : SIFT_CELL_INT;

		switch (me->fn)
		{
			case SPM_COUNT_STAR:
			case SPM_COUNT:
				out[n++] = (SiftProjLayoutCol) {SPR_COUNT, SIFT_CELL_INT, me->leaf, (int16) m};
				break;
			case SPM_SUM:
			case SPM_AVG:
				out[n++] = (SiftProjLayoutCol) {SPR_SUM_HI, SIFT_CELL_INT, me->leaf, (int16) m};
				out[n++] = (SiftProjLayoutCol) {SPR_SUM_LO, SIFT_CELL_INT, me->leaf, (int16) m};
				out[n++] = (SiftProjLayoutCol) {SPR_SUM_NN, SIFT_CELL_INT, me->leaf, (int16) m};
				break;
			case SPM_MIN:
				out[n++] = (SiftProjLayoutCol) {SPR_MIN, k, me->leaf, (int16) m};
				break;
			case SPM_MAX:
				out[n++] = (SiftProjLayoutCol) {SPR_MAX, k, me->leaf, (int16) m};
				break;
			case SPM_COUNT_DISTINCT:
				break;
		}
	}
	return n;
}

/* ---- date_trunc ---- */

#define USECS_PER_MIN	INT64CONST(60000000)
#define USECS_PER_HOUR	INT64CONST(3600000000)

static int64
floor_div(int64 a, int64 b)
{
	int64		q = a / b;

	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* Howard Hinnant's civil calendar: days since 1970-01-01 <-> year, month, day */
static void
civil_from_days(int64 z, int64 *y, int *m, int *d)
{
	int64		era, yoe, doy, mp;

	z += 719468;
	era = floor_div(z, 146097);
	{
		int64		doe = z - era * 146097;

		yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	}
	mp = (5 * doy + 2) / 153;
	*d = (int) (doy - (153 * mp + 2) / 5 + 1);
	*m = (int) (mp < 10 ? mp + 3 : mp - 9);
	*y = yoe + era * 400 + (*m <= 2);
}

static int64
days_from_civil(int64 y, int m, int d)
{
	int64		era, yoe, doy, doe;

	y -= m <= 2;
	era = floor_div(y, 400);
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

int64
sift_proj_trunc_date(int64 days, SiftProjUnit unit)
{
	int64		y;
	int			m, d;

	switch (unit)
	{
		case SPU_DAY:
			return days;
		case SPU_WEEK:
			return days - ((days % 7 + 7 + 5) % 7);
		default:
			break;
	}
	civil_from_days(days + DAYS_1970_TO_2000, &y, &m, &d);
	if (unit == SPU_QUARTER)
		m = (m - 1) / 3 * 3 + 1;
	else if (unit == SPU_YEAR)
		m = 1;
	return days_from_civil(y, m, 1) - DAYS_1970_TO_2000;
}

int64
sift_proj_trunc(int64 ts, SiftProjUnit unit)
{
	if (unit == SPU_MINUTE)
		return floor_div(ts, USECS_PER_MIN) * USECS_PER_MIN;
	if (unit == SPU_HOUR)
		return floor_div(ts, USECS_PER_HOUR) * USECS_PER_HOUR;
	return sift_proj_trunc_date(floor_div(ts, USECS_PER_DAY), unit) * USECS_PER_DAY;
}

int64
sift_proj_trunc_cell(SiftProjTime time, int64 v, SiftProjUnit unit)
{
	return time == SPT_DATE ? sift_proj_trunc_date(v, unit) : sift_proj_trunc(v, unit);
}

/* A value truncated to fine is truncated to coarse by truncating it again: every boundary of coarse is a boundary of fine (a week is not made of months, nor a month of weeks) */
bool
sift_proj_unit_refines(SiftProjUnit fine, SiftProjUnit coarse)
{
	static const uint8 rank[] = {0, 1, 2, 3, 0, 4, 5, 6};

	if (fine == coarse)
		return true;
	if (fine == SPU_WEEK || coarse == SPU_WEEK)
		return fine != SPU_WEEK && coarse == SPU_WEEK ? fine <= SPU_DAY : false;
	return rank[fine] != 0 && rank[coarse] != 0 && rank[fine] < rank[coarse];
}
