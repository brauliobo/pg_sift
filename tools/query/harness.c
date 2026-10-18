/*
 * harness.c - standalone ASan/UBSan fuzz harness for sift_query.c.
 *
 * Links src/sift_query.c with frontend libpgcommon, tools/pgshim.c (palloc is plain malloc per chunk
 * so that ASan sees every overflow, ereport longjmps back to pgshim_try) and small shims for the
 * rest of the backend services it uses: the analyzer is an ASCII test double, and regexes compile
 * to "never matches" (regex paths are covered by the SQL tests). Reads COPY-escaped queries
 * (tools/query/gen.py) and checks for each one:
 *   - errors are 42601, 54000 or 22021 (invalid encoding) only;
 *   - the canonical form is a fixpoint: deparse(analyze(parse(d))) == d;
 *   - q and its canonical form match the same documents;
 *   - recheck_doc == match_doc for documents matching the relaxed query.
 * Inputs are also fed with random byte damage (invalid UTF-8, NULs).
 */
#include "postgres.h"

#include <setjmp.h>
#include <stdio.h>

#include "mb/pg_wchar.h"
#include "regex/regex.h"

#include "sift_query.h"
#include "../pgshim.h"

void
check_stack_depth(void)
{
}

bool
pg_verify_mbstr(int encoding, const char *mbstr, int len, bool noError)
{
	if (pg_encoding_verifymbstr(encoding, mbstr, len) != len)
		ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE), errmsg("invalid byte sequence")));
	return true;
}

int
pg_mblen_range(const char *s, const char *end)
{
	int			l = pg_encoding_mblen(PG_UTF8, s);

	if (l > end - s)
		ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE), errmsg("truncated character")));
	return l;
}

int
pg_mbstrlen_with_len(const char *s, int limit)
{
	int			n = 0;

	for (int i = 0; i < limit; n++)
		i += pg_encoding_mblen(PG_UTF8, s + i);
	return n;
}

int
pg_mbcliplen(const char *s, int len, int limit)
{
	int			i = 0;

	while (i < len && i + pg_encoding_mblen(PG_UTF8, s + i) <= limit)
		i += pg_encoding_mblen(PG_UTF8, s + i);
	return i;
}

int
pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len)
{
	return pg_wchar_table[PG_UTF8].mb2wchar_with_len((const unsigned char *) from, to, len);
}

/* ------------------------------------------------ List and regex */

List *
lappend(List *list, void *datum)
{
	if (list == NIL)
	{
		list = palloc(offsetof(List, initial_elements) + 4 * sizeof(ListCell));
		list->type = T_List;
		list->length = 0;
		list->max_length = 4;
		list->elements = list->initial_elements;
	}
	else if (list->length == list->max_length)
	{
		ListCell   *e = palloc(2 * list->max_length * sizeof(ListCell));

		memcpy(e, list->elements, list->length * sizeof(ListCell));
		list->elements = e;
		list->max_length *= 2;
	}
	list->elements[list->length++].ptr_value = datum;
	return list;
}

int
pg_regcomp(regex_t *re, const pg_wchar *string, size_t len, int flags, Oid collation)
{
	return REG_OKAY;
}

int
pg_regexec(regex_t *re, const pg_wchar *string, size_t len, size_t search_start,
		   rm_detail_t *details, size_t nmatch, regmatch_t pmatch[], int flags)
{
	return REG_NOMATCH;
}

void
pg_regfree(regex_t *re)
{
}

size_t
pg_regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
	return snprintf(errbuf, errbuf_size, "regex error %d", errcode);
}

/* ------------------------------------------------ analyzer test double */

struct SiftAnalyzer
{
	char	   *buf;
	SiftToken  *tok;
};

/* the double has no prefix_index and no fields */
int
sift_analyzer_prefix_len(const SiftAnalyzer *a)
{
	return 0;
}

int
sift_analyzer_nfields(const SiftAnalyzer *a)
{
	return 0;
}

int
sift_analyzer_field(const SiftAnalyzer *a, const char *name, int len)
{
	return 0;
}

const char *
sift_analyzer_field_names(const SiftAnalyzer *a)
{
	return "";
}

int
sift_analyze_fold(SiftAnalyzer *a, const char *s, int len, char **out)
{
	a->buf = palloc(len + 1);
	for (int i = 0; i < len; i++)
		a->buf[i] = pg_ascii_tolower(s[i]);
	*out = a->buf;
	return len;
}

int
sift_analyze_tokens(SiftAnalyzer *a, const char *text, int len, SiftToken **out)
{
	char	   *f;
	int			n = 0;

	sift_analyze_fold(a, text, len, &f);
	a->tok = palloc((len / 2 + 1) * sizeof(SiftToken));
	for (int i = 0; i < len;)
	{
		int			start = i;

		while (i < len && (isalnum((unsigned char) f[i]) || IS_HIGHBIT_SET(f[i])))
			i++;
		if (i == start)
			i++;
		else
		{
			a->tok[n] = (SiftToken) {.str = f + start,.pos = n,.start = start,.end = i,
				.len = pg_mbcliplen(f + start, i - start, SIFT_MAX_TERM_BYTES)};
			n++;
		}
	}
	*out = a->tok;
	return n;
}

/* ------------------------------------------------ driver */

static const char *const docs[] = {
	"the quick brown fox jumps over the lazy dog", "quick red fox lazy brown bear",
	"e-mail the honey bear about x86_64 builds in 2024", "dog dog dog", "aaa ab abc b bab",
	"caf\xc3\xa9 na\xc3\xafve stra\xc3\x9f" "e", "\xd0\xb6\xd1\x83\xd0\xba and fox", "",
};

static bool
has_phrase(const SiftQNode *n)
{
	bool		r = n->kind == SQ_PHRASE;

	for (int i = 0; i < n->nchild; i++)
		r |= has_phrase(n->child[i]);
	return r;
}

/* Index semantics: positive phrase = AND of its terms, NOT of a phrase subtree = true */
static bool
relaxed(const SiftQNode *n, SiftAnalyzer *a, const char *doc, int len)
{
	SiftQuery	sub = {.root = (SiftQNode *) n,.analyzed = true};

	switch (n->kind)
	{
		case SQ_AND:
			for (int i = 0; i < n->nchild; i++)
				if (!relaxed(n->child[i], a, doc, len))
					return false;
			return true;
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				if (relaxed(n->child[i], a, doc, len))
					return true;
			return false;
		case SQ_NOT:
			return has_phrase(n->child[0]) || !relaxed(n->child[0], a, doc, len);
		case SQ_PHRASE:
			for (int i = 0; i < n->nterms; i++)
			{
				SiftQNode	t = {.kind = SQ_TERM,.text = n->terms[i],.textlen = n->termlens[i]};
				SiftQuery	tq = {.root = &t,.analyzed = true};

				if (!sift_query_match_doc(&tq, a, doc, len))
					return false;
			}
			return true;
		default:
			return sift_query_match_doc(&sub, a, doc, len);
	}
}

static int
unescape(char *s)
{
	char	   *w = s;

	for (char *r = s; *r && *r != '\n'; r++)
	{
		if (*r == '\\' && r[1])
		{
			r++;
			*r = *r == 'n' ? '\n' : *r == 'r' ? '\r' : *r == 't' ? '\t' : *r;
		}
		*w++ = *r;
	}
	return w - s;
}

static long n_ok,
			n_rej,
			n_bad,
			n_relaxed;

typedef struct CheckArg
{
	const char *q;
	int			len;
} CheckArg;

static void
check_run(void *arg)
{
	const char *q = ((CheckArg *) arg)->q;
	int			len = ((CheckArg *) arg)->len;
	SiftAnalyzer an = {0};
	SiftQuery  *q1,
			   *q2;
	char	   *d1,
			   *d2;

	q1 = sift_query_analyze(sift_query_parse(q, len), &an);
	d1 = sift_query_deparse(q1);
	q2 = sift_query_analyze(sift_query_parse(d1, strlen(d1)), &an);
	d2 = sift_query_deparse(q2);
	if (strcmp(d1, d2) != 0)
	{
		n_bad++;
		fprintf(stderr, "not a fixpoint: %.*s -> %s -> %s\n", Min(len, 200), q, d1, d2);
		return;
	}
	for (int i = 0; i <= lengthof(docs); i++)
	{
		const char *doc = i < lengthof(docs) ? docs[i] : q;
		int			dl = i < lengthof(docs) ? strlen(doc) : len;
		bool		m = sift_query_match_doc(q1, &an, doc, dl);

		if (m != sift_query_match_doc(q2, &an, doc, dl))
		{
			n_bad++;
			fprintf(stderr, "canonical form matches differently: %.*s -> %s\n", Min(len, 200), q, d1);
			return;
		}
		if (relaxed(q1->root, &an, doc, dl))
		{
			n_relaxed++;
			if (sift_query_recheck_doc(q1, &an, doc, dl) != m)
			{
				n_bad++;
				fprintf(stderr, "recheck differs from match: %s / %.*s\n", d1, Min(dl, 200), doc);
				return;
			}
		}
	}
	n_ok++;
}

static void
check(const char *q, int len)
{
	CheckArg	a = {q, len};

	if (pgshim_try(check_run, &a))
		return;
	if (pgshim_sqlstate == ERRCODE_SYNTAX_ERROR || pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED ||
		pgshim_sqlstate == ERRCODE_CHARACTER_NOT_IN_REPERTOIRE)
		n_rej++;
	else
	{
		n_bad++;
		fprintf(stderr, "unexpected error %s: %.*s\n", pgshim_errmsg, Min(len, 200), q);
	}
}

int
main(int argc, char **argv)
{
	char	   *line = NULL;
	size_t		cap = 0;
	unsigned	seed = 1;

	for (int f = 1; f < argc; f++)
	{
		FILE	   *in = fopen(argv[f], "r");

		if (in == NULL)
		{
			perror(argv[f]);
			return 2;
		}
		while (getline(&line, &cap, in) > 0)
		{
			int			len = unescape(line);

			check(line, len);
			pgshim_reset();
			/* the same query with one damaged byte: invalid UTF-8, NUL, cut sequence */
			if (len > 0)
			{
				seed = seed * 1103515245 + 12345;
				line[(seed >> 8) % len] = "\xff\x80\xc3\x00\xe2"[(seed >> 4) % 5];
				check(line, len);
				pgshim_reset();
			}
		}
		fclose(in);
	}
	printf("checked %ld (ok %ld, rejected %ld, bad %ld), relaxed-match rechecks %ld\n",
		   n_ok + n_rej + n_bad, n_ok, n_rej, n_bad, n_relaxed);
	return n_bad != 0;
}
