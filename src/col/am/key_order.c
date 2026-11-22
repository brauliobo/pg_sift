/*
 * col/am/key_order.c - the text of the key_order option: a list of at most SIFT_KEY_ORDER_MAX columns, each "name [ASC | DESC] [NULLS FIRST | NULLS LAST]" as ORDER BY writes them.
 */
#include "postgres.h"

#include "col/am.h"
#include "col/v1/preagg.h"
#include "core/range.h"

#define SYNTAX "a list of column names, each with an optional ASC or DESC and NULLS FIRST or NULLS LAST ('group, key' for two)"

static bool
word_is(const char *w, size_t n, const char *word)
{
	return strlen(word) == n && pg_strncasecmp(w, word, n) == 0;
}

/* The last word of [start, end), a run of letters that a blank precedes: its place in *w and *n, and where the text before it ends (without the blanks) in *before */
static bool
last_word(const char *start, const char *end, const char **w, size_t *n, const char **before)
{
	const char *b = end;

	while (b > start && isalpha((unsigned char) b[-1]))
		b--;
	if (b == end || b == start || !isspace((unsigned char) b[-1]))
		return false;
	*w = b;
	*n = end - b;
	for (*before = b; *before > start && isspace((unsigned char) (*before)[-1]); (*before)--)
		;
	return true;
}

/* One item, [start, end) of the text: the modifiers off its end, then the name */
static bool
item_parse(const char *start, const char *end, SiftKeyOrderItem *it)
{
	const char *w,
			   *before,
			   *p;
	size_t		n;
	bool		desc = false,
				nulls_first = false,
				nulls_said = false,
				whole;
	char	   *text;

	while (end > start && isspace((unsigned char) end[-1]))
		end--;
	if (last_word(start, end, &w, &n, &before) && (word_is(w, n, "first") || word_is(w, n, "last")))
	{
		nulls_first = word_is(w, n, "first");
		nulls_said = true;
		if (!last_word(start, before, &w, &n, &end) || !word_is(w, n, "nulls"))
			return false;
	}
	if (last_word(start, end, &w, &n, &before) && (word_is(w, n, "asc") || word_is(w, n, "desc")))
	{
		desc = word_is(w, n, "desc");
		end = before;
	}
	text = pnstrdup(start, end - start);
	p = text;
	it->name = sift_cpagg_name_take(&p, "");
	it->flags = (desc ? SIFT_KEY_DESC : 0) | ((nulls_said ? nulls_first : desc) ? SIFT_KEY_NULLS_FIRST : 0);
	whole = it->name[0] != '\0' && *p == '\0';
	pfree(text);
	return whole;
}

const char *
sift_key_order_parse(const char *text, SiftKeyOrderItem *items, int *n)
{
	const char *p = text;

	*n = 0;
	for (;;)
	{
		const char *start = p;
		bool		quoted = false;

		for (; *p != '\0' && (quoted || *p != ','); p++)
			quoted ^= *p == '"';
		if (*n == SIFT_KEY_ORDER_MAX)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("key_order lists more than %d columns", SIFT_KEY_ORDER_MAX),
							errdetail("SIFT_KEY_ORDER_MAX is %d.", SIFT_KEY_ORDER_MAX)));
		if (!item_parse(start, p, &items[*n]))
			return SYNTAX;
		(*n)++;
		if (*p == '\0')
			return NULL;
		p++;
	}
}
