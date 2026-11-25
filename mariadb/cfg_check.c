/*
 * cfg_check.c - what the definition of an index may say about the SQL the library runs for it. The queue and the table are
 * read through a local connection of the server, which has no ACL of its own, so the definition is not text to be trusted:
 * each SQL key is accepted only in the exact form sift_attach writes it, built from the database, the table and the name
 * of the index, and the text expression only as a composition of the columns of the table (alias t), literals and pure
 * functions. A sub-select, a statement separator, a comment, a variable or a function with side effects cannot be spelled.
 */
#include "cfg_check.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_IDENT		192
#define MAX_EXPR		4096
#define MAX_FORM		1024

const char *const cfg_meta_names[NMETA] = {"queue", "keyexpr", "source", "textexpr", "database", "table"};
enum {QUEUE, KEYEXPR, SOURCE, TEXTEXPR, DATABASE, TABLE};

char *
cfg_value(const char *cfg, const char *key)
{
	size_t		kl = strlen(key);

	for (const char *p = cfg; *p;)
	{
		const char *nl = strchr(p, '\n');
		size_t		len = nl ? (size_t) (nl - p) : strlen(p);

		if (len > kl && strncmp(p, key, kl) == 0 && p[kl] == '=')
			return strndup(p + kl + 1, len - kl - 1);
		p += len + (nl != NULL);
	}
	return NULL;
}

static const char *const functions[] = {
	"concat", "concat_ws", "ifnull", "coalesce", "nullif", "if", "lower", "upper", "lcase", "ucase", "trim", "ltrim", "rtrim",
	"substring", "substr", "mid", "left", "right", "replace", "regexp_replace", "char", "length", "char_length", "cast", "convert",
	"json_value", "json_unquote", "json_extract", "lpad", "rpad", "repeat", "reverse", "hex", "unhex", "date_format", "format",
};

static const char *const keywords[] = {
	"null", "is", "not", "and", "or", "as", "using", "case", "when", "then", "else", "end", "char", "signed", "unsigned",
	"integer", "decimal", "date", "datetime", "time", "binary", "utf8mb4", "utf8mb3", "utf8", "latin1", "ascii",
};

#define IN_LIST(list, word, len) in_list(list, sizeof(list) / sizeof(*(list)), word, len)

static bool
in_list(const char *const *list, size_t n, const char *word, size_t len)
{
	for (size_t i = 0; i < n; i++)
		if (strlen(list[i]) == len && strncasecmp(list[i], word, len) == 0)
			return true;
	return false;
}

static bool
word_char(char c)
{
	return isalnum((unsigned char) c) || c == '_' || c == '$';
}

/* A name as the server stores it: no backtick (it could close the quotes), no control character */
static bool
ident_ok(const char *s, size_t len)
{
	if (len == 0 || len > MAX_IDENT)
		return false;
	for (size_t i = 0; i < len; i++)
		if (s[i] == '`' || (unsigned char) s[i] < 0x20 || s[i] == 0x7f)
			return false;
	return true;
}

static bool
name_ok(const char *name)
{
	size_t		len = strlen(name);

	if (len == 0 || len > 63)
		return false;
	for (size_t i = 0; i < len; i++)
		if (!isalnum((unsigned char) name[i]) && name[i] != '_')
			return false;
	return true;
}

/* ------------------------------------------------ the text expression */

static const char *
skip_blank(const char *p)
{
	while (isspace((unsigned char) *p))
		p++;
	return p;
}

/* After the opening quote: the end of the literal, or NULL; a backslash is refused (its meaning depends on sql_mode) */
static const char *
literal_end(const char *p)
{
	for (; *p; p++)
	{
		if (*p == '\\')
			return NULL;
		if (*p == '\'')
		{
			if (p[1] != '\'')
				return p + 1;
			p++;
		}
	}
	return NULL;
}

/* t.col or t.`col`: the end of the reference, or NULL */
static const char *
column_end(const char *p)
{
	const char *q = p + 2;

	if (*q == '`')
	{
		const char *close = strchr(q + 1, '`');

		return close && ident_ok(q + 1, close - q - 1) ? close + 1 : NULL;
	}
	while (word_char(*q))
		q++;
	return q > p + 2 ? q : NULL;
}

/* A word: a function when a parenthesis follows, else a keyword; the end of the token, or NULL */
static const char *
word_end(const char *p)
{
	const char *q = p;

	while (word_char(*q))
		q++;
	if (*skip_blank(q) == '(' ? !IN_LIST(functions, p, q - p) : !IN_LIST(keywords, p, q - p))
		return NULL;
	return q;
}

static const char *
number_end(const char *p)
{
	while (isdigit((unsigned char) *p) || *p == '.')
		p++;
	return p;
}

/* The end of the token at p, or NULL when it is not one the expression may use; parentheses are the caller's */
static const char *
token_end(const char *p)
{
	if (*p == '\'')
		return literal_end(p + 1);
	if (p[0] == 't' && p[1] == '.')
		return column_end(p);
	if (isdigit((unsigned char) *p))
		return number_end(p);
	if (isalpha((unsigned char) *p))
		return word_end(p);
	if ((*p == '-' && p[1] == '-') || (*p == '/' && p[1] == '*'))
		return NULL;
	if (*p == '!' && p[1] == '=')
		return p + 2;
	return *p && strchr(",+-*/=<>", *p) ? p + 1 : NULL;
}

static bool
expr_ok(const char *s)
{
	const char *p = skip_blank(s);
	int			depth = 0;

	if (*p == '\0' || strlen(s) > MAX_EXPR)
		return false;
	while (*p)
	{
		const char *end = p + 1;

		if (*p == '(')
			depth++;
		else if (*p == ')')
			depth--;
		else
			end = token_end(p);
		if (end == NULL || depth < 0)
			return false;
		p = skip_blank(end);
	}
	return depth == 0;
}

/* ------------------------------------------------ the forms of the other keys */

/* `col` followed by the rest: the length of the quoted name, or 0 */
static size_t
quoted_len(const char *p)
{
	const char *close = p[0] == '`' ? strchr(p + 1, '`') : NULL;

	return close && ident_ok(p + 1, close - p - 1) ? (size_t) (close - p - 1) : 0;
}

/* t.`a` = k.`a` AND t.`b` = k.`b`: the join of a key table with the table on its key columns */
static bool
join_ok(const char *p)
{
	for (;;)
	{
		size_t		len;
		char		on[MAX_FORM];

		if (strncmp(p, "t.", 2) != 0 || (len = quoted_len(p + 2)) == 0)
			return false;
		snprintf(on, sizeof(on), "t.`%.*s` = k.`%.*s`", (int) len, p + 3, (int) len, p + 3);
		if (strncmp(p, on, strlen(on)) != 0)
			return false;
		p += strlen(on);
		if (*p == '\0')
			return true;
		if (strncmp(p, " AND ", 5) != 0)
			return false;
		p += 5;
	}
}

static bool
key_source_ok(const char *name, char *const *v)
{
	char		want[MAX_FORM];
	size_t		keylen = strncmp(v[KEYEXPR], "t.", 2) == 0 ? quoted_len(v[KEYEXPR] + 2) : 0;

	if (strcmp(v[KEYEXPR], "k.kid") == 0)
	{
		int			n = snprintf(want, sizeof(want), "`%s`.`sift_k_%s` k JOIN `%s`.`%s` t ON ", v[DATABASE], name, v[DATABASE],
								 v[TABLE]);

		return n < (int) sizeof(want) && strncmp(v[SOURCE], want, n) == 0 && join_ok(v[SOURCE] + n);
	}
	snprintf(want, sizeof(want), "`%s`.`%s` t", v[DATABASE], v[TABLE]);
	return keylen > 0 && v[KEYEXPR][2 + keylen + 2] == '\0' && strcmp(v[SOURCE], want) == 0;
}

static const char *
def_error(const char *name, char *const *v, int found)
{
	char		want[MAX_FORM];

	if (!name_ok(name))
		return "sift: an index name is 1 to 63 letters, digits or underscores";
	if (found == 0)
		return NULL;
	if (found != NMETA)
		return "sift: the definition of an index has queue, keyexpr, source, textexpr, database and table, or none of them";
	if (!ident_ok(v[DATABASE], strlen(v[DATABASE])) || !ident_ok(v[TABLE], strlen(v[TABLE])))
		return "sift: the database and the table of an index are names without backticks";
	snprintf(want, sizeof(want), "`%s`.`sift_q_%s`", v[DATABASE], name);
	if (strcmp(v[QUEUE], want) != 0)
		return "sift: the queue of an index is sift_q_<index> in its database";
	if (!key_source_ok(name, v))
		return "sift: the key and the source of an index are in the forms sift_attach writes";
	if (!expr_ok(v[TEXTEXPR]))
		return "sift: the text expression of an index combines the columns of the table (t.col), literals and string functions";
	return NULL;
}

bool
cfg_check(const char *name, const char *cfg, char *err, size_t errlen)
{
	char	   *v[NMETA];
	int			found = 0;
	const char *why;

	for (int i = 0; i < NMETA; i++)
		found += (v[i] = cfg_value(cfg, cfg_meta_names[i])) != NULL;
	why = def_error(name, v, found);
	for (int i = 0; i < NMETA; i++)
		free(v[i]);
	if (why)
		snprintf(err, errlen, "%s", why);
	return why == NULL;
}
