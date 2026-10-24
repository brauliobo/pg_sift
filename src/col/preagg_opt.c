/*
 * col/preagg_opt.c - the pre-aggregates option: the leaves that can be dims and measures, the default spec, the parser (docs/COL-PREAGG.md).
 */

#include "core/host.h"

#include "col/preagg.h"

/* a scalar leaf of the top level, or of a list of the top level: a list inside a list has no cube */
static bool
depth1(const SiftColSchema *s, const SiftColDef *d)
{
	return d->kind == SIFT_COLK_SCALAR && (d->parent < 0 || s->cols[d->parent].parent < 0);
}

/* A leaf of a wide value (a lane, its head) or of money / interval is no dimension or measure of the pre-aggregates: its sums and keys are not the value's */
static bool
plain_type(const SiftColDef *d)
{
	return !sift_col_def_needs_wide(d);
}

bool
sift_cpagg_dim_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return depth1(s, d) && plain_type(d) && (d->store == SIFT_COLS_INT || d->store == SIFT_COLS_STR);
}

/* 'a, b': the names of the leaves an option takes, each one ok(); empty or 'off': none. s NULL: the syntax only. Returns NULL, or why the option is not valid */
const char *
sift_cpagg_leaf_list(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf,
					 bool (*ok) (const SiftColSchema *, const SiftColDef *), const char *bad)
{
	const char *p = opt;

	while (*p == ' ')
		p++;
	if (*p == '\0' || strcmp(p, "off") == 0)
		return NULL;
	for (;;)
	{
		const char *end = strchr(p, ',');
		size_t		len = end ? (size_t) (end - p) : strlen(p);
		int			c = -1;

		while (len > 0 && p[len - 1] == ' ')
			len--;
		if (len == 0)
			return "empty column name";
		if (s != NULL)
		{
			for (int i = 0; i < s->ncols && c < 0; i++)
				if (strlen(names[i]) == len && strncmp(names[i], p, len) == 0)
					c = i;
			if (c < 0)
				return "unknown column";
			if (!ok(s, &s->cols[c]))
				return bad;
			leaf[c] = true;
		}
		if (end == NULL)
			return NULL;
		p = end + 1;
		while (*p == ' ')
			p++;
	}
}

/* a scalar INT or float leaf, not a date/time/bool, of the top level (parent < 0) or of the elements of the list parent */
static bool
meas_in(const SiftColSchema *s, const SiftColDef *m, int parent)
{
	if (m->parent != parent || !depth1(s, m) || !plain_type(m))
		return false;
	if (m->store == SIFT_COLS_F4 || m->store == SIFT_COLS_F8)
		return true;
	return m->store == SIFT_COLS_INT &&
		(m->type == SIFT_COLT_INT2 || m->type == SIFT_COLT_INT4 || m->type == SIFT_COLT_INT8 || m->type == SIFT_COLT_NUMERIC);
}

bool
sift_cpagg_meas_ok(const SiftColSchema *s, const SiftColDef *m, const SiftColDef *dim)
{
	return meas_in(s, m, dim->parent);
}

/* a list of the top level: an array of objects, an array of scalars or a map */
static bool
is_list(const SiftColSchema *s, int c)
{
	return s->cols[c].kind == SIFT_COLK_GROUP && s->cols[c].parent < 0;
}

/* The default measures of a dim: the measures of its list, in order of usefulness: decimals and floats (money, scores), then the integers */
static int
default_meas(const SiftColSchema *s, int dim, int16 *meas)
{
	int			nm = 0;

	for (int pass = 0; pass < 2; pass++)
		for (int c = 0; c < s->ncols && nm < SIFT_PAGG_MORE_MEAS; c++)
		{
			const SiftColDef *d = &s->cols[c];
			bool		first = d->type == SIFT_COLT_NUMERIC || d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8;

			if (c != dim && sift_cpagg_meas_ok(s, d, &s->cols[dim]) && first == (pass == 0))
				meas[nm++] = c;
		}
	return nm;
}

/* The measures of the rollup of a list: its numeric leaves (floats and decimals first) */
static void
rollup_measures(const SiftColSchema *s, int list, SiftCPaggRollup *ru)
{
	for (int pass = 0; pass < 2; pass++)
		for (int c = 0; c < s->ncols && ru->nmeas < SIFT_PAGG_MAX_MEAS; c++)
		{
			const SiftColDef *d = &s->cols[c];
			bool		first = d->type == SIFT_COLT_NUMERIC || d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8;

			if (meas_in(s, d, list) && first == (pass == 0))
				ru->meas[ru->nmeas++] = c;
		}
}

/* The rollup of a list by default: every dim candidate among its leaves (up to SIFT_PAGG_MAX_RDIMS) and its measures */
static void
rollup_default(const SiftColSchema *s, int list, SiftCPaggRollup *ru)
{
	memset(ru, 0, sizeof(*ru));
	ru->list = list;
	for (int c = 0; c < s->ncols; c++)
		if (s->cols[c].parent == list && sift_cpagg_dim_ok(s, &s->cols[c]) && ru->ndims < SIFT_PAGG_MAX_RDIMS)
			ru->dim[ru->ndims++] = c;
	rollup_measures(s, list, ru);
}

/* leaf c as the next dimension of the default cube (a calendar cell when cal) */
static void
add_dim(const SiftColSchema *s, SiftCPaggSpec *spec, int c, bool cal)
{
	SiftCPaggDim *sd = &spec->dim[spec->ndims++];

	sd->leaf = c;
	sd->cal = cal;
	sd->nmeas = default_meas(s, c, sd->meas);
	sd->lean = SIFT_PAGG_DEF_MEAS;
}

void
sift_cpagg_spec_default(const SiftColSchema *s, SiftCPaggSpec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->max_groups = SIFT_PAGG_DEF_GROUPS;
	spec->permille = SIFT_PAGG_DEF_PERMILLE;
	for (int c = 0; c < s->ncols; c++)
		if (sift_cpagg_dim_ok(s, &s->cols[c]) && s->cols[c].parent < 0 && spec->ndims < SIFT_PAGG_MAX_DIMS)
			add_dim(s, spec, c, false);
	for (int c = 0; c < s->ncols && spec->nrollups < SIFT_PAGG_MAX_ROLLUPS; c++)
		if (is_list(s, c))
		{
			rollup_default(s, c, &spec->rollup[spec->nrollups]);
			spec->nrollups += spec->rollup[spec->nrollups].ndims > 0;
		}
}

/*
 * Room for n calendar cells in the default cube: the cap is 16 dimensions and the default fills it with the plain columns in their order, which left the
 * cells of the dates out of an index of more than 16 columns (a GROUP BY of a month then scans every unit). The plain dimensions of the default give way, the
 * date and timestamp columns first (the cells of a day answer what their own histogram does) and then the last columns; none gives way when all fit, or
 * for a cube(...) the user wrote.
 */
static void
make_room(const SiftColSchema *s, SiftCPaggSpec *spec, int n)
{
	for (int pass = 0; pass < 2 && !spec->fixed; pass++)
		for (int d = spec->ndims - 1; d >= 0 && spec->ndims + n > SIFT_PAGG_MAX_DIMS; d--)
		{
			if (spec->dim[d].cal || sift_cpagg_cal_type(s, spec->dim[d].leaf) != (pass == 0))
				continue;
			memmove(spec->dim + d, spec->dim + d + 1, (spec->ndims - d - 1) * sizeof(spec->dim[0]));
			spec->ndims--;
		}
}

int
sift_cpagg_cal_count(const SiftColSchema *s)
{
	int			n = 0;

	for (int c = 0; c < s->ncols; c++)
		n += sift_cpagg_cal_type(s, c) && depth1(s, &s->cols[c]) && plain_type(&s->cols[c]);
	return n;
}

void
sift_cpagg_spec_calendar(const SiftColSchema *s, SiftCPaggSpec *spec)
{
	make_room(s, spec, sift_cpagg_cal_count(s));
	for (int c = 0; c < s->ncols; c++)
		if (sift_cpagg_cal_type(s, c) && depth1(s, &s->cols[c]) && plain_type(&s->cols[c]) && spec->ndims < SIFT_PAGG_MAX_DIMS)
			add_dim(s, spec, c, true);
}

int
sift_cpagg_cal_missing(const SiftColSchema *s, const SiftCPaggSpec *spec, int *out, int max)
{
	int			n = 0;

	for (int c = 0; c < s->ncols; c++)
	{
		bool		have = false;

		if (!sift_cpagg_cal_type(s, c) || !depth1(s, &s->cols[c]) || !plain_type(&s->cols[c]))
			continue;
		for (int d = 0; d < spec->ndims; d++)
			have |= spec->dim[d].leaf == c && spec->dim[d].cal;
		if (!have && n++ < max)
			out[n - 1] = c;
	}
	return n;
}

typedef struct OptCur
{
	const char *p;
} OptCur;

static void
skip_ws(OptCur *c)
{
	while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n')
		c->p++;
}

/* A name: up to the next delimiter, trimmed; empty at the end of the text */
static char *
take_name(OptCur *c)
{
	const char *b,
			   *e;
	char	   *n;

	skip_ws(c);
	b = c->p;
	while (*c->p && *c->p != ':' && *c->p != ',' && *c->p != ';' && *c->p != ')')
		c->p++;
	e = c->p;
	while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n'))
		e--;
	n = palloc(e - b + 1);
	memcpy(n, b, e - b);
	n[e - b] = '\0';
	return n;
}

static bool
eat(OptCur *c, char ch)
{
	skip_ws(c);
	if (*c->p != ch)
		return false;
	c->p++;
	return true;
}

static bool
rest_blank(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	return *p == '\0';
}

/* ASCII case-insensitive prefix: does p start with the lower-case word w */
static bool
has_prefix(const char *p, const char *w)
{
	for (; *w; w++, p++)
		if (*p == '\0' || (*p | 0x20) != *w)
			return false;
	return true;
}

/* The option is exactly the word (after blanks) */
static bool
is_word(const char *p, const char *w)
{
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	return has_prefix(p, w) && rest_blank(p + strlen(w));
}

/* The scalar leaf of this name: a list of scalars or a map and its element leaf share a path, and only the element is a dim or a measure */
static int
find_scalar(const SiftColSchema *s, const char *const *names, const char *name)
{
	for (int i = 0; i < s->ncols; i++)
		if (names[i] && s->cols[i].kind == SIFT_COLK_SCALAR && strcmp(names[i], name) == 0)
			return i;
	return -1;
}

/* The rollup of the list `list` in the spec, created when there is none */
static SiftCPaggRollup *
rollup_of(SiftCPaggSpec *spec, int list)
{
	for (int i = 0; i < spec->nrollups; i++)
		if (spec->rollup[i].list == list)
			return &spec->rollup[i];
	if (spec->nrollups == SIFT_PAGG_MAX_ROLLUPS)
		return NULL;
	spec->rollup[spec->nrollups] = (SiftCPaggRollup) {.list = list};
	return &spec->rollup[spec->nrollups++];
}

static int
cmp_rollup_list(const void *a, const void *b)
{
	return ((const SiftCPaggRollup *) a)->list - ((const SiftCPaggRollup *) b)->list;
}

/* "rollup(" was read: entries "d1, d2: m1, m2" of the leaves of one list each, separated by ; */
static const char *
parse_rollup(OptCur *c, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	do
	{
		SiftCPaggRollup *ru = NULL;
		char	   *first = NULL;

		do
		{
			char	   *dn = take_name(c);
			int			leaf = -1;

			if (*dn == '\0')
				return "a rollup entry needs a dimension name";
			if (!s)
				continue;
			leaf = find_scalar(s, names, dn);
			if (leaf < 0)
				return psprintf("rollup dimension \"%s\" is not a column of the index", dn);
			if (s->cols[leaf].parent < 0 || !sift_cpagg_dim_ok(s, &s->cols[leaf]))
				return psprintf("rollup dimension \"%s\" must be an integer, date, numeric or text leaf of a list", dn);
			if (ru == NULL)
			{
				if ((ru = rollup_of(spec, s->cols[leaf].parent)) == NULL)
					return "a rollup option holds at most 8 lists";
				if (ru->ndims > 0)
					return psprintf("the list of \"%s\" has a rollup already", dn);
				first = dn;
			}
			else if (ru->list != s->cols[leaf].parent)
				return psprintf("rollup dimension \"%s\" is not a column of the same list as \"%s\"", dn, first);
			if (ru->ndims == SIFT_PAGG_MAX_RDIMS)
				return "a rollup holds at most 6 dimensions";
			ru->dim[ru->ndims++] = leaf;
		} while (eat(c, ','));
		if (eat(c, ':'))
		{
			do
			{
				char	   *mn = take_name(c);
				int			m = -1;

				if (*mn == '\0')
					return "a measure name is missing";
				if (!s)
					continue;
				m = find_scalar(s, names, mn);
				if (m < 0)
					return psprintf("rollup measure \"%s\" is not a column of the index", mn);
				if (s->cols[m].parent != ru->list)
					return psprintf("rollup measure \"%s\" is not a column of the same list as the dimension \"%s\"", mn, first);
				if (!meas_in(s, &s->cols[m], ru->list))
					return psprintf("rollup measure \"%s\" must be a smallint, integer, bigint, numeric, real or double precision column", mn);
				if (ru->nmeas == SIFT_PAGG_MAX_MEAS)
					return "a rollup holds at most 8 measures";
				ru->meas[ru->nmeas++] = m;
			} while (eat(c, ','));
		}
	} while (eat(c, ';'));
	return NULL;
}

/* "cube(" was read: entries "dim: m1, m2" of top level leaves separated by ; */
static const char *
parse_cube(OptCur *c, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	do
	{
		char	   *dn = take_name(c);
		SiftCPaggDim *sd = NULL;
		int			leaf = -1;

		if (*dn == '\0')
			return "a cube entry needs a dimension name";
		if (s)
		{
			leaf = find_scalar(s, names, dn);
			if (leaf < 0)
				return psprintf("cube dimension \"%s\" is not a column of the index", dn);
			if (s->cols[leaf].parent >= 0)
				return psprintf("cube dimension \"%s\" is a leaf of a list: name it in rollup(...)", dn);
			if (!sift_cpagg_dim_ok(s, &s->cols[leaf]))
				return psprintf("cube dimension \"%s\" must be a scalar integer, date, numeric or text column", dn);
			if (spec->ndims == SIFT_PAGG_MAX_DIMS)
				return "a cube holds at most 16 dimensions";
			sd = &spec->dim[spec->ndims++];
			*sd = (SiftCPaggDim) {.leaf = leaf};
		}
		if (eat(c, ':'))
		{
			do
			{
				char	   *mn = take_name(c);
				int			m = -1;

				if (*mn == '\0')
					return "a measure name is missing";
				if (!s)
					continue;
				m = find_scalar(s, names, mn);
				if (m < 0)
					return psprintf("cube measure \"%s\" is not a column of the index", mn);
				if (s->cols[m].parent != s->cols[sd->leaf].parent)
					return psprintf("cube measure \"%s\" is not a column of the same list as the dimension \"%s\"", mn, dn);
				if (!sift_cpagg_meas_ok(s, &s->cols[m], &s->cols[sd->leaf]))
					return psprintf("cube measure \"%s\" must be a smallint, integer, bigint, numeric, real or double precision column", mn);
				if (sd->nmeas == SIFT_PAGG_MAX_MEAS)
					return "a cube dimension holds at most 8 measures";
				sd->meas[sd->nmeas++] = m;
			} while (eat(c, ','));
		}
	} while (eat(c, ';'));
	return NULL;
}

/* "auto: a, b" was read: the default cube with these plain dimensions only, in this order (what CREATE INDEX chose, col/cubecols.c) */
static const char *
parse_auto(OptCur *c, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	if (s)
		spec->ndims = 0;
	do
	{
		char	   *dn = take_name(c);
		int			leaf;

		if (*dn == '\0')
			return "a column name is missing";
		if (!s)
			continue;
		if ((leaf = find_scalar(s, names, dn)) < 0)
			return psprintf("\"%s\" is not a column of the index", dn);
		if (s->cols[leaf].parent >= 0 || !sift_cpagg_dim_ok(s, &s->cols[leaf]))
			return psprintf("\"%s\" must be a top-level integer, date, timestamp, bool or text column", dn);
		for (int j = 0; j < spec->ndims; j++)
			if (spec->dim[j].leaf == leaf)
				return psprintf("\"%s\" is named twice", dn);
		if (spec->ndims == SIFT_PAGG_MAX_DIMS)
			return "too many dimensions";
		add_dim(s, spec, leaf, false);
	} while (eat(c, ','));
	return *c->p ? psprintf("unexpected \"%s\"", c->p) : NULL;
}

/*
 * One parser for both the syntax check (s == NULL: names are not resolved) and the real thing.
 */
static const char *
parse_option(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	OptCur		cur = {opt};
	OptCur	   *c = &cur;

	if (s)
		sift_cpagg_spec_default(s, spec);
	if (opt == NULL || rest_blank(opt) || is_word(opt, "default") || is_word(opt, "auto"))
		return NULL;
	if (is_word(opt, "off"))
	{
		if (s)
		{
			memset(spec, 0, sizeof(*spec));
			spec->off = true;
		}
		return NULL;
	}
	skip_ws(c);
	if (has_prefix(c->p, "auto:"))
	{
		c->p += 5;
		return parse_auto(c, s, names, spec);
	}
	do
	{
		const char *err;
		bool		cube = has_prefix(c->p, "cube"),
					rollup = !cube && has_prefix(c->p, "rollup");

		if (!cube && !rollup)
			return psprintf("expected default, auto, off, cube(...) or rollup(...), got \"%s\"", c->p);
		c->p += cube ? 4 : 6;
		if (!eat(c, '('))
			return cube ? "expected ( after cube" : "expected ( after rollup";
		/* a clause replaces the default of its kind, the other kind keeps the default */
		if (s && cube && !spec->fixed)
		{
			spec->ndims = 0;
			spec->fixed = true;
			spec->max_groups = SIFT_PAGG_CUBE_GROUPS;
			spec->permille = 0;
		}
		if (s && rollup && !spec->rfixed)
		{
			spec->nrollups = 0;
			spec->rfixed = true;
		}
		if ((err = cube ? parse_cube(c, s, names, spec) : parse_rollup(c, s, names, spec)) != NULL)
			return err;
		if (!eat(c, ')'))
			return cube ? "expected ) at the end of cube(...)" : "expected ) at the end of rollup(...)";
		eat(c, ',');
		skip_ws(c);
	} while (*c->p);
	if (s && spec->nrollups > 1)
		qsort(spec->rollup, spec->nrollups, sizeof(SiftCPaggRollup), cmp_rollup_list);
	return NULL;
}

/* "off", or the dims "name, name, ..." (the measures are the numeric leaves of their lists) */
static const char *
parse_rollup_dims(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	OptCur		cur = {opt};
	OptCur	   *c = &cur;

	if (is_word(opt, "off"))
	{
		if (s)
			spec->nrollups = 0;
		return NULL;
	}
	if (s)
		spec->nrollups = 0;
	do
	{
		char	   *dn = take_name(c);
		SiftCPaggRollup *ru;
		int			leaf;

		if (*dn == '\0')
			return "a leaf name is missing";
		if (!s)
			continue;
		if ((leaf = find_scalar(s, names, dn)) < 0)
			return psprintf("\"%s\" is not a leaf declared in paths", dn);
		if (s->cols[leaf].parent < 0)
			return psprintf("\"%s\" is not a leaf of an array element or a map value", dn);
		if (!sift_cpagg_dim_ok(s, &s->cols[leaf]))
			return psprintf("\"%s\" is not a low-cardinality leaf: it must be a boolean, integer, date, numeric or text leaf", dn);
		if ((ru = rollup_of(spec, s->cols[leaf].parent)) == NULL)
			return "a rollup holds at most 8 lists";
		if (ru->ndims == SIFT_PAGG_MAX_RDIMS)
			return psprintf("the list of \"%s\" has more than %d rollup leaves", dn, SIFT_PAGG_MAX_RDIMS);
		for (int j = 0; j < ru->ndims; j++)
			if (ru->dim[j] == leaf)
				return psprintf("\"%s\" is named twice", dn);
		ru->dim[ru->ndims++] = leaf;
	} while (eat(c, ','));
	if (*c->p)
		return psprintf("unexpected \"%s\"", c->p);
	if (s)
	{
		for (int i = 0; i < spec->nrollups; i++)
			rollup_measures(s, spec->rollup[i].list, &spec->rollup[i]);
		qsort(spec->rollup, spec->nrollups, sizeof(SiftCPaggRollup), cmp_rollup_list);
	}
	return NULL;
}

/* a measure of the rows' rollup: an integer or numeric top-level leaf (a float sum depends on the order of the additions, so none is stored) */
static bool
rows_meas_ok(const SiftColSchema *s, const SiftColDef *m)
{
	return meas_in(s, m, -1) && m->store == SIFT_COLS_INT;
}

/* "a, b" or "a, b: m1, m2" (dims, then measures) of the rows of a unit: the rollup of list -1 (column format minor 8); "off" and "" name none */
static const char *
parse_rows(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	OptCur		cur = {opt};
	OptCur	   *c = &cur;
	SiftCPaggRollup ru = {.list = -1};
	int			nrows = 0;

	if (is_word(opt, "off") || is_word(opt, "auto"))
		return NULL;
	skip_ws(c);
	if (has_prefix(c->p, "auto:"))
		c->p += 5;
	do
	{
		char	   *dn = take_name(c);
		int			leaf;

		if (*dn == '\0')
			return "a column name is missing";
		nrows++;
		if (!s)
			continue;
		if ((leaf = find_scalar(s, names, dn)) < 0)
			return psprintf("\"%s\" is not a column of the index", dn);
		if (s->cols[leaf].parent >= 0)
			return psprintf("\"%s\" is a leaf of a list: name it in rollup", dn);
		if (!sift_cpagg_dim_ok(s, &s->cols[leaf]))
			return psprintf("\"%s\" must be a boolean, integer, date, numeric or text column", dn);
		for (int j = 0; j < ru.ndims; j++)
			if (ru.dim[j] == leaf)
				return psprintf("\"%s\" is named twice", dn);
		if (ru.ndims == SIFT_PAGG_MAX_RDIMS)
			return psprintf("a rollup of the rows holds at most %d columns", SIFT_PAGG_MAX_RDIMS);
		ru.dim[ru.ndims++] = leaf;
	} while (eat(c, ','));
	if (nrows < 2)
		return "rollup_dims needs at least two columns (the cube answers one)";
	if (eat(c, ':'))
	{
		do
		{
			char	   *mn = take_name(c);
			int			m;

			if (*mn == '\0')
				return "a measure name is missing";
			if (!s)
				continue;
			if ((m = find_scalar(s, names, mn)) < 0)
				return psprintf("\"%s\" is not a column of the index", mn);
			if (!rows_meas_ok(s, &s->cols[m]))
				return psprintf("measure \"%s\" must be a smallint, integer, bigint or numeric column", mn);
			if (ru.nmeas == SIFT_PAGG_MAX_MEAS)
				return "a rollup holds at most 8 measures";
			ru.meas[ru.nmeas++] = m;
		} while (eat(c, ','));
	}
	else if (s)
	{
		/* no measures named: the integer and numeric columns that are not dims, decimals first */
		for (int pass = 0; pass < 2; pass++)
			for (int m = 0; m < s->ncols && ru.nmeas < SIFT_PAGG_MAX_MEAS; m++)
			{
				bool		dim = false;

				for (int j = 0; j < ru.ndims; j++)
					dim |= ru.dim[j] == m;
				if (!dim && rows_meas_ok(s, &s->cols[m]) && (s->cols[m].type == SIFT_COLT_NUMERIC) == (pass == 0))
					ru.meas[ru.nmeas++] = m;
			}
	}
	if (*c->p)
		return psprintf("unexpected \"%s\"", c->p);
	if (s && spec->nrollups == SIFT_PAGG_MAX_ROLLUPS)
		return "a rollup option holds at most 8 lists";
	if (s)
	{
		memmove(spec->rollup + 1, spec->rollup, spec->nrollups * sizeof(SiftCPaggRollup));	/* ascending list order: the rows' -1 first */
		spec->rollup[0] = ru;
		spec->nrollups++;
	}
	return NULL;
}

const char *
sift_cpagg_rows_apply(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	return opt == NULL || rest_blank(opt) ? NULL : parse_rows(opt, s, names, spec->off ? &(SiftCPaggSpec) {0} : spec);
}

const char *
sift_cpagg_rows_syntax(const char *opt)
{
	return opt == NULL || rest_blank(opt) ? NULL : parse_rows(opt, NULL, NULL, NULL);
}

/* "all", "off", or the date and timestamp columns "a, b" of the calendar cells of every unit (column format minor 8) */
static const char *
parse_calendar(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	OptCur		cur = {opt};
	OptCur	   *c = &cur;
	bool		all = is_word(opt, "all");

	if (is_word(opt, "off"))
		return NULL;
	if (s && s->kinds)
		return "needs a schema without paths or kinds";
	if (all)
	{
		if (s)
		{
			sift_cpagg_spec_calendar(s, spec);
			spec->cal_all = true;
		}
		return NULL;
	}
	do
	{
		char	   *dn = take_name(c);
		int			leaf;

		if (*dn == '\0')
			return "a column name is missing";
		if (!s)
			continue;
		if ((leaf = find_scalar(s, names, dn)) < 0)
			return psprintf("\"%s\" is not a column of the index", dn);
		if (!sift_cpagg_cal_type(s, leaf) || !depth1(s, &s->cols[leaf]) || !plain_type(&s->cols[leaf]))
			return psprintf("\"%s\" must be a date, timestamp or timestamptz column", dn);
		for (int j = 0; j < spec->ndims; j++)
			if (spec->dim[j].leaf == leaf && spec->dim[j].cal)
				return psprintf("\"%s\" is named twice", dn);
		make_room(s, spec, 1);
		if (spec->ndims == SIFT_PAGG_MAX_DIMS)
			return "too many dimensions";
		spec->dim[spec->ndims] = (SiftCPaggDim) {.leaf = leaf, .cal = true, .lean = SIFT_PAGG_DEF_MEAS};
		spec->dim[spec->ndims].nmeas = default_meas(s, leaf, spec->dim[spec->ndims].meas);
		spec->ndims++;
	} while (eat(c, ','));
	return *c->p ? psprintf("unexpected \"%s\"", c->p) : NULL;
}

const char *
sift_cpagg_calendar_apply(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	if (opt == NULL || rest_blank(opt))
	{
		/* the default is every date, timestamp and timestamptz column, as for a sift_table; not with preagg = 'off', a cube given dim by dim, or JSON path leaves (minor 8 has no kinds) */
		if (!spec->off && !spec->fixed && !s->kinds)
		{
			sift_cpagg_spec_calendar(s, spec);
			spec->cal_all = true;
		}
		return NULL;
	}
	return parse_calendar(opt, s, names, spec->off ? &(SiftCPaggSpec) {0} : spec);
}

const char *
sift_cpagg_calendar_syntax(const char *opt)
{
	return opt == NULL || rest_blank(opt) ? NULL : parse_calendar(opt, NULL, NULL, NULL);
}

const char *
sift_cpagg_rollup_apply(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	if (opt == NULL || rest_blank(opt))
		return NULL;
	spec->rfixed = true;
	return parse_rollup_dims(opt, s, names, spec);
}

const char *
sift_cpagg_rollup_syntax(const char *opt)
{
	return opt == NULL || rest_blank(opt) ? NULL : parse_rollup_dims(opt, NULL, NULL, NULL);
}

const char *
sift_cpagg_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	return parse_option(opt, s, names, spec);
}

const char *
sift_cpagg_syntax(const char *opt)
{
	return parse_option(opt, NULL, NULL, NULL);
}
