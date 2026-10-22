/*
 * col/preagg_opt.c - the pre-aggregates option: the leaves that can be dims and measures, the default spec, the parser (docs/COL-PREAGG.md).
 */

#include "core/host.h"

#include "col/preagg.h"

/* ---------- leaves that can be dims and measures ---------- */

/* a scalar leaf of the top level, or of a list of the top level: a list inside a list has no cube */
static bool
depth1(const SiftColSchema *s, const SiftColDef *d)
{
	return d->kind == SIFT_COLK_SCALAR && (d->parent < 0 || s->cols[d->parent].parent < 0);
}

/* A leaf of a wide value (a lane, its head) or of money / interval is no dimension or measure of the pre-aggregates: its sums and keys are not the value's (docs/COL-LEAFTYPES-32.md) */
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

/* ---------- the option ---------- */

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

void
sift_cpagg_spec_default(const SiftColSchema *s, SiftCPaggSpec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->max_groups = SIFT_PAGG_DEF_GROUPS;
	spec->permille = SIFT_PAGG_DEF_PERMILLE;
	for (int c = 0; c < s->ncols; c++)
	{
		SiftCPaggDim *sd;

		if (!sift_cpagg_dim_ok(s, &s->cols[c]) || s->cols[c].parent >= 0 || spec->ndims == SIFT_PAGG_MAX_DIMS)
			continue;
		sd = &spec->dim[spec->ndims++];
		sd->leaf = c;
		sd->nmeas = default_meas(s, c, sd->meas);
		sd->lean = SIFT_PAGG_DEF_MEAS;
	}
	for (int c = 0; c < s->ncols && spec->nrollups < SIFT_PAGG_MAX_ROLLUPS; c++)
		if (is_list(s, c))
		{
			rollup_default(s, c, &spec->rollup[spec->nrollups]);
			spec->nrollups += spec->rollup[spec->nrollups].ndims > 0;
		}
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
	if (opt == NULL || rest_blank(opt) || is_word(opt, "default"))
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
	do
	{
		const char *err;
		bool		cube = has_prefix(c->p, "cube"),
					rollup = !cube && has_prefix(c->p, "rollup");

		if (!cube && !rollup)
			return psprintf("expected default, off, cube(...) or rollup(...), got \"%s\"", c->p);
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
