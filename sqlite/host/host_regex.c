/*
 * host_regex.c - PostgreSQL's regex API (pg_regcomp, pg_regexec) over the C library's POSIX extended regular
 * expressions. Terms and patterns are converted back to UTF-8 and matched bytewise in the C locale: the
 * query language of sift (/regex/ leaves) works for ASCII terms and patterns, not with PostgreSQL's ARE extensions.
 */
#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the one thing of the PostgreSQL side we need: regex_t holds a pointer */
typedef struct pgre
{
	regex_t	   *posix;
	int			magic;
} pgre;

typedef uint32_t pg_wchar;

#define PG_REG_OKAY		0
#define PG_REG_NOMATCH	1
#define PG_REG_BADPAT	2

static char *
to_utf8(const pg_wchar *w, size_t len)
{
	char	   *s = malloc(len * 4 + 1),
			   *p = s;

	for (size_t i = 0; i < len; i++)
	{
		pg_wchar	c = w[i];

		if (c < 0x80)
			*p++ = c;
		else if (c < 0x800)
		{
			*p++ = 0xC0 | (c >> 6);
			*p++ = 0x80 | (c & 0x3F);
		}
		else if (c < 0x10000)
		{
			*p++ = 0xE0 | (c >> 12);
			*p++ = 0x80 | ((c >> 6) & 0x3F);
			*p++ = 0x80 | (c & 0x3F);
		}
		else
		{
			*p++ = 0xF0 | (c >> 18);
			*p++ = 0x80 | ((c >> 12) & 0x3F);
			*p++ = 0x80 | ((c >> 6) & 0x3F);
			*p++ = 0x80 | (c & 0x3F);
		}
	}
	*p = '\0';
	return s;
}

/* the query parser anchors a pattern as ^(?:p)$; POSIX has no non-capturing groups */
static void
strip_noncapturing(char *s)
{
	for (char *p; (p = strstr(s, "(?:")) != NULL;)
		memmove(p + 1, p + 3, strlen(p + 3) + 1);
}

int
pg_regcomp(void *re, const pg_wchar *pattern, size_t len, int cflags, unsigned int collation)
{
	pgre	   *r = re;
	char	   *s = to_utf8(pattern, len);
	int			rc;

	strip_noncapturing(s);
	r->posix = malloc(sizeof(regex_t));
	rc = regcomp(r->posix, s, REG_EXTENDED | REG_NOSUB);
	free(s);
	if (rc != 0)
	{
		free(r->posix);
		r->posix = NULL;
	}
	r->magic = rc;
	return rc == 0 ? PG_REG_OKAY : PG_REG_BADPAT;
}

int
pg_regexec(void *re, const pg_wchar *data, size_t len, size_t search_start, void *details, size_t nmatch,
		   void *pmatch, int eflags)
{
	pgre	   *r = re;
	char	   *s = to_utf8(data, len);
	int			rc = regexec(r->posix, s, 0, NULL, 0);

	free(s);
	return rc == 0 ? PG_REG_OKAY : PG_REG_NOMATCH;
}

size_t
pg_regerror(int errcode, const void *re, char *errbuf, size_t errbuf_size)
{
	const pgre *r = re;

	return regerror(r && r->magic ? r->magic : REG_BADPAT, NULL, errbuf, errbuf_size);
}

void
pg_regfree(void *re)
{
	pgre	   *r = re;

	if (r->posix)
	{
		regfree(r->posix);
		free(r->posix);
		r->posix = NULL;
	}
}

/* no literal prefix is derived: a regex leaf scans the dictionary blocks of the whole index */
int
pg_regprefix(void *re, pg_wchar **string, size_t *slength)
{
	return PG_REG_NOMATCH;
}
