/*
 * agg/spec_lits.c - the literals a LIKE, ILIKE or regular expression requires of a string: every string that matches holds them, so a string without one of them is
 * known not to match (the executor tests the others only, agg/pred_in.c)
 */
#include "postgres.h"

#include <ctype.h>

#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/spec.h"

#define LITS_MIN	3			/* a shorter literal leaves too many strings to pay for its pass */

typedef struct Lits
{
	SiftPatLits *out;
	char	   *run;			/* the literal being read */
	uint32		runlen;
	int32		last;			/* where its last character starts, -1 when the last element of the pattern is not one of them (a quantifier then has no character to drop) */
	bool		fold;
} Lits;

/* keeps a literal among the longest SIFT_PAT_LITS, longest first */
static void
lits_add(SiftPatLits *o, const char *lit, uint32 len, bool fold)
{
	int			at = o->n;

	while (at > 0 && o->len[at - 1] < len)
		at--;
	if (at >= SIFT_PAT_LITS)
		return;
	for (int i = Min(o->n, SIFT_PAT_LITS - 1); i > at; i--)
	{
		o->lit[i] = o->lit[i - 1];
		o->len[i] = o->len[i - 1];
		o->fold[i] = o->fold[i - 1];
	}
	o->lit[at] = lit;
	o->len[at] = len;
	o->fold[at] = fold;
	o->n = Min(o->n + 1, SIFT_PAT_LITS);
}

/* ends the literal being read: kept when it is long enough */
static void
lits_flush(Lits *l)
{
	if (l->runlen >= LITS_MIN)
		lits_add(l->out, pnstrdup(l->run, l->runlen), l->runlen, l->fold);
	l->runlen = 0;
	l->last = -1;
}

/* adds the character at s (n bytes) to the literal; a folded one reads ASCII only (the lower case of any other character is not), and ends the literal */
static void
lits_char(Lits *l, const char *s, int n)
{
	if (l->fold && (n > 1 || (uint8) s[0] >= 0x80))
	{
		lits_flush(l);
		return;
	}
	l->last = l->runlen;
	for (int i = 0; i < n; i++)
		l->run[l->runlen++] = l->fold && s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 0x20 : s[i];
}

/* the end of the bracket expression that opens at re[i] (the index of its ']'), or -1 */
static int
re_bracket(const char *re, int n, int i)
{
	int			j = i + 1;

	if (j < n && re[j] == '^')
		j++;
	if (j < n && re[j] == ']')
		j++;
	while (j < n)
	{
		if (re[j] == '[' && j + 1 < n && strchr(".:=", re[j + 1]) != NULL)
		{
			int			k = j + 2;

			while (k + 1 < n && !(re[k] == re[j + 1] && re[k + 1] == ']'))
				k++;
			if (k + 1 >= n)
				return -1;
			j = k + 2;
		}
		else if (re[j] == '\\')
			j += 2;
		else if (re[j] == ']')
			return j;
		else
			j++;
	}
	return -1;
}

/* the ')' that closes the group that opens at re[i], or -1 */
static int
re_group(const char *re, int n, int i)
{
	int			depth = 0;

	for (int j = i; j < n;)
	{
		if (re[j] == '\\')
			j += 2;
		else if (re[j] == '[')
		{
			if ((j = re_bracket(re, n, j)) < 0)
				return -1;
			j++;
		}
		else
		{
			depth += re[j] == '(' ? 1 : re[j] == ')' ? -1 : 0;
			if (depth == 0)
				return j;
			j++;
		}
	}
	return -1;
}

/*
 * The literals of an advanced regular expression, the top level runs of ordinary characters: a quantifier takes the character before it out of the run (a '+' or a
 * bound of 1 or more keeps it), a group, a bracket expression, '.', an anchor or a class escape ends the run, and the construct is not looked into. A pattern this
 * does not know how to read (a top level '|', an embedded option or director, an escape of a hexadecimal or octal character or a back reference) has none.
 */
static bool
re_lits(Lits *l, const char *re, int n)
{
	if (n >= 3 && memcmp(re, "***", 3) == 0)
		return false;
	for (int i = 0; i < n;)
	{
		char		c = re[i];
		int			k;

		switch (c)
		{
			case '\\':
				if (i + 1 >= n)
					return false;
				c = re[i + 1];
				if (strchr("dDwWsSbBmMyYAZaefnrtv", c) != NULL)
					lits_flush(l);
				else if (!isalnum((unsigned char) c) && (uint8) c < 0x80)
					lits_char(l, &re[i + 1], 1);
				else
					return false;
				i += 2;
				break;
			case '.':
			case '^':
			case '$':
				lits_flush(l);
				i++;
				break;
			case '[':
				if ((k = re_bracket(re, n, i)) < 0)
					return false;
				lits_flush(l);
				i = k + 1;
				break;
			case '(':
				if (i + 1 < n && re[i + 1] == '?' && !(i + 2 < n && re[i + 2] == ':'))
					return false;
				if ((k = re_group(re, n, i)) < 0)
					return false;
				lits_flush(l);
				i = k + 1;
				break;
			case ')':
			case '|':
				return false;
			case '*':
			case '?':
				if (l->last >= 0)
					l->runlen = l->last;
				lits_flush(l);
				i++;
				break;
			case '+':
				lits_flush(l);
				i++;
				break;
			case '{':
				{
					int			m = 0,
								j = i + 1;

					if (j >= n || !isdigit((unsigned char) re[j]))
						return false;
					for (; j < n && isdigit((unsigned char) re[j]); j++)
						m = Min(m * 10 + (re[j] - '0'), 255);
					if (j < n && re[j] == ',')
						for (j++; j < n && isdigit((unsigned char) re[j]); j++)
							;
					if (j >= n || re[j] != '}')
						return false;
					if (m == 0 && l->last >= 0)
						l->runlen = l->last;
					lits_flush(l);
					i = j + 1;
				}
				break;
			default:
				k = pg_mblen(&re[i]);
				if (i + k > n)
					return false;
				lits_char(l, &re[i], k);
				i += k;
				break;
		}
	}
	lits_flush(l);
	return true;
}

/* the runs between % and _ of a LIKE pattern (a backslash escape is not read) */
static bool
like_lits(Lits *l, const char *pat, int n)
{
	for (int i = 0; i < n;)
	{
		int			k;

		if (pat[i] == '\\')
			return false;
		if (pat[i] == '%' || pat[i] == '_')
		{
			lits_flush(l);
			i++;
			continue;
		}
		k = pg_mblen(&pat[i]);
		if (i + k > n)
			return false;
		lits_char(l, &pat[i], k);
		i += k;
	}
	lits_flush(l);
	return true;
}

/* which of the pattern functions, and how: *icase for the ones that fold, *neg for the negated ones; 0 for none, 1 LIKE, 2 regular expression */
static int
pattern_kind(Oid funcid, bool *icase, bool *neg)
{
	*icase = funcid == F_TEXTICLIKE || funcid == F_TEXTICNLIKE || funcid == F_TEXTICREGEXEQ || funcid == F_TEXTICREGEXNE;
	*neg = funcid == F_TEXTNLIKE || funcid == F_TEXTICNLIKE || funcid == F_TEXTREGEXNE || funcid == F_TEXTICREGEXNE;
	switch (funcid)
	{
		case F_TEXTLIKE:
		case F_TEXTNLIKE:
		case F_TEXTICLIKE:
		case F_TEXTICNLIKE:
			return 1;
		case F_TEXTREGEXEQ:
		case F_TEXTREGEXNE:
		case F_TEXTICREGEXEQ:
		case F_TEXTICREGEXNE:
			return 2;
	}
	return 0;
}

bool
sift_pattern_lits_of(Oid funcid, Oid collid, const char *pat, uint32 len, SiftPatLits *out)
{
	bool		icase,
				neg;
	int			kind = pattern_kind(funcid, &icase, &neg);
	Lits		l = {.out = out,.last = -1,.fold = icase};

	out->n = 0;
	out->neg = neg;
	if (kind == 0 || !get_collation_isdeterministic(collid) || (GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() != 1) ||
		(icase && !sift_aggspec_ascii_fold(collid)))
		return false;
	l.run = palloc(len + 1);
	if (!(kind == 1 ? like_lits(&l, pat, len) : re_lits(&l, pat, len)))
		out->n = 0;
	return out->n > 0;
}

static bool
call_lits(Node *e, SiftPatLits *out)
{
	Oid			funcid,
				collid;
	List	   *args;
	Node	   *a,
			   *b;
	Const	   *k;
	text	   *t;

	if (IsA(e, OpExpr))
	{
		set_opfuncid((OpExpr *) e);
		funcid = ((OpExpr *) e)->opfuncid;
		collid = ((OpExpr *) e)->inputcollid;
		args = ((OpExpr *) e)->args;
	}
	else if (IsA(e, FuncExpr))
	{
		funcid = ((FuncExpr *) e)->funcid;
		collid = ((FuncExpr *) e)->inputcollid;
		args = ((FuncExpr *) e)->args;
	}
	else
		return false;
	if (list_length(args) != 2)
		return false;
	a = (Node *) linitial(args);
	b = (Node *) lsecond(args);
	while (IsA(a, RelabelType))
		a = (Node *) ((RelabelType *) a)->arg;
	while (IsA(b, RelabelType))
		b = (Node *) ((RelabelType *) b)->arg;
	k = (Const *) b;
	if (!IsA(a, Param) || !IsA(k, Const) || k->constisnull || (k->consttype != TEXTOID && k->consttype != VARCHAROID))
		return false;
	t = DatumGetTextPP(k->constvalue);
	return sift_pattern_lits_of(funcid, collid, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), out);
}

/* the expression of a lookup table over the dictionary: one pattern test, its NOT, or an AND of tests that are none of them negated */
bool
sift_pattern_lits(Node *e, SiftPatLits *out)
{
	SiftPatLits one;

	out->n = 0;
	out->neg = false;
	if (e == NULL)
		return false;
	if (IsA(e, BoolExpr) && ((BoolExpr *) e)->boolop == NOT_EXPR)
	{
		if (!sift_pattern_lits((Node *) linitial(((BoolExpr *) e)->args), out) || out->neg)
			return false;
		out->neg = true;
		return true;
	}
	if (IsA(e, BoolExpr) && ((BoolExpr *) e)->boolop == AND_EXPR)
	{
		ListCell   *lc;

		foreach(lc, ((BoolExpr *) e)->args)
		{
			if (sift_pattern_lits((Node *) lfirst(lc), &one) && !one.neg)
				for (int i = 0; i < one.n; i++)
					lits_add(out, one.lit[i], one.len[i], one.fold[i]);
		}
		return out->n > 0;
	}
	return call_lits(e, out);
}
