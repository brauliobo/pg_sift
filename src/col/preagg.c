/*
 * col/preagg.c - pre-aggregates of a unit: exact integer sums, the mini cube (GROUP BY dim -> count, per measure non-NULL count and sum),
 * the rollups of the lists, their byte formats, their option and their verification. Design and the rules of use: docs/COL-PREAGG.md.
 * Portable (sift_host.h only).
 *
 * Cube blob of a unit (all integers LEB128 varints unless said): u8 ndims, a directory of ndims x (leaf, bytes of its body) so that one dim is
 * found without reading the others, then the bodies:
 *     u8 nmeas, ngroups, u8 flags (bit 0: the last group is the NULL group), nmeas x measure leaf,
 *     keys of the groups with a value in ascending order (first: zigzag, then the positive differences),
 *     counts[ngroups],
 *     per measure: nn[ngroups], then sum[ngroups] (INT measure: zigzag 128-bit; float measure: 8 bytes, the double's bits)
 *
 * Element blob of a unit (column format minor 4): u8 nlists, nlists x (leaf of the list, bytes of its rollup) as varints, then the rollups in that
 * order. Rollup of a list (its rows are the elements): u8 ndims, u8 nmeas, ntuples, ndims x dim leaf, nmeas x measure leaf, then
 *     per dim: u8 anynull, (anynull: a bitmap of the tuples that have a NULL there, bit i of byte i / 8), then for the tuples with a value
 *     the key as the zigzag difference to the previous one (the tuples are in ascending order of their keys, dim 0 first, a NULL after the values),
 *     counts[ntuples] (elements of the tuple),
 *     per measure: nn[ntuples], sum[ntuples] (as in the cube), then the smallest value of each tuple that has one and the largest
 *     (INT measure: zigzag of the key; float measure: 8 bytes each)
 */
#include "sift_host.h"

#include "col/preagg.h"

#define PAGG_NULLGROUP	0x01

/* ---------- leaves that can be dims and measures ---------- */

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
		for (int c = 0; c < s->ncols && nm < SIFT_PAGG_DEF_MEAS; c++)
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

/* ---------- varints ---------- */

static void
put_u(StringInfo out, uint64 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(out, (char) (v | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(out, (char) v);
}

static void
put_zz(StringInfo out, int64 v)
{
	put_u(out, ((uint64) v << 1) ^ (uint64) (v >> 63));
}

static void
put_zz128(StringInfo out, SiftCI128 v)
{
	unsigned __int128 u = ((unsigned __int128) v << 1) ^ (unsigned __int128) (v >> 127);

	while (u >= 0x80)
	{
		appendStringInfoChar(out, (char) ((uint8) u | 0x80));
		u >>= 7;
	}
	appendStringInfoChar(out, (char) (uint8) u);
}

typedef struct Rd
{
	const uint8 *p;
	const uint8 *end;
	bool		bad;
} Rd;

static uint64
get_u(Rd *r)
{
	uint64		v = 0;

	for (int sh = 0; sh < 70; sh += 7)
	{
		uint8		b;

		if (r->p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *r->p++;
		v |= (uint64) (b & 0x7F) << sh;
		if (!(b & 0x80))
			return v;
	}
	r->bad = true;
	return 0;
}

static int64
get_zz(Rd *r)
{
	uint64		u = get_u(r);

	return (int64) (u >> 1) ^ -(int64) (u & 1);
}

static SiftCI128
get_zz128(Rd *r)
{
	unsigned __int128 u = 0;
	uint64		w = 0;
	const uint8 *p = r->p;

	/* nearly every sum fits 63 bits: no 128-bit shifts for those */
	for (int sh = 0; sh < 63; sh += 7)
	{
		uint8		b;

		if (p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *p++;
		w |= (uint64) (b & 0x7F) << sh;
		if (!(b & 0x80))
		{
			r->p = p;
			return (SiftCI128) ((int64) (w >> 1) ^ -(int64) (w & 1));
		}
	}
	for (int sh = 0; sh < 133; sh += 7)
	{
		uint8		b;

		if (r->p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *r->p++;
		u |= (unsigned __int128) (b & 0x7F) << sh;
		if (!(b & 0x80))
			return (SiftCI128) (u >> 1) ^ -(SiftCI128) (u & 1);
	}
	r->bad = true;
	return 0;
}

/* ---------- exact leaf sums ---------- */

static inline bool
is_null(const uint64 *nulls, uint32 i)
{
	return nulls != NULL && ((nulls[i / 64] >> (i % 64)) & 1);
}

void
sift_cpagg_leaf(const SiftCPaggCol *c, SiftCLeafAgg *a)
{
	memset(a, 0, sizeof(*a));
	for (uint32 i = 0; i < c->n; i++)
	{
		SiftCI128	k;

		if (is_null(c->nulls, i))
			continue;
		k = c->key[i];
		a->sum += k;				/* 74,496 values of 2^63 cannot leave int128 */
		if (__builtin_add_overflow(a->sumsq, k * k, &a->sumsq))
			a->flags |= SIFT_CAGG_SUMSQ_OVF;
	}
}

/* ---------- the cube of a unit ---------- */

typedef struct GroupKey
{
	int64		key;
	uint32		g;
	uint32		isnull;
} GroupKey;

/* Work areas of one unit's cube, sized by the group cap */
typedef struct Scratch
{
	uint32		hsize;
	uint32	   *gid;				/* [nrows] */
	int64	   *hk;					/* [hsize] */
	int32	   *hg;					/* [hsize] */
	int64	   *gkey;				/* [maxg + 1] */
	uint32	   *cnt;
	uint32	   *nn;
	GroupKey   *ord;
	double	   *sf;
	SiftCI128  *si;
} Scratch;

typedef struct DimBuild
{
	int			spec;				/* index into spec->dim */
	StringInfoData blob;
	Size		bytes;
	bool		ok;
	bool		keep;
} DimBuild;

static int
cmp_group(const void *a, const void *b)
{
	const GroupKey *x = a,
			   *y = b;

	if (x->isnull != y->isnull)
		return x->isnull ? 1 : -1;
	return x->key < y->key ? -1 : x->key > y->key;
}

/* The writer keeps the overflow bitmap of a leaf once a unit needed it, zeroed for the next ones: no bit set is no overflow value */
static bool
has_ovf(const SiftCPaggCol *c)
{
	for (uint32 w = 0; c->ovf != NULL && w < (c->n + 63) / 64; w++)
		if (c->ovf[w])
			return true;
	return false;
}

static inline bool
col_usable(const SiftCPaggCol *c, uint32 nrows)
{
	return c->ready && c->n == nrows && !has_ovf(c);
}

/* Builds the histogram of dim sd over the unit; false when it has more than maxg groups or the dim cannot be used (overflow values, inline text) */
static bool
build_dim(const SiftColSchema *s, const SiftCPaggDim *sd, const SiftCPaggCol *cols, uint32 nrows, uint32 maxg, const Scratch *sc,
		  StringInfo out)
{
	const SiftCPaggCol *dc = &cols[sd->leaf];
	uint32		ng = 0,
				nullg = UINT32_MAX,
				hsize = sc->hsize,
				mask = hsize - 1,
			   *gid = sc->gid,
			   *cnt = sc->cnt,
			   *nn = sc->nn;
	int64	   *hk = sc->hk,
			   *gkey = sc->gkey;
	int32	   *hg = sc->hg;
	int			mm[SIFT_PAGG_MAX_MEAS],
				nm = 0;
	GroupKey   *ord = sc->ord;

	if (!col_usable(dc, nrows) || nrows == 0)
		return false;
	memset(hg, 0xFF, hsize * sizeof(int32));
	for (uint32 i = 0; i < nrows; i++)
	{
		uint32		g;

		if (is_null(dc->nulls, i))
		{
			if (nullg == UINT32_MAX)
			{
				if (ng == maxg)
					return false;
				nullg = ng;
				gkey[ng] = 0;
				cnt[ng++] = 0;
			}
			g = nullg;
		}
		else
		{
			int64		k = dc->key[i];
			uint32		h = (uint32) (((uint64) k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & mask;

			while (hg[h] >= 0 && hk[h] != k)
				h = (h + 1) & mask;
			if (hg[h] < 0)
			{
				if (ng == maxg)
					return false;
				hk[h] = k;
				hg[h] = ng;
				gkey[ng] = k;
				cnt[ng++] = 0;
			}
			g = hg[h];
		}
		gid[i] = g;
		cnt[g]++;
	}
	for (uint32 g = 0; g < ng; g++)
		ord[g] = (GroupKey) {gkey[g], g, g == nullg};
	qsort(ord, ng, sizeof(GroupKey), cmp_group);
	for (int i = 0; i < sd->nmeas; i++)
		if (sd->meas[i] != sd->leaf && col_usable(&cols[sd->meas[i]], nrows))
			mm[nm++] = sd->meas[i];
	appendStringInfoChar(out, (char) nm);
	put_u(out, ng);
	appendStringInfoChar(out, nullg == UINT32_MAX ? 0 : PAGG_NULLGROUP);
	for (int i = 0; i < nm; i++)
		put_u(out, mm[i]);
	{
		int64		prev = 0;
		uint32		nk = ng - (nullg == UINT32_MAX ? 0 : 1);

		for (uint32 i = 0; i < nk; i++)
		{
			if (i == 0)
				put_zz(out, ord[i].key);
			else
				put_u(out, (uint64) ord[i].key - (uint64) prev);
			prev = ord[i].key;
		}
	}
	for (uint32 i = 0; i < ng; i++)
		put_u(out, cnt[ord[i].g]);
	for (int m = 0; m < nm; m++)
	{
		const SiftCPaggCol *mc = &cols[mm[m]];
		bool		fl = s->cols[mm[m]].store == SIFT_COLS_F4 || s->cols[mm[m]].store == SIFT_COLS_F8;
		double	   *sf = sc->sf;
		SiftCI128  *si = sc->si;

		memset(nn, 0, ng * sizeof(uint32));
		if (fl)
		{
			for (uint32 i = 0; i < nrows; i++)
			{
				double		v;

				if (is_null(mc->nulls, i))
					continue;
				v = sift_col_f8_from_key(mc->key[i]);
				/* the first value seeds the sum, as float8pl's transition does: a group of -0 sums to -0 */
				sf[gid[i]] = nn[gid[i]]++ == 0 ? v : sf[gid[i]] + v;
			}
			for (uint32 i = 0; i < ng; i++)
				put_u(out, nn[ord[i].g]);
			for (uint32 i = 0; i < ng; i++)
			{
				double		v = nn[ord[i].g] ? sf[ord[i].g] : 0;

				appendBinaryStringInfo(out, (const char *) &v, sizeof(double));
			}
		}
		else
		{
			memset(si, 0, ng * sizeof(SiftCI128));
			for (uint32 i = 0; i < nrows; i++)
				if (!is_null(mc->nulls, i))
				{
					si[gid[i]] += mc->key[i];
					nn[gid[i]]++;
				}
			for (uint32 i = 0; i < ng; i++)
				put_u(out, nn[ord[i].g]);
			for (uint32 i = 0; i < ng; i++)
				put_zz128(out, si[ord[i].g]);
		}
	}
	return true;
}

/* The kept dims as a cube blob: the directory of (leaf, bytes), then the bodies; nothing when none is kept */
static void
emit_cube(const SiftCPaggSpec *spec, const DimBuild *db, int nd, StringInfo out)
{
	int			kept = 0;

	for (int d = 0; d < nd; d++)
		kept += db[d].keep;
	if (kept == 0)
		return;
	appendStringInfoChar(out, (char) kept);
	for (int d = 0; d < nd; d++)
		if (db[d].keep)
		{
			put_u(out, spec->dim[db[d].spec].leaf);
			put_u(out, db[d].blob.len);
		}
	for (int d = 0; d < nd; d++)
		if (db[d].keep)
			appendBinaryStringInfo(out, db[d].blob.data, db[d].blob.len);
}

/* The histogram of every dim of the spec */
static void
build_dims(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, uint32 maxg, DimBuild *db)
{
	uint32		hsize = 16;
	Scratch		sc;

	while (hsize < 2 * (maxg + 1))
		hsize *= 2;
	sc.hsize = hsize;
	sc.gid = palloc(nrows * sizeof(uint32));
	sc.hk = palloc(hsize * sizeof(int64));
	sc.hg = palloc(hsize * sizeof(int32));
	sc.gkey = palloc((maxg + 1) * sizeof(int64));
	sc.cnt = palloc((maxg + 1) * sizeof(uint32));
	sc.nn = palloc((maxg + 1) * sizeof(uint32));
	sc.ord = palloc((maxg + 1) * sizeof(GroupKey));
	sc.sf = palloc((maxg + 1) * sizeof(double));
	sc.si = palloc((maxg + 1) * sizeof(SiftCI128));
	for (int d = 0; d < spec->ndims; d++)
	{
		db[d] = (DimBuild) {.spec = d};
		initStringInfo(&db[d].blob);
		db[d].ok = build_dim(s, &spec->dim[d], cols, nrows, maxg, &sc, &db[d].blob);
		db[d].bytes = db[d].blob.len;
	}
	pfree(sc.gid);
	pfree(sc.hk);
	pfree(sc.hg);
	pfree(sc.gkey);
	pfree(sc.cnt);
	pfree(sc.nn);
	pfree(sc.ord);
	pfree(sc.sf);
	pfree(sc.si);
}

/* ---------- the rollup of a list ---------- */

typedef struct RuBuild
{
	int			nd,
				nm;
	const SiftCPaggCol *dc[SIFT_PAGG_MAX_RDIMS],
			   *mc[SIFT_PAGG_MAX_MEAS];
	int16		dleaf[SIFT_PAGG_MAX_RDIMS],
				mleaf[SIFT_PAGG_MAX_MEAS];
	bool		mfl[SIFT_PAGG_MAX_MEAS];
	uint32		n,					/* elements */
				cap,				/* the most tuples */
				nt,
				hsize;
	uint32	   *gid,				/* [n] the tuple of each element */
			   *cnt;				/* [cap] */
	int64	   *tkey;				/* [cap * nd] */
	uint8	   *tnull;				/* [cap] bit j: dim j is NULL */
	int32	   *hslot;				/* [hsize] */
} RuBuild;

/* the order of the tuples: dim 0 first, the NULL of a dim after its values; the sort callback has no argument, the backend is one thread */
typedef struct RuOrder
{
	int			nd;
	const int64 *key;
	const uint8 *null;
} RuOrder;

static const RuOrder *ru_order;

static int
cmp_tuple(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	for (int j = 0; j < ru_order->nd; j++)
	{
		bool		nx = ru_order->null[x] >> j & 1,
					ny = ru_order->null[y] >> j & 1;
		int64		kx = ru_order->key[(Size) x * ru_order->nd + j],
					ky = ru_order->key[(Size) y * ru_order->nd + j];

		if (nx != ny)
			return nx ? 1 : -1;
		if (kx != ky)
			return kx < ky ? -1 : 1;
	}
	return x < y ? -1 : x > y;
}

/* The distinct tuples of the dims over the elements, with the elements of each; false past the cap */
static bool
ru_tuples(RuBuild *b)
{
	uint32		mask = b->hsize - 1;

	memset(b->hslot, 0xFF, b->hsize * sizeof(int32));
	b->nt = 0;
	for (uint32 i = 0; i < b->n; i++)
	{
		int64		k[SIFT_PAGG_MAX_RDIMS];
		uint8		nm = 0;
		uint64		h = 0;
		uint32		pos,
					t;

		for (int j = 0; j < b->nd; j++)
		{
			if (is_null(b->dc[j]->nulls, i))
			{
				nm |= 1 << j;
				k[j] = 0;
			}
			else
				k[j] = b->dc[j]->key[i];
			h = (h ^ (uint64) k[j]) * UINT64CONST(0x9E3779B97F4A7C15);
			h ^= h >> 31;
		}
		h = (h ^ nm) * UINT64CONST(0x9E3779B97F4A7C15);
		pos = (uint32) (h >> 24) & mask;
		for (;;)
		{
			int32		sl = b->hslot[pos];

			if (sl < 0)
			{
				if (b->nt == b->cap)
					return false;
				t = b->nt++;
				memcpy(b->tkey + (Size) t * b->nd, k, b->nd * sizeof(int64));
				b->tnull[t] = nm;
				b->cnt[t] = 0;
				b->hslot[pos] = t;
				break;
			}
			if (b->tnull[sl] == nm && memcmp(b->tkey + (Size) sl * b->nd, k, b->nd * sizeof(int64)) == 0)
			{
				t = sl;
				break;
			}
			pos = (pos + 1) & mask;
		}
		b->gid[i] = t;
		b->cnt[t]++;
	}
	return true;
}

/* The blob of the tuples found by ru_tuples (format at the top of the file) */
static void
ru_encode(const RuBuild *b, StringInfo out)
{
	uint32		nt = b->nt,
			   *ord = palloc(nt * sizeof(uint32)),
			   *nn = palloc(nt * sizeof(uint32));
	RuOrder		od = {b->nd, b->tkey, b->tnull};
	SiftCI128  *si = palloc(nt * sizeof(SiftCI128));
	int64	   *mn = palloc(nt * sizeof(int64)),
			   *mx = palloc(nt * sizeof(int64));
	double	   *sf = palloc(nt * sizeof(double)),
			   *fmn = palloc(nt * sizeof(double)),
			   *fmx = palloc(nt * sizeof(double));

	for (uint32 t = 0; t < nt; t++)
		ord[t] = t;
	ru_order = &od;
	qsort(ord, nt, sizeof(uint32), cmp_tuple);
	appendStringInfoChar(out, (char) b->nd);
	appendStringInfoChar(out, (char) b->nm);
	put_u(out, nt);
	for (int j = 0; j < b->nd; j++)
		put_u(out, b->dleaf[j]);
	for (int m = 0; m < b->nm; m++)
		put_u(out, b->mleaf[m]);
	for (int j = 0; j < b->nd; j++)
	{
		bool		any = false;
		int64		prev = 0;

		for (uint32 t = 0; t < nt; t++)
			any |= (b->tnull[t] >> j & 1) != 0;
		appendStringInfoChar(out, any);
		if (any)
		{
			uint32		nb = (nt + 7) / 8,
						at = out->len;

			for (uint32 i = 0; i < nb; i++)
				appendStringInfoChar(out, 0);
			for (uint32 i = 0; i < nt; i++)
				if (b->tnull[ord[i]] >> j & 1)
					out->data[at + i / 8] |= (char) (1 << (i % 8));
		}
		for (uint32 i = 0; i < nt; i++)
		{
			int64		k;

			if (b->tnull[ord[i]] >> j & 1)
				continue;
			k = b->tkey[(Size) ord[i] * b->nd + j];
			put_zz(out, (int64) ((uint64) k - (uint64) prev));
			prev = k;
		}
	}
	for (uint32 i = 0; i < nt; i++)
		put_u(out, b->cnt[ord[i]]);
	for (int m = 0; m < b->nm; m++)
	{
		const SiftCPaggCol *mc = b->mc[m];

		memset(nn, 0, nt * sizeof(uint32));
		for (uint32 i = 0; i < b->n; i++)
		{
			uint32		g = b->gid[i];

			if (is_null(mc->nulls, i))
				continue;
			if (b->mfl[m])
			{
				double		v = sift_col_f8_from_key(mc->key[i]);

				if (nn[g]++ == 0)
					sf[g] = fmn[g] = fmx[g] = v;
				else
				{
					sf[g] += v;
					fmn[g] = v < fmn[g] ? v : fmn[g];
					fmx[g] = v > fmx[g] ? v : fmx[g];
				}
			}
			else
			{
				int64		v = mc->key[i];

				if (nn[g]++ == 0)
				{
					si[g] = 0;
					mn[g] = mx[g] = v;
				}
				si[g] += v;
				mn[g] = Min(mn[g], v);
				mx[g] = Max(mx[g], v);
			}
		}
		for (uint32 i = 0; i < nt; i++)
			put_u(out, nn[ord[i]]);
		for (uint32 i = 0; i < nt; i++)
		{
			uint32		g = ord[i];

			if (b->mfl[m])
			{
				double		v = nn[g] ? sf[g] : 0;

				appendBinaryStringInfo(out, (const char *) &v, sizeof(double));
			}
			else
				put_zz128(out, nn[g] ? si[g] : 0);
		}
		for (int pass = 0; pass < 2; pass++)
			for (uint32 i = 0; i < nt; i++)
			{
				uint32		g = ord[i];

				if (nn[g] == 0)
					continue;
				if (b->mfl[m])
					appendBinaryStringInfo(out, (const char *) (pass ? &fmx[g] : &fmn[g]), sizeof(double));
				else
					put_zz(out, pass ? mx[g] : mn[g]);
			}
	}
	pfree(ord);
	pfree(nn);
	pfree(si);
	pfree(mn);
	pfree(mx);
	pfree(sf);
	pfree(fmn);
	pfree(fmx);
}

/* The distinct values (a NULL one more) of an element column, or limit + 1 when there are more than limit */
static uint32
ru_distinct(const SiftCPaggCol *c, uint32 n, uint32 limit, int64 *hk, uint8 *hu, uint32 hsize)
{
	uint32		mask = hsize - 1,
				nd = 0;
	bool		nullseen = false;

	memset(hu, 0, hsize);
	for (uint32 i = 0; i < n; i++)
	{
		int64		k;
		uint32		h;

		if (is_null(c->nulls, i))
		{
			if (!nullseen && ++nd > limit)
				return limit + 1;
			nullseen = true;
			continue;
		}
		k = c->key[i];
		h = (uint32) (((uint64) k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & mask;
		while (hu[h] && hk[h] != k)
			h = (h + 1) & mask;
		if (!hu[h])
		{
			if (++nd > limit)
				return limit + 1;
			hu[h] = 1;
			hk[h] = k;
		}
	}
	return nd;
}

static int
cmp_i16(const void *a, const void *b)
{
	return *(const int16 *) a - *(const int16 *) b;
}

/*
 * Builds the rollup of one list over the unit. fixed: exactly the dims of ru (the cap of tuples is the largest one, an explicit rollup(...) or
 * the check of a stored one); else the dims go from the most distinct values down until the tuples are at most a 32nd of the elements, and
 * the blob fits room (limited: the budget of the list). false when no rollup fits or the list cannot be used.
 */
static bool
build_rollup(const SiftColSchema *s, const SiftCPaggRollup *ru, const SiftCPaggCol *cols, uint32 nrows, bool fixed, bool limited, Size room, StringInfo out)
{
	RuBuild		b;
	int			cand[SIFT_PAGG_MAX_RDIMS],
				nc = 0,
				nmeas = 0;
	uint32		dist[SIFT_PAGG_MAX_RDIMS],
				n = 0,
				hs = 16,
				mhs = 16;
	int64	   *hk;
	uint8	   *hu;
	bool		done = false;
	StringInfoData tmp;

	if (!col_usable(&cols[ru->list], nrows) || (limited && room == 0))
		return false;
	for (int j = 0; j < ru->ndims; j++)
	{
		const SiftCPaggCol *c = &cols[ru->dim[j]];

		if (!c->ready || has_ovf(c) || (nc > 0 && c->n != n))
			continue;
		n = c->n;
		cand[nc++] = j;
	}
	if (n == 0 || nc == 0)
		return false;
	b.n = n;
	b.cap = fixed ? SIFT_PAGG_ROLLUP_TUPLES : Min((uint32) SIFT_PAGG_ROLLUP_TUPLES, Max((uint32) SIFT_PAGG_ROLLUP_MIN, n / 32));
	while (mhs < 2 * (b.cap + 2))
		mhs *= 2;
	hk = palloc(mhs * sizeof(int64));
	hu = palloc(mhs);
	for (int i = 0; i < nc; i++)
		dist[i] = ru_distinct(&cols[ru->dim[cand[i]]], n, b.cap, hk, hu, mhs);
	pfree(hk);
	pfree(hu);
	for (int m = 0; m < ru->nmeas && nmeas < SIFT_PAGG_MAX_MEAS; m++)
	{
		const SiftCPaggCol *c = &cols[ru->meas[m]];

		if (c->ready && !has_ovf(c) && c->n == n)
		{
			b.mc[nmeas] = c;
			b.mleaf[nmeas] = ru->meas[m];
			b.mfl[nmeas++] = s->cols[ru->meas[m]].store == SIFT_COLS_F4 || s->cols[ru->meas[m]].store == SIFT_COLS_F8;
		}
	}
	b.nm = nmeas;
	/* the order of the attempts: the dims of fewer distinct values first (the position in ru breaks a tie); the stored order is by leaf */
	for (int i = 1; i < nc; i++)
		for (int j = i; j > 0 && dist[j] < dist[j - 1]; j--)
		{
			uint32		d = dist[j];
			int			c = cand[j];

			dist[j] = dist[j - 1];
			cand[j] = cand[j - 1];
			dist[j - 1] = d;
			cand[j - 1] = c;
		}
	{
		uint64		prod = 1;
		int			k = 0;

		if (fixed)
			k = nc;
		else
		{
			while (k < nc && dist[k] <= b.cap && prod * dist[k] <= (uint64) b.cap * 8)
				prod *= dist[k++];
			k = Max(k, 1);
		}
		while (hs < 2 * (b.cap + 1))
			hs *= 2;
		b.hsize = hs;
		b.gid = palloc(n * sizeof(uint32));
		b.cnt = palloc((b.cap + 1) * sizeof(uint32));
		b.tkey = palloc((Size) (b.cap + 1) * SIFT_PAGG_MAX_RDIMS * sizeof(int64));
		b.tnull = palloc(b.cap + 1);
		b.hslot = palloc(hs * sizeof(int32));
		initStringInfo(&tmp);
		for (; k >= 1 && !done; k--)
		{
			int16		leaves[SIFT_PAGG_MAX_RDIMS];

			if (!fixed && dist[k - 1] > b.cap)
				continue;
			for (int i = 0; i < k; i++)
				leaves[i] = ru->dim[cand[i]];
			qsort(leaves, k, sizeof(int16), cmp_i16);
			b.nd = k;
			for (int i = 0; i < k; i++)
			{
				b.dleaf[i] = leaves[i];
				b.dc[i] = &cols[leaves[i]];
			}
			if (ru_tuples(&b))
			{
				resetStringInfo(&tmp);
				ru_encode(&b, &tmp);
				if (!limited || tmp.len <= room)
				{
					appendBinaryStringInfo(out, tmp.data, tmp.len);
					done = true;
				}
			}
			if (fixed)
				break;
		}
	}
	pfree(b.gid);
	pfree(b.cnt);
	pfree(b.tkey);
	pfree(b.tnull);
	pfree(b.hslot);
	pfree(tmp.data);
	return done;
}

/* The element blob: the rollups of the spec's lists that a unit can hold, in ascending list order; limited: each within room bytes (a list's own budget, the cube's is not shared) */
static void
emit_rollups(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, bool limited, Size room, StringInfo out)
{
	StringInfoData body[SIFT_PAGG_MAX_ROLLUPS];
	int			list[SIFT_PAGG_MAX_ROLLUPS],
				nl = 0;

	for (int i = 0; i < spec->nrollups; i++)
	{
		initStringInfo(&body[nl]);
		if (build_rollup(s, &spec->rollup[i], cols, nrows, spec->rfixed, limited, room, &body[nl]))
		{
			list[nl++] = spec->rollup[i].list;
		}
		else
			pfree(body[nl].data);
	}
	if (nl == 0)
		return;
	appendStringInfoChar(out, (char) nl);
	for (int i = 0; i < nl; i++)
	{
		put_u(out, list[i]);
		put_u(out, body[i].len);
	}
	for (int i = 0; i < nl; i++)
	{
		appendBinaryStringInfo(out, body[i].data, body[i].len);
		pfree(body[i].data);
	}
}

void
sift_cpagg_cube(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, Size unit_bytes,
				StringInfo out, StringInfo eout)
{
	DimBuild	db[SIFT_PAGG_MAX_DIMS];
	Size		budget = 0,
				used = 0;

	if (spec->off || nrows == 0 || (spec->ndims == 0 && (eout == NULL || spec->nrollups == 0)))
		return;
	if (spec->permille > 0)
		budget = Max((Size) SIFT_PAGG_MIN_BUDGET, unit_bytes / 1000 * spec->permille);
	if (spec->ndims > 0)
	{
		build_dims(s, spec, cols, nrows, Min(spec->max_groups, SIFT_PAGG_CUBE_GROUPS), db);
		/* smallest dims first under the budget: the cheapest histograms answer the most questions per byte */
		for (int round = 0; round < spec->ndims; round++)
		{
			int			best = -1;

			for (int d = 0; d < spec->ndims; d++)
				if (db[d].ok && !db[d].keep && (best < 0 || db[d].bytes < db[best].bytes))
					best = d;
			if (best < 0 || (budget > 0 && used + db[best].bytes + 1 > budget))
				break;
			db[best].keep = true;
			used += db[best].bytes;
		}
		emit_cube(spec, db, spec->ndims, out);
		for (int d = 0; d < spec->ndims; d++)
			pfree(db[d].blob.data);
	}
	if (eout && spec->nrollups > 0)
		emit_rollups(s, spec, cols, nrows, !spec->rfixed, Max((Size) SIFT_PAGG_MIN_BUDGET, unit_bytes / 1000 * SIFT_PAGG_DEF_PERMILLE), eout);
}


/* ---------- reading ---------- */

/* One dim's body (cube blob format above, after the directory): false when it is not consistent with the schema */
static bool
decode_dim(const SiftColSchema *s, Rd *r, int leaf, SiftCHistDim *hd)
{
	uint32		ng,
				nk;

	memset(hd, 0, sizeof(*hd));
	hd->leaf = leaf;
	if (r->p + 2 > r->end)
		return false;
	hd->nmeas = *r->p++;
	ng = (uint32) get_u(r);
	if (r->p >= r->end || hd->nmeas > SIFT_PAGG_MAX_MEAS || ng == 0 || ng > SIFT_PAGG_CUBE_GROUPS)
		return false;
	hd->ngroups = ng;
	hd->has_null = (*r->p++ & PAGG_NULLGROUP) != 0;
	for (int m = 0; m < hd->nmeas; m++)
	{
		uint64		ml = get_u(r);

		if (ml >= (uint64) s->ncols)
			return false;
		hd->meas[m] = (int16) ml;
	}
	hd->key = palloc0(ng * sizeof(int64));
	hd->count = palloc(ng * sizeof(uint32));
	nk = ng - (hd->has_null ? 1 : 0);
	for (uint32 i = 0; i < nk; i++)
		hd->key[i] = i == 0 ? get_zz(r) : (int64) ((uint64) hd->key[i - 1] + get_u(r));
	for (uint32 i = 0; i < ng; i++)
		hd->count[i] = (uint32) get_u(r);
	for (int m = 0; m < hd->nmeas; m++)
	{
		const SiftColDef *md = &s->cols[hd->meas[m]];

		hd->nn[m] = palloc(ng * sizeof(uint32));
		for (uint32 i = 0; i < ng; i++)
			hd->nn[m][i] = (uint32) get_u(r);
		if (md->store == SIFT_COLS_F4 || md->store == SIFT_COLS_F8)
		{
			if ((Size) (r->end - r->p) < (Size) ng * sizeof(double))
				return false;
			hd->sumf[m] = palloc(ng * sizeof(double));
			memcpy(hd->sumf[m], r->p, ng * sizeof(double));
			r->p += ng * sizeof(double);
		}
		else
		{
			hd->sum[m] = palloc(ng * sizeof(SiftCI128));
			for (uint32 i = 0; i < ng; i++)
				hd->sum[m][i] = get_zz128(r);
		}
	}
	return !r->bad;
}

/*
 * The directory of a blob: leaf and body bytes of each entry (at most max of them; the dims of a cube, or the lists of an element blob); body
 * points at the first body. false when inconsistent
 */
static bool
read_dir(const SiftColSchema *s, const uint8 *blob, uint32 len, int max, int *nd, int *leaf, uint32 *blen, const uint8 **body)
{
	Rd			r = {blob, blob + len, false};
	uint64		total = 0;

	if (len == 0)
		return false;
	*nd = *r.p++;
	if (*nd == 0 || *nd > max)
		return false;
	for (int d = 0; d < *nd; d++)
	{
		uint64		l = get_u(&r),
					n = get_u(&r);

		if (r.bad || l >= (uint64) s->ncols || n == 0 || n > len)
			return false;
		leaf[d] = (int) l;
		blen[d] = (uint32) n;
		total += n;
	}
	*body = r.p;
	return (uint64) (r.end - r.p) == total;
}

bool
sift_cpagg_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCHist *h)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			nd,
				leaf[SIFT_PAGG_MAX_DIMS];
	uint32		blen[SIFT_PAGG_MAX_DIMS];
	const uint8 *p;
	bool		ok = false;

	memset(h, 0, sizeof(*h));
	if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, &nd, leaf, blen, &p))
		goto done;
	for (int d = 0; d < nd; d++)
	{
		Rd			r = {p, p + blen[d], false};

		if (!decode_dim(s, &r, leaf[d], &h->dim[d]) || r.p != r.end)
			goto done;
		p += blen[d];
	}
	h->ndims = nd;
	ok = true;
done:
	MemoryContextSwitchTo(old);
	return ok;
}

int
sift_cpagg_decode_dim(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, int leaf, SiftCHistDim *out)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			nd,
				lf[SIFT_PAGG_MAX_DIMS],
				res = -1;
	uint32		blen[SIFT_PAGG_MAX_DIMS];
	const uint8 *p;

	if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, &nd, lf, blen, &p))
		goto done;
	res = 0;
	for (int d = 0; d < nd; d++)
	{
		if (lf[d] == leaf)
		{
			Rd			r = {p, p + blen[d], false};

			res = decode_dim(s, &r, leaf, out) && r.p == r.end ? 1 : -1;
			break;
		}
		p += blen[d];
	}
done:
	MemoryContextSwitchTo(old);
	return res;
}

const uint8 *
sift_cpagg_elem_cube(const SiftColSchema *s, const uint8 *eblob, uint32 elen, int group, uint32 *len)
{
	int			nl,
				list[SIFT_PAGG_SPEC_DIMS];
	uint32		blen[SIFT_PAGG_SPEC_DIMS];
	const uint8 *p;

	*len = 0;
	if (!read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, &nl, list, blen, &p))
	{
		*len = UINT32_MAX;
		return NULL;
	}
	for (int i = 0; i < nl; p += blen[i++])
		if (list[i] == group)
		{
			*len = blen[i];
			return p;
		}
	return NULL;
}

const SiftCHistDim *
sift_cpagg_find(const SiftCHist *h, int leaf)
{
	for (int d = 0; d < h->ndims; d++)
		if (h->dim[d].leaf == leaf)
			return &h->dim[d];
	return NULL;
}

int
sift_cpagg_meas_slot(const SiftCHistDim *d, int m)
{
	for (int i = 0; i < d->nmeas; i++)
		if (d->meas[i] == m)
			return i;
	return -1;
}

/* ---------- the rollup, read ---------- */

bool
sift_cpagg_rollup_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCRollup *r)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	Rd			rd = {blob, blob + len, false};
	uint32		nt;
	bool		ok = false;

	memset(r, 0, sizeof(*r));
	r->list = -1;
	if (len < 3)
		goto done;
	r->ndims = *rd.p++;
	r->nmeas = *rd.p++;
	nt = (uint32) get_u(&rd);
	if (r->ndims == 0 || r->ndims > SIFT_PAGG_MAX_RDIMS || r->nmeas > SIFT_PAGG_MAX_MEAS || nt == 0 || nt > SIFT_PAGG_ROLLUP_TUPLES)
		goto done;
	r->ntuples = nt;
	for (int j = 0; j < r->ndims; j++)
	{
		uint64		l = get_u(&rd);

		if (rd.bad || l >= (uint64) s->ncols || s->cols[l].parent < 0 || (r->list >= 0 && r->list != s->cols[l].parent))
			goto done;
		r->dim[j] = (int16) l;
		r->list = s->cols[l].parent;
	}
	for (int m = 0; m < r->nmeas; m++)
	{
		uint64		l = get_u(&rd);

		if (rd.bad || l >= (uint64) s->ncols || s->cols[l].parent != r->list)
			goto done;
		r->meas[m] = (int16) l;
	}
	for (int j = 0; j < r->ndims; j++)
	{
		int64		prev = 0;

		r->key[j] = palloc0(nt * sizeof(int64));
		if (rd.p >= rd.end || *rd.p > 1)
			goto done;
		if (*rd.p++)
		{
			uint32		nb = (nt + 7) / 8;

			if ((uint32) (rd.end - rd.p) < nb)
				goto done;
			r->isnull[j] = palloc0(nt);
			for (uint32 t = 0; t < nt; t++)
				r->isnull[j][t] = rd.p[t / 8] >> (t % 8) & 1;
			rd.p += nb;
		}
		for (uint32 t = 0; t < nt; t++)
		{
			if (r->isnull[j] && r->isnull[j][t])
				continue;
			prev = (int64) ((uint64) prev + (uint64) get_zz(&rd));
			r->key[j][t] = prev;
		}
	}
	r->count = palloc(nt * sizeof(uint32));
	for (uint32 t = 0; t < nt; t++)
		r->count[t] = (uint32) get_u(&rd);
	for (int m = 0; m < r->nmeas; m++)
	{
		bool		fl = s->cols[r->meas[m]].store == SIFT_COLS_F4 || s->cols[r->meas[m]].store == SIFT_COLS_F8;

		r->nn[m] = palloc(nt * sizeof(uint32));
		for (uint32 t = 0; t < nt; t++)
			r->nn[m][t] = (uint32) get_u(&rd);
		if (fl)
		{
			if ((Size) (rd.end - rd.p) < (Size) nt * sizeof(double))
				goto done;
			r->sumf[m] = palloc(nt * sizeof(double));
			memcpy(r->sumf[m], rd.p, nt * sizeof(double));
			rd.p += nt * sizeof(double);
			r->minf[m] = palloc0(nt * sizeof(double));
			r->maxf[m] = palloc0(nt * sizeof(double));
		}
		else
		{
			r->sum[m] = palloc(nt * sizeof(SiftCI128));
			for (uint32 t = 0; t < nt; t++)
				r->sum[m][t] = get_zz128(&rd);
			r->min[m] = palloc0(nt * sizeof(int64));
			r->max[m] = palloc0(nt * sizeof(int64));
		}
		for (int pass = 0; pass < 2; pass++)
			for (uint32 t = 0; t < nt; t++)
			{
				if (r->nn[m][t] == 0)
					continue;
				if (fl)
				{
					double		v;

					if (rd.end - rd.p < (long) sizeof(double))
						goto done;
					memcpy(&v, rd.p, sizeof(double));
					rd.p += sizeof(double);
					(pass ? r->maxf[m] : r->minf[m])[t] = v;
				}
				else
					(pass ? r->max[m] : r->min[m])[t] = get_zz(&rd);
			}
	}
	ok = !rd.bad && rd.p == rd.end;
done:
	MemoryContextSwitchTo(old);
	return ok;
}

/* the order of the tuples by one dim, ties by position (so that the sums of a group add in the order of the tuples) */
static const SiftCRollup *proj_r;
static int	proj_j;

static int
cmp_proj(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;
	bool		nx = proj_r->isnull[proj_j] && proj_r->isnull[proj_j][x],
				ny = proj_r->isnull[proj_j] && proj_r->isnull[proj_j][y];

	if (nx != ny)
		return nx ? 1 : -1;
	if (!nx && proj_r->key[proj_j][x] != proj_r->key[proj_j][y])
		return proj_r->key[proj_j][x] < proj_r->key[proj_j][y] ? -1 : 1;
	return x < y ? -1 : x > y;
}

bool
sift_cpagg_rollup_dim(const SiftColSchema *s, const SiftCRollup *r, int leaf, MemoryContext mcxt, SiftCHistDim *out)
{
	MemoryContext old;
	int			j = -1;
	uint32	   *ord,
				ng = 0;
	bool		isnull_group;

	for (int i = 0; i < r->ndims; i++)
		if (r->dim[i] == leaf)
			j = i;
	if (j < 0)
		return false;
	old = MemoryContextSwitchTo(mcxt);
	memset(out, 0, sizeof(*out));
	out->leaf = leaf;
	out->nmeas = r->nmeas;
	ord = palloc(r->ntuples * sizeof(uint32));
	for (uint32 t = 0; t < r->ntuples; t++)
		ord[t] = t;
	proj_r = r;
	proj_j = j;
	qsort(ord, r->ntuples, sizeof(uint32), cmp_proj);
	out->key = palloc0(r->ntuples * sizeof(int64));
	out->count = palloc0(r->ntuples * sizeof(uint32));
	for (int m = 0; m < r->nmeas; m++)
	{
		out->meas[m] = r->meas[m];
		out->nn[m] = palloc0(r->ntuples * sizeof(uint32));
		if (r->sum[m])
			out->sum[m] = palloc0(r->ntuples * sizeof(SiftCI128));
		else
			out->sumf[m] = palloc0(r->ntuples * sizeof(double));
	}
	for (uint32 i = 0; i < r->ntuples; i++)
	{
		uint32		t = ord[i];
		bool		nul = r->isnull[j] && r->isnull[j][t];

		if (i == 0 || nul != (r->isnull[j] && r->isnull[j][ord[i - 1]]) || (!nul && r->key[j][t] != r->key[j][ord[i - 1]]))
			ng++;
		out->key[ng - 1] = r->key[j][t];
		out->count[ng - 1] += r->count[t];
		for (int m = 0; m < r->nmeas; m++)
		{
			if (r->nn[m][t] > 0 && out->nn[m][ng - 1] == 0 && out->sumf[m])
				out->sumf[m][ng - 1] = r->sumf[m][t];
			else if (out->sumf[m])
				out->sumf[m][ng - 1] += r->sumf[m][t];
			if (out->sum[m])
				out->sum[m][ng - 1] += r->sum[m][t];
			out->nn[m][ng - 1] += r->nn[m][t];
		}
	}
	isnull_group = ng > 0 && r->isnull[j] && r->isnull[j][ord[r->ntuples - 1]];
	out->ngroups = ng;
	out->has_null = isnull_group;
	pfree(ord);
	MemoryContextSwitchTo(old);
	(void) s;
	return true;
}

/* ---------- verification ---------- */

/* Marks the leaves the cube and the rollups read; false when a blob does not decode */
bool
sift_cpagg_leaves(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, bool *need)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg leaves", ALLOCSET_SMALL_SIZES);
	SiftCHist	h;
	bool		ok = true;

	if (len > 0 && sift_cpagg_decode(s, blob, len, cx, &h))
	{
		for (int d = 0; d < h.ndims; d++)
		{
			need[h.dim[d].leaf] = true;
			for (int m = 0; m < h.dim[d].nmeas; m++)
				need[h.dim[d].meas[m]] = true;
		}
	}
	else if (len > 0)
		ok = false;
	if (ok && elen > 0)
	{
		int			nl,
					list[SIFT_PAGG_MAX_ROLLUPS];
		uint32		blen[SIFT_PAGG_MAX_ROLLUPS];
		const uint8 *p;

		if (!read_dir(s, eblob, elen, SIFT_PAGG_MAX_ROLLUPS, &nl, list, blen, &p))
			ok = false;
		for (int i = 0; ok && i < nl; p += blen[i++])
		{
			SiftCRollup r;

			if (!sift_cpagg_rollup_decode(s, p, blen[i], cx, &r) || r.list != list[i])
			{
				ok = false;
				break;
			}
			need[r.list] = true;
			for (int j = 0; j < r.ndims; j++)
				need[r.dim[j]] = true;
			for (int m = 0; m < r.nmeas; m++)
				need[r.meas[m]] = true;
		}
	}
	MemoryContextDelete(cx);
	return ok;
}

static bool
same_bytes(const StringInfoData *a, const uint8 *b, uint32 len)
{
	return a->len == (int) len && (len == 0 || memcmp(a->data, b, len) == 0);
}

/* The spec that recomputes a stored cube and element blob: the dims, rollups and measures they hold, exactly */
static bool
spec_of(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, MemoryContext cx, SiftCPaggSpec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->fixed = spec->rfixed = true;
	spec->max_groups = SIFT_PAGG_CUBE_GROUPS;
	if (len > 0)
	{
		SiftCHist	h;

		if (!sift_cpagg_decode(s, blob, len, cx, &h))
			return false;
		for (int d = 0; d < h.ndims; d++, spec->ndims++)
		{
			spec->dim[spec->ndims].leaf = h.dim[d].leaf;
			spec->dim[spec->ndims].nmeas = h.dim[d].nmeas;
			memcpy(spec->dim[spec->ndims].meas, h.dim[d].meas, sizeof(spec->dim[0].meas));
		}
	}
	if (elen > 0)
	{
		int			nl,
					list[SIFT_PAGG_MAX_ROLLUPS];
		uint32		blen[SIFT_PAGG_MAX_ROLLUPS];
		const uint8 *p;

		if (!read_dir(s, eblob, elen, SIFT_PAGG_MAX_ROLLUPS, &nl, list, blen, &p))
			return false;
		for (int i = 0; i < nl; p += blen[i++])
		{
			SiftCRollup r;
			SiftCPaggRollup *ru = &spec->rollup[spec->nrollups++];

			if (!sift_cpagg_rollup_decode(s, p, blen[i], cx, &r))
				return false;
			*ru = (SiftCPaggRollup) {.list = r.list, .ndims = r.ndims, .nmeas = r.nmeas};
			memcpy(ru->dim, r.dim, sizeof(ru->dim));
			memcpy(ru->meas, r.meas, sizeof(ru->meas));
		}
	}
	return true;
}

bool
sift_cpagg_check(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen)
{
	SiftCPaggSpec spec;
	StringInfoData again,
				eagain;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg check", ALLOCSET_SMALL_SIZES);
	bool		ok = spec_of(s, blob, len, eblob, elen, cx, &spec);

	MemoryContextDelete(cx);
	if (!ok)
		return false;
	initStringInfo(&again);
	initStringInfo(&eagain);
	sift_cpagg_cube(s, &spec, cols, nrows, 0, &again, elen > 0 ? &eagain : NULL);
	ok = same_bytes(&again, blob, len) && same_bytes(&eagain, eblob, elen);
	pfree(again.data);
	pfree(eagain.data);
	return ok;
}
