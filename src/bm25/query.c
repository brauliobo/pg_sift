/*
 * bm25/query.c - SiftQL parser, analysis/normalization, canonical deparse
 * and document evaluation.
 */
#include "core/host.h"

#include <ctype.h>

#include "bm25/query.h"
#include "bm25/query_int.h"
#include "core/limits.h"

#define WORD_SPECIALS		"(){}\"^~/\\*?:"

/* ---------------------------------------------------------------- lexer */

typedef enum QTokKind
{
	T_EOF, T_WORD, T_PHRASE, T_REGEX, T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE,
	T_CARET, T_AND, T_OR, T_NOT, T_FIELD
} QTokKind;

static const char *const tok_names[] = {
	"end of query", "word", "phrase", "regular expression", "\"(\"", "\")\"",
	"\"{\"", "\"}\"", "\"^\"", "AND", "OR", "NOT", "field"
};

typedef struct QTok
{
	QTokKind	kind;
	int			loc;
	bool		spaced;			/* whitespace before the token */
	SiftQKind	leaf;			/* T_WORD: SQ_TERM, SQ_PREFIX, SQ_WILDCARD or SQ_FUZZY */
	int			fuzzy;
	char	   *text;
	int			textlen;
	char	   *field;			/* name: before a word, or alone before a group (T_FIELD) */
} QTok;

typedef struct QParser
{
	const char *src;
	int			len;
	int			pos;			/* byte offset */
	int			cpos;			/* 1-based character position of pos */
	int			nleaves;
	QTok		tok;
} QParser;

static inline bool
is_space(unsigned char c)
{
	return c <= ' ' || c == 0x7F;
}

static inline bool
is_special(unsigned char c)
{
	return c != '\0' && strchr("(){}\"^~/", c) != NULL;
}

/* Characters that may directly follow "~n" or "^n" */
static inline bool
ends_suffix(unsigned char c)
{
	return c == '\0' || is_space(c) || strchr("(){}\"/", c) != NULL;
}

static inline unsigned char
cur(const QParser *p)
{
	return p->pos < p->len ? (unsigned char) p->src[p->pos] : '\0';
}

static inline bool
at_end(const QParser *p)
{
	return p->pos >= p->len;
}

/* Moves past the current character, appending it to buf if given */
static void
advance(QParser *p, StringInfo buf)
{
	int			l = char_len(p->src + p->pos, p->src + p->len);

	if (buf)
		appendBinaryStringInfo(buf, p->src + p->pos, l);
	p->pos += l;
	p->cpos++;
}

/*
 * "phrase" or /regex/. A backslash escapes any character; regex text keeps
 * its escapes except "\/", which becomes "/".
 */
static void
lex_quoted(QParser *p, QTok *t, char close, bool keep_escapes)
{
	StringInfoData buf;

	initStringInfo(&buf);
	advance(p, NULL);
	while (at_end(p) || cur(p) != close)
	{
		if (at_end(p))
			SYNTAX(t->loc, close == '"' ? "unterminated phrase" : "unterminated regular expression");
		if (cur(p) == '\\')
		{
			advance(p, NULL);
			if (at_end(p))
				continue;
			if (keep_escapes && cur(p) != close)
				appendStringInfoChar(&buf, '\\');
		}
		advance(p, &buf);
	}
	advance(p, NULL);
	t->text = buf.data;
	t->textlen = buf.len;
}

/*
 * Rewrites every run of unescaped '*' and '?' as "?..?*" so equivalent
 * patterns share one spelling. Returns the number of metas left; *trailing
 * tells whether the pattern ends with an unescaped '*'.
 */
int
sift_query_canon_pattern(StringInfo pat, bool *trailing)
{
	StringInfoData out;
	int			nmeta = 0;

	initStringInfo(&out);
	*trailing = false;
	for (int i = 0; i < pat->len;)
	{
		int			nq = 0;
		bool		star = false;

		*trailing = false;
		if (pat->data[i] == '\\')
		{
			appendBinaryStringInfo(&out, pat->data + i, 2);
			i += 2;
			continue;
		}
		if (pat->data[i] != '*' && pat->data[i] != '?')
		{
			appendStringInfoChar(&out, pat->data[i++]);
			continue;
		}
		for (; i < pat->len && (pat->data[i] == '*' || pat->data[i] == '?'); i++)
			pat->data[i] == '?' ? nq++ : (star = true);
		for (int k = 0; k < nq; k++)
			appendStringInfoChar(&out, '?');
		if (star)
			appendStringInfoChar(&out, '*');
		nmeta += nq + star;
		*trailing = star;
	}
	*pat = out;
	return nmeta;
}

/*
 * Does the ':' under the cursor end a field name (lowercase identifier) that
 * qualifies what follows: a word character, or a group opened right after it?
 * name:"x" and name:/x/ stay words, as before fields existed.
 */
static bool
field_follows(const QParser *p, const char *name, int len)
{
	unsigned char next = p->pos + 1 < p->len ? (unsigned char) p->src[p->pos + 1] : '\0';

	if (len == 0 || !(islower((unsigned char) name[0]) || name[0] == '_'))
		return false;
	for (int i = 1; i < len; i++)
		if (!(islower((unsigned char) name[i]) || isdigit((unsigned char) name[i]) || name[i] == '_'))
			return false;
	return next == '(' || next == '{' || (!is_space(next) && !is_special(next) && next != '\0');
}

static void
lex_word(QParser *p, QTok *t)
{
	StringInfoData lit,
				pat;
	bool		escaped = false;
	bool		meta = false;
	int			nlit = 0;

	initStringInfo(&lit);
	initStringInfo(&pat);
	while (!at_end(p) && !is_space(cur(p)) && !is_special(cur(p)))
	{
		if (cur(p) == ':' && !meta && !escaped && t->field == NULL && field_follows(p, lit.data, lit.len))
		{
			advance(p, NULL);
			t->field = pstrdup(lit.data);
			if (cur(p) == '(' || cur(p) == '{')
			{
				t->kind = T_FIELD;
				return;
			}
			resetStringInfo(&lit);
			resetStringInfo(&pat);
			nlit = 0;
			continue;
		}
		if (cur(p) == '*' || cur(p) == '?')
		{
			meta = true;
			advance(p, &pat);
			continue;
		}
		if (cur(p) == '\\')
		{
			int			at = p->cpos;

			advance(p, NULL);
			if (at_end(p))
				SYNTAX(at, "escape character at end of query");
			escaped = true;
			if (strchr("*?\\", cur(p)))
				appendStringInfoChar(&pat, '\\');
		}
		appendBinaryStringInfo(&pat, p->src + p->pos, char_len(p->src + p->pos, p->src + p->len));
		advance(p, &lit);
		nlit++;
	}

	t->kind = T_WORD;
	t->leaf = SQ_TERM;
	t->text = lit.data;
	t->textlen = lit.len;
	if (meta)
	{
		bool		trailing;

		if (nlit == 0)
			SYNTAX(t->loc, "wildcard pattern needs a literal character");
		if (sift_query_canon_pattern(&pat, &trailing) == 1 && trailing)
			t->leaf = SQ_PREFIX;
		else
		{
			t->leaf = SQ_WILDCARD;
			t->text = pat.data;
			t->textlen = pat.len;
		}
	}

	if (cur(p) == '~')
	{
		int			at = p->cpos;

		if (meta)
			SYNTAX(at, "fuzzy matching does not apply to wildcard patterns");
		advance(p, NULL);
		t->leaf = SQ_FUZZY;
		t->fuzzy = 1;
		if (isdigit(cur(p)))
		{
			t->fuzzy = cur(p) - '0';
			advance(p, NULL);
			if (t->fuzzy < 1 || t->fuzzy > 2 || isdigit(cur(p)))
				SYNTAX(at, "fuzzy distance must be 1 or 2");
		}
		if (!ends_suffix(cur(p)) && cur(p) != '^')
			SYNTAX(p->cpos, "unexpected character after \"~\"");
	}
	else if (!escaped && !meta)
	{
		if (strcmp(lit.data, "AND") == 0)
			t->kind = T_AND;
		else if (strcmp(lit.data, "OR") == 0)
			t->kind = T_OR;
		else if (strcmp(lit.data, "NOT") == 0)
			t->kind = T_NOT;
	}
}

static void
next_token(QParser *p)
{
	QTok	   *t = &p->tok;
	int			start = p->pos;
	unsigned char c;

	while (!at_end(p) && is_space(cur(p)))
		advance(p, NULL);
	memset(t, 0, sizeof(*t));
	t->spaced = p->pos > start;
	t->loc = p->cpos;
	c = cur(p);
	if (at_end(p))
		t->kind = T_EOF;
	else if (c == '"' || c == '/')
	{
		t->kind = c == '"' ? T_PHRASE : T_REGEX;
		lex_quoted(p, t, c, c == '/');
		if (c == '/' && t->textlen == 0)
			SYNTAX(t->loc, "empty regular expression");
		if (c == '/' && t->textlen > SIFT_MAX_REGEX_BYTES)
			LIMIT(t->loc, "regular expression longer than %d bytes", SIFT_MAX_REGEX_BYTES);
	}
	else if (c == '~')
		SYNTAX(t->loc, "\"~\" must directly follow a word");
	else if (is_special(c))
	{
		t->kind = c == '(' ? T_LPAREN : c == ')' ? T_RPAREN : c == '{' ? T_LBRACE :
			c == '}' ? T_RBRACE : T_CARET;
		advance(p, NULL);
	}
	else
		lex_word(p, t);
}

/* Boost right after '^': digits [. digits] [e [+-] digits] */
static float4
lex_boost(QParser *p)
{
	int			at = p->cpos,
				start = p->pos,
				ndig = 0;
	float4		f;

	for (; isdigit(cur(p)); ndig++)
		advance(p, NULL);
	if (cur(p) == '.')
		for (advance(p, NULL); isdigit(cur(p)); ndig++)
			advance(p, NULL);
	if (ndig > 0 && (cur(p) == 'e' || cur(p) == 'E'))
	{
		advance(p, NULL);
		if (cur(p) == '+' || cur(p) == '-')
			advance(p, NULL);
		if (!isdigit(cur(p)))
			ndig = 0;
		while (isdigit(cur(p)))
			advance(p, NULL);
	}
	if (ndig == 0 || p->pos - start > 64 || !ends_suffix(cur(p)))
		SYNTAX(at, "boost must be a non-negative number");
	f = strtof(pnstrdup(p->src + start, p->pos - start), NULL);
	if (!(f <= SIFT_MAX_BOOST))
		LIMIT(at, "boost must be between 0 and %g", SIFT_MAX_BOOST);
	return f;
}

/* ---------------------------------------------------------------- parser */

SiftQNode *
sift_query_new_node(SiftQKind kind, int loc)
{
	SiftQNode  *n = palloc0(sizeof(SiftQNode));

	n->kind = kind;
	n->boost = 1.0f;
	n->loc = loc;
	n->leafno = -1;
	return n;
}

SiftQNode *
sift_query_new_not(SiftQNode *child, int loc)
{
	SiftQNode  *n = sift_query_new_node(SQ_NOT, loc);

	n->nchild = 1;
	n->child = palloc(sizeof(SiftQNode *));
	n->child[0] = child;
	return n;
}

/* A one-element list yields its element */
static SiftQNode *
list_node(SiftQKind kind, List *kids)
{
	SiftQNode  *n;
	int			i = 0;

	if (list_length(kids) == 1)
		return linitial(kids);
	n = sift_query_new_node(kind, ((SiftQNode *) linitial(kids))->loc);
	n->nchild = list_length(kids);
	n->child = palloc(n->nchild * sizeof(SiftQNode *));
	foreach_ptr(SiftQNode, c, kids)
		n->child[i++] = c;
	return n;
}

/* Splices unboosted groups of the same operator: (a b) c == a b c; a name:(a b) hands its qualifier to the members first */
List *
sift_query_add_child(List *kids, SiftQNode *c, SiftQKind kind)
{
	if (c->kind != kind || c->boost != 1.0f)
		return lappend(kids, c);
	for (int i = 0; i < c->nchild; i++)
	{
		if (!c->child[i]->fieldname)
			c->child[i]->fieldname = c->fieldname;
		kids = lappend(kids, c->child[i]);
	}
	return kids;
}

static SiftQNode *
leaf_from_token(QParser *p)
{
	QTok	   *t = &p->tok;
	SiftQNode  *n;

	if (++p->nleaves > SIFT_MAX_LEAVES)
		LIMIT(t->loc, "more than %d terms", SIFT_MAX_LEAVES);
	n = sift_query_new_node(t->kind == T_PHRASE ? SQ_PHRASE : t->kind == T_REGEX ? SQ_REGEX : t->leaf, t->loc);
	n->text = t->text;
	n->textlen = t->textlen;
	n->fuzzy = t->fuzzy;
	n->fieldname = t->field;
	return n;
}

static bool
starts_operand(QTokKind k)
{
	return k == T_WORD || k == T_PHRASE || k == T_REGEX || k == T_LPAREN || k == T_LBRACE || k == T_NOT || k == T_FIELD;
}

static void
expect_operand(const QParser *p, const char *after)
{
	if (!starts_operand(p->tok.kind))
		SYNTAX(p->tok.loc, "expected a term after %s, found %s", after, tok_names[p->tok.kind]);
}

static SiftQNode *parse_or(QParser *p, int depth);

static SiftQNode *
parse_braces(QParser *p, int open)
{
	List	   *kids = NIL;

	for (next_token(p); p->tok.kind == T_WORD || p->tok.kind == T_REGEX; next_token(p))
		kids = lappend(kids, leaf_from_token(p));
	if (p->tok.kind == T_EOF)
		SYNTAX(open, "unclosed \"{\"");
	if (p->tok.kind != T_RBRACE)
		SYNTAX(p->tok.loc, "{ } may only hold words, patterns and regular expressions, found %s",
			   tok_names[p->tok.kind]);
	if (kids == NIL)
		SYNTAX(open, "empty { }");
	return list_node(SQ_OR, kids);
}

static SiftQNode *
parse_primary(QParser *p, int depth)
{
	QTok	   *t = &p->tok;
	SiftQNode  *n;
	int			open = t->loc;
	char	   *field = NULL;

	if (t->kind == T_FIELD)
	{
		field = t->field;
		next_token(p);
		open = t->loc;
	}
	switch (t->kind)
	{
		case T_LBRACE:
		case T_LPAREN:
			if (depth >= SIFT_MAX_DEPTH)
				LIMIT(open, "nesting deeper than %d", SIFT_MAX_DEPTH);
			if (t->kind == T_LBRACE)
			{
				n = parse_braces(p, open);
				break;
			}
			next_token(p);
			if (t->kind == T_RPAREN)
				SYNTAX(open, "empty group");
			n = parse_or(p, depth + 1);
			if (t->kind == T_EOF)
				SYNTAX(open, "unclosed \"(\"");
			if (t->kind != T_RPAREN)
				SYNTAX(t->loc, "unexpected %s", tok_names[t->kind]);
			break;
		case T_WORD:
		case T_PHRASE:
		case T_REGEX:
			n = leaf_from_token(p);
			break;
		default:
			SYNTAX(t->loc, "unexpected %s", tok_names[t->kind]);
	}

	if (field && !n->fieldname)
		n->fieldname = field;
	next_token(p);
	if (t->kind == T_CARET)
	{
		int			at = t->loc;
		double		b;

		if (t->spaced)
			SYNTAX(at, "\"^\" must directly follow a term or group");
		b = (double) n->boost * lex_boost(p);
		if (b > SIFT_MAX_BOOST)
			LIMIT(at, "combined boost exceeds %g", SIFT_MAX_BOOST);
		n->boost = (float4) b;
		next_token(p);
	}
	return n;
}

/* NOT chains are folded by parity, so they cost no recursion */
static SiftQNode *
parse_unary(QParser *p, int depth)
{
	int			loc = p->tok.loc;
	bool		seen = false,
				neg = false;
	SiftQNode  *n;

	for (; p->tok.kind == T_NOT; next_token(p))
		seen = true, neg = !neg;
	if (seen)
		expect_operand(p, "NOT");
	n = parse_primary(p, depth);
	if (!neg)
		return n;
	return n->kind == SQ_NOT ? n->child[0] : sift_query_new_not(n, loc);
}

static SiftQNode *
parse_and(QParser *p, int depth)
{
	List	   *kids = NIL;

	for (;;)
	{
		if (p->tok.kind == T_AND)
		{
			if (kids == NIL)
				SYNTAX(p->tok.loc, "unexpected AND");
			next_token(p);
			expect_operand(p, "AND");
		}
		else if (!starts_operand(p->tok.kind))
			break;
		kids = sift_query_add_child(kids, parse_unary(p, depth), SQ_AND);
	}
	if (kids == NIL)
		SYNTAX(p->tok.loc, "unexpected %s", tok_names[p->tok.kind]);
	return list_node(SQ_AND, kids);
}

static SiftQNode *
parse_or(QParser *p, int depth)
{
	List	   *kids;

	check_stack_depth();
	kids = sift_query_add_child(NIL, parse_and(p, depth), SQ_OR);
	while (p->tok.kind == T_OR)
	{
		next_token(p);
		expect_operand(p, "OR");
		kids = sift_query_add_child(kids, parse_and(p, depth), SQ_OR);
	}
	return list_node(SQ_OR, kids);
}

/*
 * A NOT needs a positive sibling in its AND. Checked after parsing, once
 * unboosted groups are spliced: "a (NOT b NOT c)" is valid.
 */
static void
check_nots(const SiftQNode *n, SiftQKind parent)
{
	bool		pos = false;

	if (n->kind == SQ_NOT && parent != SQ_AND)
		SYNTAX(n->loc, "NOT needs a positive term in the same AND group");
	for (int i = 0; i < n->nchild; i++)
	{
		pos |= n->child[i]->kind != SQ_NOT;
		check_nots(n->child[i], n->kind);
	}
	if (n->kind == SQ_AND && !pos)
		SYNTAX(n->child[0]->loc, "NOT needs a positive term in the same AND group");
}

SiftQuery *
sift_query_parse(const char *src, int len)
{
	QParser		p = {.src = src,.len = len,.cpos = 1};
	SiftQuery  *q = palloc0(sizeof(SiftQuery));
	int64_t		max_bytes = sift_limit(query_bytes, SIFT_QUERY_MAX_BYTES);

	if (len > max_bytes)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift query is too long"),
				 errdetail("The query has %d bytes, the maximum is %d.", len, (int) max_bytes)));
	pg_verify_mbstr(GetDatabaseEncoding(), src, len, false);
	next_token(&p);
	if (p.tok.kind == T_EOF)
		SYNTAX(p.tok.loc, "empty query");
	q->root = parse_or(&p, 0);
	if (p.tok.kind != T_EOF)
		SYNTAX(p.tok.loc, "unexpected %s", tok_names[p.tok.kind]);
	check_nots(q->root, SQ_NOTHING);
	q->nleaves = p.nleaves;
	return q;
}

/*
 * The OR of the queries srcs[0..n): each parsed on its own (its errors, its positions), then joined as the
 * parser joins the arms of an OR, so that analysis sees the tree of "q1 OR q2 OR ..."
 */
SiftQuery *
sift_query_parse_any(const char *const *srcs, const int *lens, int n)
{
	SiftQuery  *q = palloc0(sizeof(SiftQuery));
	List	   *kids = NIL;

	Assert(n > 0);
	for (int i = 0; i < n; i++)
	{
		SiftQuery  *e = sift_query_parse(srcs[i], lens[i]);

		kids = sift_query_add_child(kids, e->root, SQ_OR);
		q->nleaves += e->nleaves;
	}
	q->root = list_node(SQ_OR, kids);
	return q;
}

/* ---------------------------------------------------------------- deparse */

static void
append_escaped(StringInfo buf, const char *s, int len, const char *specials, bool space)
{
	for (const char *e = s + len; s < e;)
	{
		int			l = char_len(s, e);

		if (l == 1 && ((space && is_space(*s)) || strchr(specials, *s)) && *s != '\0')
			appendStringInfoChar(buf, '\\');
		appendBinaryStringInfo(buf, s, l);
		s += l;
	}
}

static void
append_word(StringInfo buf, const char *s, int len)
{
	if (strcmp(s, "AND") == 0 || strcmp(s, "OR") == 0 || strcmp(s, "NOT") == 0)
		appendStringInfoChar(buf, '\\');
	append_escaped(buf, s, len, WORD_SPECIALS, true);
}

/* Pattern text keeps "\*", "\?" and "\\"; other specials get escaped */
static void
append_pattern(StringInfo buf, const char *s, int len)
{
	for (const char *e = s + len; s < e;)
	{
		int			l = *s == '\\' ? 2 : char_len(s, e);

		if (*s == '\\' || *s == '*' || *s == '?')
			appendBinaryStringInfo(buf, s, l);
		else
			append_escaped(buf, s, l, WORD_SPECIALS, true);
		s += l;
	}
}

/* Regex text keeps its escapes in pairs; only a bare '/' needs one */
static void
append_regex(StringInfo buf, const char *s, int len)
{
	appendStringInfoChar(buf, '/');
	for (const char *e = s + len; s < e;)
	{
		int			l = *s == '\\' ? 1 + char_len(s + 1, e) : char_len(s, e);

		if (*s == '/')
			appendStringInfoChar(buf, '\\');
		appendBinaryStringInfo(buf, s, l);
		s += l;
	}
	appendStringInfoChar(buf, '/');
}

static void
deparse_node(StringInfo buf, const SiftQNode *n, SiftQKind parent)
{
	bool		boosted = n->boost != 1.0f;
	bool		group = (n->kind == SQ_NOT && boosted) ||
		((n->kind == SQ_AND || n->kind == SQ_OR) &&
		 (boosted || parent == SQ_NOT || parent == n->kind || (parent == SQ_AND && n->kind == SQ_OR)));

	if (group)
		appendStringInfoChar(buf, '(');
	if (n->field)
		appendStringInfo(buf, "%s:", n->fieldname);
	switch (n->kind)
	{
		case SQ_TERM:
			append_word(buf, n->text, n->textlen);
			break;
		case SQ_PREFIX:
			append_word(buf, n->text, n->textlen);
			appendStringInfoChar(buf, '*');
			break;
		case SQ_WILDCARD:
			append_pattern(buf, n->text, n->textlen);
			break;
		case SQ_FUZZY:
			append_word(buf, n->text, n->textlen);
			appendStringInfo(buf, "~%d", n->fuzzy);
			break;
		case SQ_REGEX:
			append_regex(buf, n->text, n->textlen);
			break;
		case SQ_PHRASE:
			appendStringInfoChar(buf, '"');
			if (n->nterms == 0)
				append_escaped(buf, n->text, n->textlen, "\"\\", false);
			for (int i = 0; i < n->nterms; i++)
			{
				if (i > 0)
					appendStringInfoChar(buf, ' ');
				append_escaped(buf, n->terms[i], n->termlens[i], "\"\\", true);
			}
			appendStringInfoChar(buf, '"');
			break;
		case SQ_AND:
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
			{
				if (i > 0)
					appendStringInfoString(buf, n->kind == SQ_AND ? " AND " : " OR ");
				deparse_node(buf, n->child[i], n->kind);
			}
			break;
		case SQ_NOT:
			appendStringInfoString(buf, "NOT ");
			deparse_node(buf, n->child[0], SQ_NOT);
			break;
		case SQ_NOTHING:
			appendStringInfoString(buf, "\"\"");
			break;
	}
	if (group)
		appendStringInfoChar(buf, ')');
	if (boosted)
	{
		char		f[32];

		float_to_shortest_decimal_buf(n->boost, f);
		appendStringInfo(buf, "^%s", f);
	}
}

char *
sift_query_deparse(const SiftQuery *q)
{
	StringInfoData buf;

	initStringInfo(&buf);
	deparse_node(&buf, q->root, SQ_NOTHING);
	return buf.data;
}
