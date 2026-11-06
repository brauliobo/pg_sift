/*
 * regex_match.c - drives sqlite/host/host_regex.c the way src/bm25/query_analyze.c does, with the command line of mariadb/tests/regex_test.c
 * so that mariadb/tests/regex_check.sh compares either engine with PostgreSQL's.
 *
 *   regex_match match nosub|sub     stdin: PATTERN<TAB>WORD per line; prints 1, 0 or error per line
 * A byte 0x01 in a pattern or word stands for a newline.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t pg_wchar;

int			pg_regcomp(void *re, const pg_wchar *pattern, size_t len, int cflags, unsigned int collation);
int			pg_regexec(void *re, const pg_wchar *data, size_t len, size_t search_start, void *details, size_t nmatch, void *pmatch, int eflags);
void		pg_regfree(void *re);

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
compile(void *re, const char *pat)
{
	pg_wchar   *w = malloc((strlen(pat) + 1) * sizeof(pg_wchar));
	int			rc = pg_regcomp(re, w, utf8_to_wchar(pat, w), 0, 0);

	free(w);
	return rc == 0;
}

int
main(int argc, char **argv)
{
	char	   *line = NULL;
	size_t		cap = 0;
	ssize_t		len;

	if (argc != 3 || strcmp(argv[1], "match") != 0)
	{
		fprintf(stderr, "usage: regex_match match nosub|sub\n");
		return 2;
	}
	while ((len = getline(&line, &cap, stdin)) > 0)
	{
		char		re[64] = {0},
					bare[64] = {0},
				   *tab = strchr(line, '\t'),
				   *pat = line,
				   *anchored;
		int			opt,
					ok;

		if (line[len - 1] == '\n')
			line[--len] = 0;
		*tab = 0;
		opt = strncmp(pat, "(?", 2) == 0 ? 2 + strspn(pat + 2, "cimnpstw") : 0;
		opt = pat[opt] == ')' ? opt + 1 : 0;
		anchored = malloc(strlen(pat) + 16);
		sprintf(anchored, "%.*s^(?:%s)$", opt, pat, pat + opt);
		ok = compile(bare, pat);
		if (ok)
		{
			pg_regfree(bare);
			ok = compile(re, anchored);
		}
		if (ok)
		{
			pg_wchar   *w = malloc((strlen(tab + 1) + 1) * sizeof(pg_wchar));
			int			rc = pg_regexec(re, w, utf8_to_wchar(tab + 1, w), 0, NULL, 0, NULL, 0);

			puts(rc == 0 ? "1" : rc == 1 ? "0" : "error");
			free(w);
			pg_regfree(re);
		}
		else
			puts("error");
		free(anchored);
	}
	return 0;
}
