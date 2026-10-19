/*
 * regex_test.c - drives mariadb/host/host_regex.c the way src/bm25/sift_query.c does (see regex_check.sh).
 *
 *   regex_test match nosub|sub     stdin: PATTERN<TAB>WORD per line; prints 1, 0 or error per line
 *   regex_test info                stdin: PATTERN per line; prints 1 when re_info says the pattern has a back-reference, else 0 (error
 *                                  when it does not compile), on a regex_t that held garbage before the compile (palloc is not zeroed)
 *   regex_test prefix WORDS        stdin: PATTERN per line; prints none, P:prefix or E:prefix per line and fails when
 *                                  a word the pattern matches does not start with its prefix (or is not equal to it)
 * A byte 0x01 in a pattern or word stands for a newline. Memory is plain malloc, the matcher needs nothing else.
 */
#include "host.h"

void *
palloc(Size size)
{
	return malloc(size ? size : 1);
}

void *
palloc0(Size size)
{
	return calloc(1, size ? size : 1);
}

void *
repalloc(void *p, Size size)
{
	return realloc(p, size);
}

void
pfree(void *p)
{
	free(p);
}

static int
utf8_to_wchar(const char *s, pg_wchar *w)
{
	int			n = 0;

	while (*s)
	{
		unsigned char c = *s++;
		int			extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		pg_wchar	v = extra ? c & (0x3F >> extra) : c;

		while (extra--)
			v = v << 6 | (*s++ & 0x3F);
		w[n++] = v == 1 ? '\n' : v;
	}
	return n;
}

static int
wchar_to_utf8(const pg_wchar *w, int n, char *out)
{
	char	   *o = out;

	for (; n--; w++)
	{
		if (*w < 0x80)
			*o++ = *w;
		else if (*w < 0x800)
			*o++ = 0xC0 | *w >> 6, *o++ = 0x80 | (*w & 0x3F);
		else if (*w < 0x10000)
			*o++ = 0xE0 | *w >> 12, *o++ = 0x80 | (*w >> 6 & 0x3F), *o++ = 0x80 | (*w & 0x3F);
		else
			*o++ = 0xF0 | *w >> 18, *o++ = 0x80 | (*w >> 12 & 0x3F), *o++ = 0x80 | (*w >> 6 & 0x3F),
				*o++ = 0x80 | (*w & 0x3F);
	}
	*o = 0;
	return o - out;
}

static bool
compile(regex_t *re, const char *pat, int flags)
{
	pg_wchar   *w = malloc((strlen(pat) + 1) * sizeof(pg_wchar));
	int			n = utf8_to_wchar(pat, w);
	int			rc = pg_regcomp(re, w, n, flags, C_COLLATION_OID);

	free(w);
	return rc == REG_OKAY;
}

/* the two compiles of analyze_regex in src/bm25/sift_query.c; false when either fails */
static bool
compile_sift(regex_t *re, const char *pat, int flags)
{
	int			opt = strncmp(pat, "(?", 2) == 0 ? 2 + strspn(pat + 2, "cimnpstw") : 0;
	char	   *anchored = malloc(strlen(pat) + 16);
	regex_t		bare;
	bool		ok;

	opt = pat[opt] == ')' ? opt + 1 : 0;
	sprintf(anchored, "%.*s^(?:%s)$", opt, pat, pat + opt);
	ok = compile(&bare, pat, flags) && (pg_regfree(&bare), compile(re, anchored, flags));
	free(anchored);
	return ok;
}

/* 1, 0, or -1 for an execution error */
static int
matches(regex_t *re, const char *word)
{
	pg_wchar   *w = malloc((strlen(word) + 1) * sizeof(pg_wchar));
	int			rc = pg_regexec(re, w, utf8_to_wchar(word, w), 0, NULL, 0, NULL, 0);

	free(w);
	return rc == REG_OKAY ? 1 : rc == REG_NOMATCH ? 0 : -1;
}

static int
do_match(int flags)
{
	char	   *line = NULL,
			   *prev = strdup("");
	size_t		cap = 0;
	ssize_t		len;
	regex_t		re;
	bool		ok = false;

	while ((len = getline(&line, &cap, stdin)) > 0)
	{
		char	   *tab = strchr(line, '\t');
		int			r;

		if (line[len - 1] == '\n')
			line[--len] = 0;
		*tab = 0;
		if (strcmp(prev, line) != 0 || !prev[0])
		{
			if (ok)
				pg_regfree(&re);
			ok = compile_sift(&re, line, flags);
			free(prev);
			prev = strdup(line);
		}
		r = ok ? matches(&re, tab + 1) : -1;
		puts(r < 0 ? "error" : r ? "1" : "0");
	}
	return 0;
}

static char **
read_words(const char *path, int *n)
{
	FILE	   *f = fopen(path, "r");
	char	  **words = NULL,
			   *line = NULL;
	size_t		cap = 0;
	ssize_t		len;

	*n = 0;
	while ((len = getline(&line, &cap, f)) >= 0)
	{
		if (len && line[len - 1] == '\n')
			line[len - 1] = 0;
		for (char *c = line; *c; c++)
			*c = *c == 1 ? '\n' : *c;
		words = realloc(words, (*n + 1) * sizeof(char *));
		words[(*n)++] = strdup(line);
	}
	fclose(f);
	return words;
}

static int
do_prefix(const char *wordsfile)
{
	int			nwords,
				bad = 0,
				i;
	char	  **words = read_words(wordsfile, &nwords),
			   *line = NULL;
	size_t		cap = 0;
	ssize_t		len;

	while ((len = getline(&line, &cap, stdin)) > 0)
	{
		regex_t		re;
		pg_wchar   *p;
		size_t		n;
		char		pfx[4096];
		int			rc;

		if (line[len - 1] == '\n')
			line[--len] = 0;
		if (!compile_sift(&re, line, REG_ADVANCED | REG_NOSUB))
		{
			puts("error");
			continue;
		}
		rc = pg_regprefix(&re, &p, &n);
		if (rc != REG_PREFIX && rc != REG_EXACT)
		{
			puts("none");
			continue;
		}
		wchar_to_utf8(p, n, pfx);
		printf("%s:", rc == REG_EXACT ? "E" : "P");
		for (i = 0; pfx[i]; i++)
			putchar(pfx[i] == '\n' ? 1 : pfx[i]);
		putchar('\n');
		for (i = 0; i < nwords; i++)
			if (matches(&re, words[i]) == 1 &&
				(rc == REG_EXACT ? strcmp(words[i], pfx) != 0 : strncmp(words[i], pfx, strlen(pfx)) != 0))
			{
				fprintf(stderr, "prefix %s of /%s/ is not one of the match \"%s\"\n", pfx, line, words[i]);
				bad++;
			}
	}
	return bad != 0;
}

static int
do_info(void)
{
	char	   *line = NULL;
	size_t		cap = 0;
	ssize_t		len;
	regex_t		re;

	while ((len = getline(&line, &cap, stdin)) > 0)
	{
		if (line[len - 1] == '\n')
			line[--len] = 0;
		memset(&re, 0xA5, sizeof(re));
		if (!compile(&re, line, REG_ADVANCED | REG_NOSUB))
			puts("error");
		else
		{
			puts((re.re_info & REG_UBACKREF) ? "1" : "0");
			pg_regfree(&re);
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "info") == 0)
		return do_info();
	if (argc == 3 && strcmp(argv[1], "match") == 0)
		return do_match(REG_ADVANCED | (strcmp(argv[2], "nosub") == 0 ? REG_NOSUB : 0));
	if (argc == 3 && strcmp(argv[1], "prefix") == 0)
		return do_prefix(argv[2]);
	fprintf(stderr, "usage: regex_test match nosub|sub | regex_test info | regex_test prefix WORDS\n");
	return 2;
}
