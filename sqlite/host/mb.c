/*
 * mb.c - the multibyte helpers of the PostgreSQL backend (UTF-8 only) and its string functions.
 */
#include "host.h"
#include "utf8.h"

/* ---------- multibyte: UTF-8 ---------- */

int
GetDatabaseEncoding(void)
{
	return PG_UTF8;
}

const char *
GetDatabaseEncodingName(void)
{
	return "UTF8";
}

int
pg_database_encoding_max_length(void)
{
	return 4;
}

static int
utf8_len(unsigned char c)
{
	return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
}

int
pg_mblen(const char *s)
{
	return utf8_len((unsigned char) *s);
}

int
pg_mblen_range(const char *s, const char *end)
{
	return Min(utf8_len((unsigned char) *s), (int) (end - s));
}

int
pg_mbstrlen_with_len(const char *s, int len)
{
	int			n = 0;

	for (int i = 0; i < len; i += pg_mblen(s + i))
		n++;
	return n;
}

int
pg_mbcliplen(const char *s, int len, int limit)
{
	int			n = 0;

	while (n < len)
	{
		int			l = pg_mblen(s + n);

		if (n + l > limit || n + l > len)
			break;
		n += l;
	}
	return n;
}

int
pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len)
{
	int			n = 0;

	while (len > 0 && *from)
	{
		unsigned char c = (unsigned char) *from;
		int			l = Min(utf8_len(c), len);

		if (l == 1)
			to[n++] = c;
		else if (l == 2)
			to[n++] = ((c & 0x1F) << 6) | (from[1] & 0x3F);
		else if (l == 3)
			to[n++] = ((c & 0x0F) << 12) | ((from[1] & 0x3F) << 6) | (from[2] & 0x3F);
		else
			to[n++] = ((c & 0x07) << 18) | ((from[1] & 0x3F) << 12) | ((from[2] & 0x3F) << 6) | (from[3] & 0x3F);
		from += l;
		len -= l;
	}
	to[n] = 0;
	return n;
}

int
pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len)
{
	char	   *p = to;

	for (; len > 0 && *from; len--, from++)
		p = host_utf8_emit(p, *from);
	*p = '\0';
	return p - to;
}

/* no overlongs, surrogates or code points above U+10FFFF, as PostgreSQL */
static int
utf8_valid_len(const unsigned char *s, int len)
{
	unsigned char c = s[0];
	int			l = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;

	if (l == 0 || l > len)
		return 0;
	for (int i = 1; i < l; i++)
		if ((s[i] & 0xC0) != 0x80)
			return 0;
	if (l == 3 && ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] > 0x9F)))
		return 0;
	if (l == 4 && ((c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] > 0x8F)))
		return 0;
	return l;
}

void
pg_verify_mbstr(int encoding, const char *s, int len, bool noError)
{
	for (int i = 0; i < len;)
	{
		int			l = utf8_valid_len((const unsigned char *) s + i, len - i);

		if (l == 0)
		{
			if (noError)
				return;
			ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE), errmsg("invalid byte sequence for encoding \"UTF8\"")));
		}
		i += l;
	}
}

/* ---------- strings ---------- */

int
pg_strcasecmp(const char *s1, const char *s2)
{
	return strcasecmp(s1, s2);
}

int
pg_strncasecmp(const char *s1, const char *s2, size_t n)
{
	return strncasecmp(s1, s2, n);
}

/* PostgreSQL prints the shortest digits that read back as the same float (Ryu), in %g like form with E exponents */
int
float_to_shortest_decimal_buf(float f, char *result)
{
	for (int prec = 1; prec <= 9; prec++)
	{
		snprintf(result, 32, "%.*g", prec, (double) f);
		if (strtof(result, NULL) == f)
			break;
	}
	for (char *p = result; *p; p++)
		if (*p == 'e')
			*p = 'E';
	return strlen(result);
}
