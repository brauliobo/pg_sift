/*
 * col/scope_parse.c - the text of the option scope of a sift_col index (col/scope_decl.c) as specs, and the specs of an index kept for the text they were parsed from
 */
#include "postgres.h"

#include <ctype.h>

#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/scope.h"

/* [using name], when the text has it; p is after the closing parenthesis of the relation */
static const char *
parse_using(const char *p, char **name, const char **err)
{
	p = sift_conn_skip_ws(p);
	if (strncmp(p, "using", 5) != 0 || !isspace((unsigned char) p[5]))
		return p;
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 5), name, err)) == NULL)
		return NULL;
	return sift_conn_skip_ws(p);
}

/* [schema.]name(a, b) [using index] of one relation, whose columns have the roles ra and rb (the words of the errors) */
static const char *
parse_rel(const char *p, SiftScopeRel *r, const char *ra, const char *rb, const char **err)
{
	char	   *name;

	if ((p = sift_conn_read_name(sift_conn_skip_ws(p), &name, err)) == NULL)
		return NULL;
	if (*p == '.')
	{
		r->schema = name;
		if ((p = sift_conn_read_name(p + 1, &r->name, err)) == NULL)
			return NULL;
	}
	else
		r->name = name;
	p = sift_conn_skip_ws(p);
	if (*p != '(')
	{
		*err = psprintf("expected '(' and the %s and %s columns of the relation after its name", ra, rb);
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 1), &r->a, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (*p != ',')
	{
		*err = psprintf("expected ',' and the %s column after the %s column", rb, ra);
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 1), &r->b, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (*p != ')')
	{
		*err = psprintf("expected ')' after the %s column: a scope is a member column and a scope key column", rb);
		return NULL;
	}
	return parse_using(p + 1, &r->index_name, err);
}

/* One scope, from *pp to its end (a ';' or the end of the text); NULL with *err set when it does not parse */
static SiftScopeSpec *
parse_one(const char **pp, const char **err)
{
	SiftScopeSpec *s = palloc0(sizeof(SiftScopeSpec));
	const char *p = sift_conn_skip_ws(*pp);

	if ((p = sift_conn_read_name(p, &s->key_name, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (*p != ':')
	{
		*err = "expected ':' after the name of the derived key";
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 1), &s->fk, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (strncmp(p, "->", 2) != 0)
	{
		*err = "expected '->' after the column of this index that holds the member key";
		return NULL;
	}
	if ((p = parse_rel(p + 2, &s->rel, "member", "scope key", err)) == NULL)
		return NULL;
	if (*p == '|')
	{
		s->joined = palloc0(sizeof(SiftScopeRel));
		s->via = palloc0(sizeof(SiftScopeRel));
		if ((p = parse_rel(p + 1, s->joined, "via", "scope key", err)) == NULL)
			return NULL;
		if (strncmp(p, "via", 3) != 0 || !isspace((unsigned char) p[3]))
		{
			*err = "expected 'via' and the relation that pairs the via column with the member after the second path";
			return NULL;
		}
		if ((p = parse_rel(p + 3, s->via, "via", "member", err)) == NULL)
			return NULL;
	}
	if (strncmp(p, "cols", 4) == 0 && *sift_conn_skip_ws(p + 4) == '(')
	{
		p = sift_conn_skip_ws(p + 4) + 1;
		for (;;)
		{
			char	   *col;

			if ((p = sift_conn_read_name(sift_conn_skip_ws(p), &col, err)) == NULL)
				return NULL;
			s->cols = lappend(s->cols, col);
			p = sift_conn_skip_ws(p);
			if (*p == ',')
			{
				p++;
				continue;
			}
			if (*p++ != ')')
			{
				*err = "expected ',' or ')' in cols(..), the list of the columns the scope segments keep";
				return NULL;
			}
			break;
		}
		p = sift_conn_skip_ws(p);
	}
	*pp = p;
	return s;
}

SiftScopeSpecs *
sift_scope_parse(const char *text, const char **err)
{
	SiftScopeSpecs *all = palloc0(sizeof(SiftScopeSpecs));
	const char *p = text;

	for (;;)
	{
		SiftScopeSpec *s;

		if (all->n == SIFT_SCOPE_MAX)
		{
			*err = psprintf("too many scopes (%d at most in an index)", SIFT_SCOPE_MAX);
			return NULL;
		}
		if ((s = parse_one(&p, err)) == NULL)
			return NULL;
		for (int i = 0; i < all->n; i++)
			if (strcmp(all->s[i]->key_name, s->key_name) == 0)
			{
				*err = psprintf("the derived key \"%s\" is declared twice: each scope has a name of its own", s->key_name);
				return NULL;
			}
		all->s[all->n++] = s;
		if (*p == ';')
		{
			p++;
			continue;
		}
		if (*p != '\0')
		{
			*err = "unexpected text after the scope";
			return NULL;
		}
		return all;
	}
}

/*
 * The scopes of the index, or NULL: the option is not set, or 'off'. The parse is remembered with the text it was made of (a statement asks for it a dozen times): the text is the whole of what it depends
 * on, so a changed option parses again. The specs are read, never changed.
 */
#define SPECS_MEMO_N 8
static struct
{
	Oid			index;
	char	   *text;
	SiftScopeSpecs *specs;
	MemoryContext cx;
} specs_memo[SPECS_MEMO_N];
static int	specs_memo_next;

SiftScopeSpecs *
sift_scope_specs(Relation index)
{
	const char *text = sift_col_options_scope(index);
	const char *err = NULL;
	SiftScopeSpecs *s;
	MemoryContext old;
	int			slot;

	if (text[0] == '\0' || strcmp(text, "off") == 0)
		return NULL;
	for (int i = 0; i < SPECS_MEMO_N; i++)
		if (specs_memo[i].index == RelationGetRelid(index) && strcmp(specs_memo[i].text, text) == 0)
			return specs_memo[i].specs;
	slot = specs_memo_next++ % SPECS_MEMO_N;
	if (specs_memo[slot].cx != NULL)
		MemoryContextDelete(specs_memo[slot].cx);
	specs_memo[slot] = (typeof(specs_memo[0])) {0};
	specs_memo[slot].cx = AllocSetContextCreate(TopMemoryContext, "sift cache scope specs", ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(specs_memo[slot].cx);
	s = sift_scope_parse(text, &err);
	if (s == NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid scope option \"%s\": %s", text, err)));
	specs_memo[slot].text = pstrdup(text);
	MemoryContextSwitchTo(old);
	specs_memo[slot].index = RelationGetRelid(index);
	specs_memo[slot].specs = s;
	return s;
}
