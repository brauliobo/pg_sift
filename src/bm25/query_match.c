/*
 * bm25/query_match.c - SiftQL term matchers and the evaluation of an analyzed query on a document.
 */
#include "sift_host.h"

#include <ctype.h>

#include "bm25/query.h"
#include "bm25/query_int.h"

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

int
sift_doc_leaf_tf(SiftDoc *d, const SiftQNode *leaf)
{
	return doc_count_tokens(d, leaf->field, leaf, NULL, 0);
}

bool
sift_doc_match(SiftDoc *d, const SiftQNode *n)
{
	return eval_node(d, n);
}
