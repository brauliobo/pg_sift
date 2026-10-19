/*
 * sift_query.c - SiftQL parser, analysis/normalization, canonical deparse
 * and document evaluation.
 */
#include "sift_host.h"

#include <ctype.h>

#include "sift_query.h"

#define QERROR(code, loc, ...) \
	ereport(ERROR, (errcode(code), \
					errmsg("sift query: %s at position %d", psprintf(__VA_ARGS__), (loc))))
#define SYNTAX(loc, ...)	QERROR(ERRCODE_SYNTAX_ERROR, loc, __VA_ARGS__)
#define LIMIT(loc, ...)		QERROR(ERRCODE_PROGRAM_LIMIT_EXCEEDED, loc, __VA_ARGS__)

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

static inline int
char_len(const char *s, const char *end)
{
	return (unsigned char) *s < 0x80 ? 1 : pg_mblen_range(s, end);
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
static int
canon_pattern(StringInfo pat, bool *trailing)
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
		if (canon_pattern(&pat, &trailing) == 1 && trailing)
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

static SiftQNode *
new_node(SiftQKind kind, int loc)
{
	SiftQNode  *n = palloc0(sizeof(SiftQNode));

	n->kind = kind;
	n->boost = 1.0f;
	n->loc = loc;
	n->leafno = -1;
	return n;
}

static SiftQNode *
new_not(SiftQNode *child, int loc)
{
	SiftQNode  *n = new_node(SQ_NOT, loc);

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
	n = new_node(kind, ((SiftQNode *) linitial(kids))->loc);
	n->nchild = list_length(kids);
	n->child = palloc(n->nchild * sizeof(SiftQNode *));
	foreach_ptr(SiftQNode, c, kids)
		n->child[i++] = c;
	return n;
}

/* Splices unboosted groups of the same operator: (a b) c == a b c; a name:(a b) hands its qualifier to the members first */
static List *
add_child(List *kids, SiftQNode *c, SiftQKind kind)
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
	n = new_node(t->kind == T_PHRASE ? SQ_PHRASE : t->kind == T_REGEX ? SQ_REGEX : t->leaf, t->loc);
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
	return n->kind == SQ_NOT ? n->child[0] : new_not(n, loc);
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
		kids = add_child(kids, parse_unary(p, depth), SQ_AND);
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
	kids = add_child(NIL, parse_and(p, depth), SQ_OR);
	while (p->tok.kind == T_OR)
	{
		next_token(p);
		expect_operand(p, "OR");
		kids = add_child(kids, parse_and(p, depth), SQ_OR);
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

	if (len > SIFT_QUERY_MAX_BYTES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift query is too long"),
				 errdetail("The query has %d bytes, the maximum is %d.", len, SIFT_QUERY_MAX_BYTES)));
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

/* ---------------------------------------------------------------- analysis */

static int
cmp_bytes(const char *a, int al, const char *b, int bl)
{
	int			c = Min(al, bl) > 0 ? memcmp(a, b, Min(al, bl)) : 0;

	return c ? c : (al > bl) - (al < bl);
}

static int	node_cmp(const SiftQNode *a, const SiftQNode *b);

/* Total order ignoring the nodes' own boosts (children's boosts count) */
static int
node_cmp_shape(const SiftQNode *a, const SiftQNode *b)
{
	int			c;

	if (a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	if ((c = cmp_bytes(a->text, a->textlen, b->text, b->textlen)) != 0)
		return c;
	if (a->fuzzy != b->fuzzy)
		return a->fuzzy < b->fuzzy ? -1 : 1;
	if (a->field != b->field)
		return a->field < b->field ? -1 : 1;
	if (a->nterms != b->nterms)
		return a->nterms < b->nterms ? -1 : 1;
	for (int i = 0; i < a->nterms; i++)
		if ((c = cmp_bytes(a->terms[i], a->termlens[i], b->terms[i], b->termlens[i])) != 0)
			return c;
	if (a->nchild != b->nchild)
		return a->nchild < b->nchild ? -1 : 1;
	for (int i = 0; i < a->nchild; i++)
		if ((c = node_cmp(a->child[i], b->child[i])) != 0)
			return c;
	return 0;
}

static int
node_cmp(const SiftQNode *a, const SiftQNode *b)
{
	int			c = node_cmp_shape(a, b);

	return c ? c : (a->boost > b->boost) - (a->boost < b->boost);
}

static int
node_qsort_cmp(const void *a, const void *b)
{
	return node_cmp(*(SiftQNode *const *) a, *(SiftQNode *const *) b);
}

static SiftQNode *
new_leaf(const SiftQNode *raw, SiftQKind kind, const char *text, int len, float4 boost)
{
	SiftQNode  *n = new_node(kind, raw->loc);

	n->boost = boost;
	n->fuzzy = kind == SQ_FUZZY ? raw->fuzzy : 0;
	n->text = pnstrdup(text, len);
	n->textlen = len;
	return n;
}

/* Words and phrases: index-time tokenization; several tokens make a phrase */
static SiftQNode *
analyze_words(const SiftQNode *raw, SiftAnalyzer *a, float4 boost)
{
	SiftToken  *tok;
	int			nt = sift_analyze_tokens(a, raw->text, raw->textlen, &tok);
	SiftQNode  *n;

	if (nt <= 1)
		return nt ? new_leaf(raw, SQ_TERM, tok[0].str, tok[0].len, boost) : NULL;
	if (nt > SIFT_MAX_PHRASE)
		LIMIT(raw->loc, "phrase longer than %d terms", SIFT_MAX_PHRASE);
	n = new_node(SQ_PHRASE, raw->loc);
	n->boost = boost;
	n->nterms = nt;
	n->terms = palloc(nt * sizeof(char *));
	n->termlens = palloc(nt * sizeof(int));
	for (int i = 0; i < nt; i++)
	{
		n->terms[i] = pnstrdup(tok[i].str, tok[i].len);
		n->termlens[i] = tok[i].len;
	}
	return n;
}

/* Folds s into buf, returns the folded length; escape protects pattern metas */
static int
append_folded(SiftAnalyzer *a, StringInfo buf, const char *s, int len, bool escape)
{
	char	   *f;
	int			fl;

	if (len == 0)
		return 0;
	fl = sift_analyze_fold(a, s, len, &f);
	for (int i = 0; i < fl; i++)
	{
		if (escape && f[i] != '\0' && strchr("*?\\", f[i]))
			appendStringInfoChar(buf, '\\');
		appendStringInfoChar(buf, f[i]);
	}
	return fl;
}

/* A prefix leaf; one the index holds as a term of its own is marked */
static SiftQNode *
prefix_leaf(const SiftQNode *raw, SiftAnalyzer *a, const char *text, int len, float4 boost)
{
	SiftQNode  *n = new_leaf(raw, SQ_PREFIX, text, len, boost);

	n->gram = pg_mbstrlen_with_len(text, len) <= sift_analyzer_prefix_len(a);
	return n;
}

/*
 * Prefix and wildcard literals: folded, not stemmed. A pattern that needs
 * more bytes than any term has matches nothing.
 */
static SiftQNode *
analyze_literal(const SiftQNode *raw, SiftAnalyzer *a, float4 boost)
{
	StringInfoData buf,
				run;
	int			lit = 0,
				minlen = 0;
	bool		trailing;

	initStringInfo(&buf);
	initStringInfo(&run);
	if (raw->kind == SQ_PREFIX)
	{
		append_folded(a, &buf, raw->text, raw->textlen, false);
		buf.len = pg_mbcliplen(buf.data, buf.len, SIFT_MAX_TERM_BYTES);
		return buf.len ? prefix_leaf(raw, a, buf.data, buf.len, boost) : NULL;
	}

	for (int i = 0; i <= raw->textlen; i++)
	{
		char		c = i < raw->textlen ? raw->text[i] : '\0';

		if (c != '*' && c != '?' && c != '\0')
		{
			i += c == '\\';
			appendStringInfoChar(&run, raw->text[i]);
			continue;
		}
		lit += append_folded(a, &buf, run.data, run.len, true);
		minlen += c == '?';
		resetStringInfo(&run);
		if (c)
			appendStringInfoChar(&buf, c);
	}
	if (lit == 0)
		return NULL;
	if (lit + minlen > SIFT_MAX_TERM_BYTES)
		return new_node(SQ_NOTHING, raw->loc);

	/* Folding may drop every character between metas: re-canonicalize */
	if (canon_pattern(&buf, &trailing) > 1 || !trailing)
		return new_leaf(raw, SQ_WILDCARD, buf.data, buf.len, boost);
	resetStringInfo(&run);
	for (int i = 0; i < buf.len - 1; i++)
	{
		i += buf.data[i] == '\\';
		appendStringInfoChar(&run, buf.data[i]);
	}
	return prefix_leaf(raw, a, run.data, run.len, boost);
}

/*
 * Fuzzy applies to a word that is one token: its folded, unstemmed span.
 * Other words, and tokens under 3 characters, match as plain words; under
 * 6 characters one edit is allowed.
 */
static SiftQNode *
analyze_fuzzy(const SiftQNode *raw, SiftAnalyzer *a, float4 boost)
{
	SiftToken  *tok;
	StringInfoData buf;
	SiftQNode  *n;
	int			nchar,
				start,
				end;

	if (sift_analyze_tokens(a, raw->text, raw->textlen, &tok) != 1)
		return analyze_words(raw, a, boost);
	start = tok[0].start;
	end = tok[0].end;
	initStringInfo(&buf);
	append_folded(a, &buf, raw->text + start, end - start, false);
	buf.len = pg_mbcliplen(buf.data, buf.len, SIFT_MAX_TERM_BYTES);
	nchar = pg_mbstrlen_with_len(buf.data, buf.len);
	if (nchar < 3)
		return analyze_words(raw, a, boost);
	n = new_leaf(raw, SQ_FUZZY, buf.data, buf.len, boost);
	n->fuzzy = nchar < 6 ? 1 : raw->fuzzy;
	return n;
}

static regex_t *
compile_regex(const char *pat, int len, int loc)
{
	regex_t    *re = palloc(sizeof(regex_t));
	pg_wchar   *w = palloc((len + 1) * sizeof(pg_wchar));
	int			rc = pg_regcomp(re, w, pg_mb2wchar_with_len(pat, w, len),
								REG_ADVANCED | REG_NOSUB, C_COLLATION_OID);

	if (rc != REG_OKAY)
	{
		char		msg[128];

		pg_regerror(rc, re, msg, sizeof(msg));
		SYNTAX(loc, "invalid regular expression: %s", msg);
	}
	if (re->re_info & REG_UBACKREF)
		SYNTAX(loc, "back-references are not supported in a regular expression");
	pfree(w);
	return re;
}

/*
 * Validates the pattern alone, so "a)|(b" cannot escape the anchors. Leading
 * embedded options "(?flags)" stay ahead of the anchors.
 */
static SiftQNode *
analyze_regex(const SiftQNode *raw, float4 boost)
{
	SiftQNode  *n = new_leaf(raw, SQ_REGEX, raw->text, raw->textlen, boost);
	int			opt = strncmp(n->text, "(?", 2) == 0 ? 2 + strspn(n->text + 2, "cimnpstw") : 0;
	char	   *anchored;

	opt = n->text[opt] == ')' ? opt + 1 : 0;
	anchored = psprintf("%.*s^(?:%s)$", opt, n->text, n->text + opt);

	pg_regfree(compile_regex(n->text, n->textlen, n->loc));
	n->re = compile_regex(anchored, strlen(anchored), n->loc);
	return n;
}

static SiftQNode *analyze_node(const SiftQNode *raw, SiftAnalyzer *a, double boost, bool neg, const char *field);

/*
 * Folds c into an equal sibling prev: a positive leaf twice scores as one
 * leaf with the boosts added; NOTs and negated subtrees just dedupe. Inner
 * positive duplicates stay, since boosts live on leaves only.
 */
static bool
merge_dup(SiftQNode *prev, const SiftQNode *c, bool neg)
{
	double		sum = (double) prev->boost + c->boost;

	if (node_cmp_shape(prev, c) != 0)
		return false;
	if (neg || prev->kind == SQ_NOT)
		return true;
	if (prev->nchild > 0 || sum > SIFT_MAX_BOOST)
		return false;
	prev->boost = (float4) sum;
	return true;
}

/*
 * Flattens, sorts and merges duplicates (adding boosts while the sum stays
 * in range). AND folds to SQ_NOTHING with no positive child or with x and
 * NOT x; empty leaves are dropped. Returns NULL when nothing is left.
 */
static SiftQNode *
analyze_bool(const SiftQNode *raw, SiftAnalyzer *a, double boost, bool neg, const char *field)
{
	List	   *list = NIL;
	bool		nothing = false;
	SiftQNode **kids;
	SiftQNode  *n;
	int			nk = 0,
				m = 0,
				npos = 0;

	for (int i = 0; i < raw->nchild; i++)
	{
		SiftQNode  *c = analyze_node(raw->child[i], a, boost, neg, raw->fieldname ? raw->fieldname : field);

		if (c && c->kind == SQ_NOTHING)
			nothing = true;
		else if (c)
			list = add_child(list, c, raw->kind);
	}
	if (nothing && raw->kind == SQ_AND)
		return new_node(SQ_NOTHING, raw->loc);
	if (list == NIL)
		return nothing ? new_node(SQ_NOTHING, raw->loc) : NULL;

	kids = palloc(list_length(list) * sizeof(SiftQNode *));
	foreach_ptr(SiftQNode, c, list)
		kids[nk++] = c;
	qsort(kids, nk, sizeof(SiftQNode *), node_qsort_cmp);
	for (int i = 0; i < nk; i++)
		if (m == 0 || !merge_dup(kids[m - 1], kids[i], neg))
			kids[m++] = kids[i];
	qsort(kids, m, sizeof(SiftQNode *), node_qsort_cmp);

	if (raw->kind == SQ_AND)
	{
		while (npos < m && kids[npos]->kind != SQ_NOT)
			npos++;
		if (npos == 0)
			return new_node(SQ_NOTHING, raw->loc);
		for (int j = npos; j < m; j++)
			for (int i = 0; i < npos; i++)
				if (node_cmp_shape(kids[i], kids[j]->child[0]) == 0)
					return new_node(SQ_NOTHING, raw->loc);
	}
	if (m == 1)
		return kids[0];
	n = new_node(raw->kind, raw->loc);
	n->nchild = m;
	n->child = kids;
	return n;
}

#define NOT_QUALIFIABLE \
	"a field applies to words, prefixes, patterns and fuzzy terms, not to phrases or regular expressions"

/*
 * Without fields a colon is plain text, as before fields existed: name:word is the word "name:word",
 * and so is every word of name:(a b). An index with fields rejects a name it does not have.
 */
static const SiftQNode *
unqualified(const SiftQNode *raw, const char *name)
{
	SiftQNode  *n;

	if (raw->kind == SQ_PHRASE || raw->kind == SQ_REGEX)
		SYNTAX(raw->loc, NOT_QUALIFIABLE);
	n = palloc(sizeof(SiftQNode));
	*n = *raw;
	n->text = psprintf("%s:%s", name, raw->text);
	n->textlen = strlen(n->text);
	n->fieldname = NULL;
	return n;
}

/* The fields of a, "name, city" */
static char *
field_list(const SiftAnalyzer *a)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (const char *s = sift_analyzer_field_names(a); *s; s++)
		if (*s == ',')
			appendStringInfoString(&buf, ", ");
		else
			appendStringInfoChar(&buf, *s);
	return buf.data;
}

/* Binds an analyzed leaf to the field it is qualified with, if any */
static SiftQNode *
qualify(SiftQNode *n, const SiftQNode *raw, SiftAnalyzer *a, const char *field)
{
	const char *name = raw->fieldname ? raw->fieldname : field;
	int			k;

	if (name == NULL || n == NULL || n->kind == SQ_NOTHING)
		return n;
	if ((k = sift_analyzer_field(a, name, strlen(name))) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("sift query: unknown field \"%s\" (available fields: %s) at position %d", name, field_list(a), raw->loc),
				 errhint("Fields come from the fields option of the sift index; a literal colon is written \\:.")));
	if (n->kind == SQ_PHRASE || n->kind == SQ_REGEX)
		SYNTAX(raw->loc, NOT_QUALIFIABLE);
	n->field = k;
	n->fieldname = pstrdup(name);
	return n;
}

/*
 * boost is the product of the boosts above raw; under NOT (neg) boosts are
 * meaningless and reset to 1. field is the qualifier of the group above.
 */
static SiftQNode *
analyze_node(const SiftQNode *raw, SiftAnalyzer *a, double boost, bool neg, const char *field)
{
	SiftQNode  *c;
	float4		b;

	check_stack_depth();
	boost *= neg ? 1.0 : raw->boost;
	if (boost > SIFT_MAX_BOOST)
		LIMIT(raw->loc, "combined boost exceeds %g", SIFT_MAX_BOOST);
	b = neg ? 1.0f : (float4) boost;
	if (raw->kind != SQ_NOT && raw->kind != SQ_AND && raw->kind != SQ_OR &&
		(raw->fieldname || field) && sift_analyzer_nfields(a) == 0)
	{
		raw = unqualified(raw, raw->fieldname ? raw->fieldname : field);
		field = NULL;
	}
	switch (raw->kind)
	{
		case SQ_TERM:
		case SQ_PHRASE:
			return qualify(analyze_words(raw, a, b), raw, a, field);
		case SQ_PREFIX:
		case SQ_WILDCARD:
			return qualify(analyze_literal(raw, a, b), raw, a, field);
		case SQ_FUZZY:
			return qualify(analyze_fuzzy(raw, a, b), raw, a, field);
		case SQ_REGEX:
			return qualify(analyze_regex(raw, b), raw, a, field);
		case SQ_NOT:
			c = analyze_node(raw->child[0], a, 1.0, true, raw->fieldname ? raw->fieldname : field);
			return c && c->kind != SQ_NOTHING ? new_not(c, raw->loc) : NULL;
		case SQ_AND:
		case SQ_OR:
			return analyze_bool(raw, a, boost, neg, field);
		case SQ_NOTHING:
			break;
	}
	return new_node(SQ_NOTHING, raw->loc);
}

static void
number_leaves(SiftQuery *q, SiftQNode *n)
{
	q->needs_recheck |= n->kind == SQ_PHRASE;
	if (n->nchild == 0 && n->kind != SQ_NOTHING)
		n->leafno = q->nleaves++;
	for (int i = 0; i < n->nchild; i++)
		number_leaves(q, n->child[i]);
}

SiftQuery *
sift_query_analyze(const SiftQuery *raw, SiftAnalyzer *a)
{
	SiftQuery  *q = palloc0(sizeof(SiftQuery));

	Assert(!raw->analyzed);
	q->root = analyze_node(raw->root, a, 1.0, false, NULL);
	if (q->root == NULL)
		q->root = new_node(SQ_NOTHING, 1);
	q->analyzed = true;
	number_leaves(q, q->root);
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

/* ---------------------------------------------------------------- term matchers */

#define WBUF	(SIFT_MAX_TERM_BYTES + 1)

/* Wide copy of s, in buf when it fits */
static pg_wchar *
to_wchar(const char *s, int len, pg_wchar *buf, int *wlen)
{
	pg_wchar   *w = len < WBUF ? buf : palloc((len + 1) * sizeof(pg_wchar));

	*wlen = pg_mb2wchar_with_len(s, w, len);
	return w;
}

/* '*' any run, '?' one character, '\c' literal c; backtracks to the last '*' */
static bool
wild_match(const char *p, const char *pe, const char *t, const char *te)
{
	const char *sp = NULL,
			   *st = NULL;

	while (t < te)
	{
		if (p < pe && *p == '*')
		{
			sp = ++p;
			st = t;
			continue;
		}
		if (p < pe && *p == '?')
		{
			p++;
			t += char_len(t, te);
			continue;
		}
		if (p < pe)
		{
			const char *lit = p + (*p == '\\');
			int			l = char_len(lit, pe);

			if (l <= te - t && memcmp(lit, t, l) == 0)
			{
				p = lit + l;
				t += l;
				continue;
			}
		}
		if (sp == NULL)
			return false;
		st += char_len(st, te);
		t = st;
		p = sp;
	}
	while (p < pe && *p == '*')
		p++;
	return p == pe;
}

static bool
regex_match(regex_t *re, const char *s, int len)
{
	pg_wchar	buf[WBUF];
	int			wl;
	pg_wchar   *w = to_wchar(s, len, buf, &wl);
	int			rc = pg_regexec(re, w, wl, 0, NULL, 0, NULL, 0);

	if (rc != REG_OKAY && rc != REG_NOMATCH)
	{
		char		msg[128];

		pg_regerror(rc, re, msg, sizeof(msg));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("regular expression failed: %s", msg)));
	}
	return rc == REG_OKAY;
}

/* Levenshtein distance of a and b is <= k, in a diagonal band of width 2k+1 */
static bool
lev_within(const pg_wchar *a, int n, const pg_wchar *b, int m, int k)
{
	int			rows[2][WBUF + 4];
	int		   *prev = rows[0],
			   *row = rows[1];
	int			big = k + 1;

	if (abs(n - m) > k)
		return false;
	Assert(n < WBUF);
	for (int j = 0; j <= Min(m, k + 1); j++)
		prev[j] = Min(j, big);
	for (int i = 1; i <= n; i++)
	{
		int			lo = Max(1, i - k),
					hi = Min(m, i + k),
					best;
		int		   *tmp;

		row[lo - 1] = lo == 1 ? Min(i, big) : big;
		best = row[lo - 1];
		for (int j = lo; j <= hi; j++)
		{
			int			v = prev[j - 1] + (a[i - 1] != b[j - 1]);

			v = Min(v, prev[j] + 1);
			v = Min(v, row[j - 1] + 1);
			row[j] = Min(v, big);
			best = Min(best, row[j]);
		}
		if (hi < m)
			row[hi + 1] = big;
		if (best > k)
			return false;
		tmp = prev, prev = row, row = tmp;
	}
	return prev[m] <= k;
}

/* Edit distance <= k with the first character fixed */
static bool
fuzzy_match(const char *q, int qlen, const char *t, int tlen, int k)
{
	int			l = char_len(q, q + qlen),
				n,
				m;
	pg_wchar	qb[WBUF],
				tb[WBUF];
	pg_wchar   *qw,
			   *tw;

	if (abs(qlen - tlen) > 4 * k || tlen < l || memcmp(q, t, l) != 0)
		return false;
	qw = to_wchar(q + l, qlen - l, qb, &n);
	tw = to_wchar(t + l, tlen - l, tb, &m);
	return lev_within(qw, n, tw, m, k);
}

bool
sift_query_leaf_match(const SiftQNode *leaf, const char *term, int len)
{
	switch (leaf->kind)
	{
		case SQ_TERM:
			return len == leaf->textlen && memcmp(term, leaf->text, len) == 0;
		case SQ_PREFIX:
			return len >= leaf->textlen && memcmp(term, leaf->text, leaf->textlen) == 0;
		case SQ_WILDCARD:
			return wild_match(leaf->text, leaf->text + leaf->textlen, term, term + len);
		case SQ_REGEX:
			return regex_match(leaf->re, term, len);
		case SQ_FUZZY:
			return fuzzy_match(leaf->text, leaf->textlen, term, len, leaf->fuzzy);
		default:
			elog(ERROR, "sift: query node kind %d has no term matcher", leaf->kind);
	}
	pg_unreachable();
}

/* ---------------------------------------------------------------- document evaluation */

/* A document's tokens, grouped by distinct term once a leaf needs the grouping (phrases scan the tokens) */
struct SiftDoc
{
	SiftToken  *tok;
	int			ntok;
	bool		grouped;
	int			nterms;
	int		   *id;				/* token -> distinct term */
	int		   *next;			/* token -> next token of the same term, -1 */
	int		   *first;			/* distinct term -> first token */
	int		   *slots;			/* open addressing: term + 1, 0 = free */
	uint32		mask;
	uint8	   *sec;			/* token -> 0-based section of the text (split at SIFT_FIELD_SEP), NULL when there is one */
};

static uint32
doc_slot(const SiftDoc *d, const char *s, int len)
{
	for (uint32 h = hash_bytes((const unsigned char *) s, len) & d->mask;; h = (h + 1) & d->mask)
	{
		int			t = d->slots[h] - 1;

		if (t < 0 || (d->tok[d->first[t]].len == len && memcmp(d->tok[d->first[t]].str, s, len) == 0))
			return h;
	}
}

/* Groups the tokens by distinct term: hash table, per-term chains */
static void
doc_group(SiftDoc *d)
{
	Size		size = 16;
	int		   *last;

	if (d->grouped)
		return;
	d->grouped = true;
	while (size < (Size) d->ntok * 2)
		size <<= 1;
	d->mask = size - 1;
	d->slots = palloc0(size * sizeof(int));
	d->id = palloc(d->ntok * sizeof(int));
	d->next = palloc(d->ntok * sizeof(int));
	d->first = palloc(d->ntok * sizeof(int));
	last = palloc(d->ntok * sizeof(int));
	d->nterms = 0;
	for (int t = 0; t < d->ntok; t++)
	{
		uint32		h = doc_slot(d, d->tok[t].str, d->tok[t].len);
		int			id = d->slots[h] - 1;

		if (id < 0)
		{
			id = d->nterms++;
			d->slots[h] = id + 1;
			d->first[id] = t;
		}
		else
			d->next[last[id]] = t;
		d->id[t] = id;
		d->next[t] = -1;
		last[id] = t;
	}
}

static int
doc_find(SiftDoc *d, const char *s, int len)
{
	doc_group(d);
	return d->slots[doc_slot(d, s, len)] - 1;
}

/* Allocates in CurrentMemoryContext; tokens stay in analyzer memory */
static void
doc_init(SiftDoc *d, SiftAnalyzer *a, const char *text, int len)
{
	d->ntok = sift_analyze_tokens(a, text, len, &d->tok);
	d->grouped = false;
	d->sec = NULL;
	if (sift_analyzer_nfields(a) > 0 && memchr(text, SIFT_FIELD_SEP, len))
	{
		const char *sep = memchr(text, SIFT_FIELD_SEP, len);
		int			section = 0;

		d->sec = palloc(d->ntok);
		for (int t = 0; t < d->ntok; t++)
		{
			for (; sep && sep < text + d->tok[t].start; section++)
				sep = memchr(sep + 1, SIFT_FIELD_SEP, text + len - (sep + 1));
			d->sec[t] = Min(section, UINT8_MAX);
		}
	}
}

/* Tokens of the text's field-th section (0: any) that satisfy the leaf, or equal/extend the prefix pre of len bytes */
static int
doc_count_tokens(const SiftDoc *d, int field, const SiftQNode *leaf, const char *pre, int len)
{
	int			n = 0;

	for (int t = 0; t < d->ntok; t++)
	{
		const SiftToken *w = &d->tok[t];

		if (field && (d->sec ? d->sec[t] : 0) + 1 != field)
			continue;
		if (leaf ? sift_query_leaf_match(leaf, w->str, w->len) : w->len >= len && memcmp(w->str, pre, len) == 0)
			n++;
	}
	return n;
}

/* The terms at consecutive positions, found on the tokens: a phrase never needs the term grouping */
static bool
eval_phrase(const SiftDoc *d, const SiftQNode *n)
{
	const SiftToken *tok = d->tok;

	for (int t = 0; t + n->nterms <= d->ntok; t++)
	{
		int			i = 0;

		while (i < n->nterms && tok[t + i].len == n->termlens[i] && tok[t + i].pos == tok[t].pos + i &&
			   memcmp(tok[t + i].str, n->terms[i], n->termlens[i]) == 0)
			i++;
		if (i == n->nterms)
			return true;
	}
	return false;
}

static bool
eval_node(SiftDoc *d, const SiftQNode *n)
{
	if (n->field)
		return doc_count_tokens(d, n->field, n, NULL, 0) > 0;
	switch (n->kind)
	{
		case SQ_AND:
			for (int i = 0; i < n->nchild; i++)
				if (!eval_node(d, n->child[i]))
					return false;
			return true;
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				if (eval_node(d, n->child[i]))
					return true;
			return false;
		case SQ_NOT:
			return !eval_node(d, n->child[0]);
		case SQ_NOTHING:
			return false;
		case SQ_TERM:
			return doc_find(d, n->text, n->textlen) >= 0;
		case SQ_PHRASE:
			return eval_phrase(d, n);
		default:
			doc_group(d);
			for (int t = 0; t < d->nterms; t++)
				if (sift_query_leaf_match(n, d->tok[d->first[t]].str, d->tok[d->first[t]].len))
					return true;
			return false;
	}
}

bool
sift_query_match_doc(const SiftQuery *q, SiftAnalyzer *a, const char *text, int len)
{
	SiftDoc	d;

	Assert(q->analyzed);
	if (q->root->kind == SQ_NOTHING)
		return false;
	doc_init(&d, a, text, len);
	return eval_node(&d, q->root);
}

static bool
has_phrase(const SiftQNode *n)
{
	if (n->kind == SQ_PHRASE)
		return true;
	for (int i = 0; i < n->nchild; i++)
		if (has_phrase(n->child[i]))
			return true;
	return false;
}

bool
sift_doc_recheck(const SiftQuery *q, SiftDoc *d)
{
	const SiftQNode *r = q->root;

	Assert(q->analyzed);
	if (!q->needs_recheck)
		return true;
	if (r->kind != SQ_AND)
		return eval_node(d, r);
	for (int i = 0; i < r->nchild; i++)
		if (has_phrase(r->child[i]) && !eval_node(d, r->child[i]))
			return false;
	return true;
}

bool
sift_query_recheck_doc(const SiftQuery *q, SiftAnalyzer *a, const char *text, int len)
{
	return sift_doc_recheck(q, sift_doc_open(a, text, len));
}

SiftDoc *
sift_doc_open(SiftAnalyzer *a, const char *text, int len)
{
	SiftDoc    *d = palloc(sizeof(SiftDoc));

	doc_init(d, a, text, len);
	return d;
}

int
sift_doc_ntokens(const SiftDoc *d)
{
	return d->ntok;
}

int
sift_doc_nterms(SiftDoc *d)
{
	doc_group(d);
	return d->nterms;
}

const char *
sift_doc_term(SiftDoc *d, int i, int *len)
{
	doc_group(d);
	*len = d->tok[d->first[i]].len;
	return d->tok[d->first[i]].str;
}

int
sift_doc_tf(SiftDoc *d, int i)
{
	int			tf = 0;

	doc_group(d);
	for (int t = d->first[i]; t >= 0; t = d->next[t])
		tf++;
	return tf;
}

int
sift_doc_find(SiftDoc *d, const char *term, int len)
{
	return doc_find(d, term, len);
}

int
sift_doc_term_tf(SiftDoc *d, const char *term, int len)
{
	int			field = 0;

	if (SIFT_FIELD_TERM(*term, SIFT_MAX_FIELDS))
	{
		field = (unsigned char) *term - SIFT_FIELD_BASE + 1;
		term++, len--;
	}
	if (*term == SIFT_PREFIX_SEP)
		return doc_count_tokens(d, field, NULL, term + 1, len - 1);
	if (field == 0)
	{
		int			at = doc_find(d, term, len);

		return at < 0 ? 0 : sift_doc_tf(d, at);
	}
	return doc_count_tokens(d, field, &(SiftQNode) {.kind = SQ_TERM, .text = (char *) term, .textlen = len}, NULL, 0);
}

bool
sift_doc_match(SiftDoc *d, const SiftQNode *n)
{
	return eval_node(d, n);
}
