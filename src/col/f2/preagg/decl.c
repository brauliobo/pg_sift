/*
 * col/f2/preagg/decl.c - the option preagg of a Format 2.0 relation: the cubes the user declared (col/f2/preagg/v2.h).
 *
 *   option := default | auto | off | auto: column, ... | clause { (',' | ';') clause }
 *   clause := cube( entry { ';' entry } ) | rollup( ... ) | calendar( ... )
 *   entry  := dimension [ ':' measure { ',' measure } ]   (a bare dimension may be followed by ',' and the next entry)
 *
 * Names are those of the columns of the relation, double quoted when they need it. A rollup names leaves of the elements of a list (items[].kind), its entries `dimension, ... [: measure, ...]`
 * separated by ';': the cubes of the elements of the lists it names are the declared ones. A calendar names date buckets that a part of Format 2.0 keeps no cells of: it is read for its syntax
 * and left at that.
 */
#include "postgres.h"

#include <ctype.h>

#include "col/f2/f2.h"
#include "col/f2/preagg/v2.h"

typedef struct Cur
{
	const char *p;
} Cur;

static void
skip_ws(Cur *c)
{
	while (isspace((unsigned char) *c->p))
		c->p++;
}

static bool
eat(Cur *c, char ch)
{
	skip_ws(c);
	if (*c->p != ch)
		return false;
	c->p++;
	return true;
}

/* The keyword at the cursor (any case), not the start of a longer name */
static bool
word(Cur *c, const char *w)
{
	const int	n = (int) strlen(w);

	skip_ws(c);
	if (pg_strncasecmp(c->p, w, n) != 0 || isalnum((unsigned char) c->p[n]) || c->p[n] == '_')
		return false;
	c->p += n;
	return true;
}

static bool
only(const char *p, const char *w)
{
	Cur			c = {p};

	return word(&c, w) && (skip_ws(&c), *c.p == '\0');
}

/* A name up to ":,;)", blanks around it taken off; "a name in double quotes" is taken as written ("" is a quote); empty at the end of the text */
static char *
take_name(Cur *c, const char **bad)
{
	StringInfoData name;

	skip_ws(c);
	initStringInfo(&name);
	if (*c->p == '"')
	{
		for (c->p++; *c->p != '"' || c->p[1] == '"'; c->p++)
		{
			if (*c->p == '\0')
			{
				*bad = "a quoted column name is not closed";
				return name.data;
			}
			appendStringInfoChar(&name, *c->p);
			c->p += *c->p == '"';
		}
		c->p++;
		skip_ws(c);
		return name.data;
	}
	while (*c->p != '\0' && strchr(":,;)", *c->p) == NULL)
		appendStringInfoChar(&name, *c->p++);
	while (name.len > 0 && isspace((unsigned char) name.data[name.len - 1]))
		name.data[--name.len] = '\0';
	return name.data;
}

/* The top level leaf that holds the column of this name; -1: none */
static int
find_leaf(const SiftColSchema *s, const char *name)
{
	for (int i = 0; i < s->ncols; i++)
	{
		const SiftColDef *d = &s->cols[i];

		if (d->parent < 0 && d->lane == 0 && d->kind == SIFT_COLK_SCALAR && (d->path == NULL || d->path[0] == '\0') && d->name != NULL && strcmp(d->name, name) == 0)
			return i;
	}
	return -1;
}

/* The leaf of an element of a list whose path is this name; -1: none */
static int
find_elem_leaf(const SiftColSchema *s, const char *name)
{
	for (int i = 0; i < s->ncols; i++)
	{
		const SiftColDef *d = &s->cols[i];

		if (d->parent >= 0 && d->lane == 0 && d->kind == SIFT_COLK_SCALAR && d->path != NULL && strcmp(d->path, name) == 0)
			return i;
	}
	return -1;
}

static bool
listed(const int *v, int n, int leaf)
{
	for (int i = 0; i < n; i++)
		if (v[i] == leaf)
			return true;
	return false;
}

/* A column of an entry: what its leaf (of the rows, or of the elements of a list) is added to (the dimensions or the measures); NULL, or why not */
static const char *
add_leaf(const SiftColSchema *s, bool strict, bool elem, const char *name, int *v, int *n, int max, const char *what)
{
	int			leaf;

	if (*name == '\0')
		return psprintf("a cube %s name is missing", what);
	if (s == NULL)
		return NULL;
	leaf = elem ? find_elem_leaf(s, name) : find_leaf(s, name);
	if (leaf < 0)
		return strict ? psprintf("cube %s \"%s\" is not a column of the relation", what, name) : NULL;
	if (listed(v, *n, leaf))
		return NULL;
	if (*n == max)
		return psprintf("a cube holds at most %d %ss", max, what);
	v[(*n)++] = leaf;
	return NULL;
}

/* "cube(" was read: the entries up to ")" */
static const char *
cube_clause(Cur *c, const SiftColSchema *s, bool strict, SiftPreaggDecl *decl)
{
	const char *bad = NULL;
	bool		more;

	do
	{
		char	   *dim = take_name(c, &bad);
		bool		measures = false;

		if (bad != NULL || (bad = add_leaf(s, strict, false, dim, decl->dim, &decl->ndims, SIFT_PREAGG_MAX_DIMS, "dimension")) != NULL)
			return bad;
		for (bool again = eat(c, ':'); again; again = eat(c, ','))
		{
			char	   *m = take_name(c, &bad);

			measures = true;
			if (bad != NULL || (bad = add_leaf(s, strict, false, m, decl->meas, &decl->nmeas, SIFT_PREAGG_MAX_MEAS, "measure")) != NULL)
				return bad;
		}
		more = eat(c, ';') || (!measures && eat(c, ','));
	} while (more);
	return eat(c, ')') ? NULL : "expected ) at the end of cube(...)";
}

/* "rollup(" was read: the entries up to ")", each dimensions of the elements of one list and their measures; a name that is no leaf of the elements of a list declares nothing */
static const char *
rollup_clause(Cur *c, const SiftColSchema *s, SiftPreaggDecl *decl)
{
	const char *bad = NULL;

	do
	{
		do
		{
			char	   *dim = take_name(c, &bad);

			if (bad != NULL || (bad = add_leaf(s, false, true, dim, decl->rdim, &decl->nrdims, SIFT_PREAGG_MAX_RLEAVES, "dimension")) != NULL)
				return bad;
		} while (eat(c, ','));
		for (bool again = eat(c, ':'); again; again = eat(c, ','))
		{
			char	   *m = take_name(c, &bad);

			if (bad != NULL || (bad = add_leaf(s, false, true, m, decl->rmeas, &decl->nrmeas, SIFT_PREAGG_MAX_RLEAVES, "measure")) != NULL)
				return bad;
		}
	} while (eat(c, ';'));
	return eat(c, ')') ? NULL : "expected ) at the end of rollup(...)";
}

/* A clause whose cells a part of Format 2.0 does not keep: up to its closing parenthesis, the names in double quotes left alone */
static const char *
skip_clause(Cur *c, const char *what)
{
	if (!eat(c, '('))
		return psprintf("expected ( after %s", what);
	for (bool quoted = false; *c->p != '\0'; c->p++)
	{
		quoted ^= *c->p == '"';
		if (*c->p == ')' && !quoted)
		{
			c->p++;
			return NULL;
		}
	}
	return psprintf("expected ) at the end of %s(...)", what);
}

const char *
sift_preagg_decl_parse(const char *opt, const SiftColSchema *schema, bool strict, SiftPreaggDecl *decl, bool *off)
{
	Cur			c = {opt != NULL ? opt : ""};

	*decl = (SiftPreaggDecl) {0};
	*off = false;
	skip_ws(&c);
	if (*c.p == '\0' || only(c.p, "default") || only(c.p, "auto") || (word(&c, "auto") && eat(&c, ':')))
		return NULL;
	c.p = opt;
	if (only(c.p, "off"))
	{
		*off = true;
		return NULL;
	}
	do
	{
		const char *bad;

		if (word(&c, "cube"))
			bad = eat(&c, '(') ? cube_clause(&c, schema, strict, decl) : "expected ( after cube";
		else if (word(&c, "rollup"))
			bad = eat(&c, '(') ? rollup_clause(&c, schema, decl) : "expected ( after rollup";
		else if (word(&c, "calendar"))
			bad = skip_clause(&c, "calendar");
		else
			bad = psprintf("expected default, auto, off, cube(...), rollup(...) or calendar(...), got \"%s\"", c.p);
		if (bad != NULL)
			return bad;
		if (!eat(&c, ','))
			(void) eat(&c, ';');
		skip_ws(&c);
	} while (*c.p != '\0');
	return NULL;
}

bool
sift_preagg_cube_declared(const SiftPreaggDecl *decl, const SiftPreaggCubeDesc *cd)
{
	const uint8 kind = cd->flags & SIFT_PCUBE_KIND;

	if (decl == NULL || (kind != 0 && kind != SIFT_PCUBE_KIND))
		return false;
	if (kind == 0)
		return decl->ndims > 0;
	for (int d = 0; d < cd->ndims; d++)
		if (!listed(decl->rdim, decl->nrdims, cd->dim_leaf[d]))
			return false;
	return cd->ndims > 0;
}
