/*
 * col/gorder_opt.c - the order_layout option: the layout of the order array of each column, and the one place that chooses it where the option does not say. Portable (core/host.h only).
 */

#include "col/gorder.h"
#include "col/preagg.h"

/* The layout a column with no order_layout (or 'auto') gets, from the rows of the segment being written: each chunk of the middle of its order compact where that is at least a fifth smaller
 * than the whole one (sift_cgord_build decides a chunk at a time; a segment with no such chunk is the whole order). One line to change when the numbers say another default. */
#define AUTO_LAYOUT(nvals)	SIFT_CGORD_L_AUTO

int
sift_cgord_layout_pick(uint8 want, uint32 nvals)
{
	if (want == SIFT_CGORD_OL_WHOLE)
		return SIFT_CGORD_L_WHOLE;
	if (want == SIFT_CGORD_OL_COMPACT)
		return SIFT_CGORD_L_COMPACT;
	return AUTO_LAYOUT(nvals);
}

static const char *
trim(const char *p, size_t *len)
{
	while (*p == ' ')
		p++;
	*len = strlen(p);
	while (*len > 0 && p[*len - 1] == ' ')
		(*len)--;
	return p;
}

static const char *
layout_word(const char *p, size_t len, uint8 *layout)
{
	if (len == 4 && strncmp(p, "auto", len) == 0)
		*layout = SIFT_CGORD_OL_AUTO;
	else if (len == 5 && strncmp(p, "whole", len) == 0)
		*layout = SIFT_CGORD_OL_WHOLE;
	else if (len == 7 && strncmp(p, "compact", len) == 0)
		*layout = SIFT_CGORD_OL_COMPACT;
	else if (len == 8 && strncmp(p, "sequence", len) == 0)
		return "the layout sequence is not built in this version";
	else
		return psprintf("unknown layout \"%.*s\" (auto, whole or compact)", (int) len, p);
	return NULL;
}

/*
 * 'whole', 'compact' or 'auto' for every column that can have an order array, or 'compact: a, b; whole: c' for the columns it names (the rest are 'auto'). want[c] gets the layout of
 * column c (SIFT_CGORD_OL_*; zeroed by the caller). s NULL: just the syntax. Returns NULL, or why the value is not valid.
 */
const char *
sift_cgord_layout_parse(const char *opt, const SiftColSchema *s, const char *const *names, uint8 *want)
{
	char	   *copy;
	char	   *item;
	bool		seen[SIFT_COL_MAX_COLS] = {false};
	size_t		len;
	const char *p = trim(opt ? opt : "", &len);
	uint8		layout;
	const char *err;

	if (len == 0)
		return NULL;
	if (strchr(p, ':') == NULL)
	{
		if ((err = layout_word(p, len, &layout)) != NULL)
			return err;
		for (int c = 0; s != NULL && c < s->ncols; c++)
			if (sift_cgord_leaf_ok(s, &s->cols[c]))
				want[c] = layout;
		return NULL;
	}
	copy = pstrdup(p);
	for (item = strtok(copy, ";"); item != NULL; item = strtok(NULL, ";"))
	{
		char	   *colon = strchr(item, ':');
		bool		on[SIFT_COL_MAX_COLS] = {false};
		const char *list;

		if (colon == NULL)
			return "expected 'layout: column, column; layout: column'";
		*colon = '\0';
		p = trim(item, &len);
		if ((err = layout_word(p, len, &layout)) != NULL)
			return err;
		list = trim(colon + 1, &len);
		if (len == 0)
			return "empty column list";
		if ((err = sift_cpagg_leaf_list(list, s, names, on, sift_cgord_leaf_ok, "not an integer, date or timestamp column")) != NULL)
			return err;
		for (int c = 0; s != NULL && c < s->ncols; c++)
			if (on[c])
			{
				if (seen[c])
					return psprintf("column \"%s\" is in two layouts", names[c]);
				seen[c] = true;
				want[c] = layout;
			}
	}
	return NULL;
}
