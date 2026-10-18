/*
 * rx_oracle.c - a throwaway PostgreSQL extension for regex_check.sh: runs the patterns of SiftQL through the real
 * pg_regcomp/pg_regexec/pg_regprefix exactly as src/sift_query.c does (REG_ADVANCED, the C collation, the bare
 * pattern compiled once to validate it, then "(?flags)^(?:PATTERN)$").
 */
#include "postgres.h"

#include "catalog/pg_collation_d.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "regex/regex.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

static bool
compile(regex_t *re, const char *pat, int len, int flags)
{
	pg_wchar   *w = palloc((len + 1) * sizeof(pg_wchar));
	int			n = pg_mb2wchar_with_len(pat, w, len);

	return pg_regcomp(re, w, n, flags, C_COLLATION_OID) == REG_OKAY;
}

/* the two compiles of analyze_regex; false when either fails */
static bool
compile_sift(regex_t *re, const char *pat, int flags)
{
	int			opt = strncmp(pat, "(?", 2) == 0 ? 2 + strspn(pat + 2, "cimnpstw") : 0;
	char	   *anchored;
	regex_t		bare;

	opt = pat[opt] == ')' ? opt + 1 : 0;
	anchored = psprintf("%.*s^(?:%s)$", opt, pat, pat + opt);
	return compile(&bare, pat, strlen(pat), flags) && compile(re, anchored, strlen(anchored), flags);
}

static char *
cstr(text *t)
{
	char	   *s = text_to_cstring(t);
	char	   *p;

	for (p = s; *p; p++)
		if (*p == 1)
			*p = '\n';
	return s;
}

PG_FUNCTION_INFO_V1(rx_match);
Datum
rx_match(PG_FUNCTION_ARGS)
{
	char	   *pat = cstr(PG_GETARG_TEXT_PP(0)),
			   *word = cstr(PG_GETARG_TEXT_PP(1));
	int			len = strlen(word);
	pg_wchar   *w = palloc((len + 1) * sizeof(pg_wchar));
	regex_t		re;
	int			rc,
				n;

	if (!compile_sift(&re, pat, PG_GETARG_INT32(2)))
		PG_RETURN_TEXT_P(cstring_to_text("error"));
	n = pg_mb2wchar_with_len(word, w, len);
	rc = pg_regexec(&re, w, n, 0, NULL, 0, NULL, 0);
	PG_RETURN_TEXT_P(cstring_to_text(rc == REG_OKAY ? "1" : rc == REG_NOMATCH ? "0" : "error"));
}

PG_FUNCTION_INFO_V1(rx_prefix);
Datum
rx_prefix(PG_FUNCTION_ARGS)
{
	char	   *pat = cstr(PG_GETARG_TEXT_PP(0));
	regex_t		re;
	pg_wchar   *w;
	size_t		n;
	char	   *out;
	int			rc;

	if (!compile_sift(&re, pat, PG_GETARG_INT32(1)))
		PG_RETURN_TEXT_P(cstring_to_text("error"));
	rc = pg_regprefix(&re, &w, &n);
	if (rc != REG_PREFIX && rc != REG_EXACT)
		PG_RETURN_TEXT_P(cstring_to_text("none"));
	out = palloc(n * 4 + 3);
	memcpy(out, rc == REG_EXACT ? "E:" : "P:", 2);
	out[2 + pg_wchar2mb_with_len(w, out + 2, n)] = 0;
	PG_RETURN_TEXT_P(cstring_to_text(out));
}
