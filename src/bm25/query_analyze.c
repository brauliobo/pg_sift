/*
 * bm25/query_analyze.c - SiftQL analysis: normalization of a parsed query and the per-field weights.
 */
#include "core/host.h"

#include <ctype.h>

#include "bm25/query.h"
#include "bm25/query_int.h"

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
	SiftQNode  *n = sift_query_new_node(kind, raw->loc);

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
	n = sift_query_new_node(SQ_PHRASE, raw->loc);
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
		return sift_query_new_node(SQ_NOTHING, raw->loc);

	/* Folding may drop every character between metas: re-canonicalize */
	if (sift_query_canon_pattern(&buf, &trailing) > 1 || !trailing)
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
			list = sift_query_add_child(list, c, raw->kind);
	}
	if (nothing && raw->kind == SQ_AND)
		return sift_query_new_node(SQ_NOTHING, raw->loc);
	if (list == NIL)
		return nothing ? sift_query_new_node(SQ_NOTHING, raw->loc) : NULL;

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
			return sift_query_new_node(SQ_NOTHING, raw->loc);
		for (int j = npos; j < m; j++)
			for (int i = 0; i < npos; i++)
				if (node_cmp_shape(kids[i], kids[j]->child[0]) == 0)
					return sift_query_new_node(SQ_NOTHING, raw->loc);
	}
	if (m == 1)
		return kids[0];
	n = sift_query_new_node(raw->kind, raw->loc);
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
			return c && c->kind != SQ_NOTHING ? sift_query_new_not(c, raw->loc) : NULL;
		case SQ_AND:
		case SQ_OR:
			return analyze_bool(raw, a, boost, neg, field);
		case SQ_NOTHING:
			break;
	}
	return sift_query_new_node(SQ_NOTHING, raw->loc);
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
		q->root = sift_query_new_node(SQ_NOTHING, 1);
	q->analyzed = true;
	number_leaves(q, q->root);
	return q;
}

/* ---------------------------------------------------------------- field weights */

/* Moves the qualifier of a group to its leaves (the innermost wins), as analysis does, and marks the qualified leaves for deparse */
static void
push_field(SiftQNode *n, const char *field)
{
	if (n->fieldname)
		field = n->fieldname;
	if (n->nchild > 0)
		n->fieldname = NULL;
	else if (!n->fieldname)
		n->fieldname = (char *) field;
	n->field = n->fieldname != NULL;
	for (int i = 0; i < n->nchild; i++)
		push_field(n->child[i], field);
}

/* A word, prefix, pattern or fuzzy leaf that no qualifier scopes becomes the OR of itself in every weighted field; a NOT keeps its leaves */
static SiftQNode *
weigh_node(SiftQNode *n, int nf, const char *const *names, const double *weights)
{
	SiftQNode  *grp;

	switch (n->kind)
	{
		case SQ_AND:
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				n->child[i] = weigh_node(n->child[i], nf, names, weights);
			return n;
		case SQ_TERM:
		case SQ_PREFIX:
		case SQ_WILDCARD:
		case SQ_FUZZY:
			break;
		default:
			return n;
	}
	if (n->fieldname)
		return n;
	grp = nf > 1 ? sift_query_new_node(SQ_OR, n->loc) : n;
	if (nf > 1)
	{
		grp->nchild = nf;
		grp->child = palloc(nf * sizeof(SiftQNode *));
	}
	for (int i = 0; i < nf; i++)
	{
		SiftQNode  *c = nf > 1 ? palloc(sizeof(SiftQNode)) : n;
		double		boost = n->boost * weights[i];

		if (boost > SIFT_MAX_BOOST)
			LIMIT(n->loc, "boost over %d", (int) SIFT_MAX_BOOST);
		*c = *n;
		c->fieldname = (char *) names[i];
		c->field = 1;
		c->boost = (float4) boost;
		if (nf > 1)
			grp->child[i] = c;
	}
	return grp;
}

SiftQuery *
sift_query_weigh(SiftQuery *raw, int nf, const char *const *names, const double *weights)
{
	Assert(!raw->analyzed && nf > 0);
	push_field(raw->root, NULL);
	raw->root = weigh_node(raw->root, nf, names, weights);
	return raw;
}
