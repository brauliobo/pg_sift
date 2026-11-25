/*
 * bm25/analyze/index.c - the analyzer configuration of an index of PostgreSQL: its key columns name the sections of the text.
 */
#include "postgres.h"

#include <ctype.h>

#include "lib/stringinfo.h"
#include "utils/rel.h"

#include "bm25/analyze/analyze.h"

#define SIFT_FIELD_NAME_BYTES	16	/* of a field named after a column */

static bool
field_taken(const char *names, const char *name, int len)
{
	for (const char *s = names; *s;)
	{
		const char *e = strchr(s, ',') ? strchr(s, ',') : s + strlen(s);

		if (e - s == len && strncmp(s, name, len) == 0)
			return true;
		s = *e ? e + 1 : e;
	}
	return false;
}

/* A column name as a field name: lowercase letters, digits and underscores; f<k> for one that has no letter or digit, or that an earlier field has */
static void
add_field_name(StringInfo names, const char *attname, int k)
{
	char		name[SIFT_FIELD_NAME_BYTES + 1];
	int			n = 0;
	bool		word = false;

	for (const char *c = attname; *c && n < SIFT_FIELD_NAME_BYTES; c++)
	{
		bool		ascii = isalnum((unsigned char) *c) && !(*c & 0x80);

		word |= ascii;
		name[n++] = ascii ? tolower((unsigned char) *c) : '_';
	}
	if (n > 0 && isdigit((unsigned char) name[0]))
	{
		memmove(name + 1, name, Min(n, SIFT_FIELD_NAME_BYTES - 1));
		name[0] = '_';
		n = Min(n + 1, SIFT_FIELD_NAME_BYTES);
	}
	if (!word || field_taken(names->data, name, n))
		n = snprintf(name, sizeof(name), "f%d", k + 1);
	while (field_taken(names->data, name, n))
		name[n++] = '_';
	if (names->len > 0)
		appendStringInfoChar(names, ',');
	appendBinaryStringInfo(names, name, n);
}

/*
 * The sections of an index of several key columns are its columns, named as they are (fields = '...' names them otherwise, all of them); the
 * names are what a query says: title:word searches the section of the column title.
 */
static const char *
key_fields(Relation index, const char *explicit, StringInfo names)
{
	int			nkey = IndexRelationGetNumberOfKeyAttributes(index);
	int			nnamed = *explicit ? 1 : 0;

	for (const char *c = explicit; *c; c++)
		nnamed += *c == ',';
	if (nnamed > 0)
	{
		if (nnamed != nkey)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("sift index \"%s\": fields names %d sections, the index has %d key columns", RelationGetRelationName(index), nnamed, nkey),
					 errhint("Name one field for each key column, or leave fields out to take the column names.")));
		return explicit;
	}
	for (int k = 0; k < nkey; k++)
		add_field_name(names, NameStr(TupleDescAttr(RelationGetDescr(index), k)->attname), k);
	if (names->len >= SIFT_FIELDS_BYTES)
	{
		resetStringInfo(names);
		for (int k = 0; k < nkey; k++)
			appendStringInfo(names, "%sf%d", k ? "," : "", k + 1);
	}
	sift_analyze_validate_fields(names->data);
	return names->data;
}

void
sift_analyzer_config_from_index(Relation index, SiftAnalyzerConfig *cfg)
{
	const SiftOptions *opts = sift_index_options(index);
	StringInfoData names;

	initStringInfo(&names);
	sift_analyzer_config_with_fields(opts, IndexRelationGetNumberOfKeyAttributes(index) > 1 ? key_fields(index, sift_options_fields(opts), &names)
					   : sift_options_fields(opts), cfg);
	pfree(names.data);
}
