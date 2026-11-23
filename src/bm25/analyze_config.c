/*
 * bm25/analyze_config.c - analyzer configuration: options, stemmer names, field names, the configuration hash.
 */

#include <ctype.h>

#include "bm25/analyze.h"

#include "lib/stringinfo.h"
#include "utils/rel.h"

#define SIFT_FIELD_NAME_BYTES	16	/* of a field named after a column */

static const struct
{
	const char *name;
	SiftStemmer id;
}			stem_names[] = {
	{"", SIFT_STEM_NONE}, {"none", SIFT_STEM_NONE},
	{"en", SIFT_STEM_EN}, {"english", SIFT_STEM_EN}, {"english_stem", SIFT_STEM_EN},
};

/* Stemmer names the built-in stemmers answer to; any other name is a text search configuration */
static bool
builtin_stemmer(const char *name, SiftStemmer *id)
{
	for (int i = 0; i < lengthof(stem_names); i++)
		if (pg_strcasecmp(name, stem_names[i].name) == 0)
		{
			*id = stem_names[i].id;
			return true;
		}
	return false;
}

/* Configuration */

void
sift_analyze_validate_fields(const char *names)
{
	int			n = 0;

	for (const char *s = names; *s; n++)
	{
		const char *e = strchr(s, ',') ? strchr(s, ',') : s + strlen(s);

		if (e == s || !(islower((unsigned char) *s) || *s == '_'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid sift field names \"%s\"", names),
					 errhint("Use lowercase identifiers separated by commas.")));
		for (const char *c = s; c < e; c++)
			if (!(islower((unsigned char) *c) || isdigit((unsigned char) *c) || *c == '_'))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid sift field names \"%s\"", names),
						 errhint("Use lowercase identifiers separated by commas.")));
		for (const char *o = names; o < s; o = strchr(o, ',') + 1)
			if (e - s == (strchr(o, ',') - o) && strncmp(o, s, e - s) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("duplicate sift field \"%.*s\"", (int) (e - s), s)));
		s = *e ? e + 1 : e;
	}
	if (n > SIFT_MAX_FIELDS || strlen(names) >= SIFT_FIELDS_BYTES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a sift index has at most %d fields of at most %d bytes of names", SIFT_MAX_FIELDS,
						SIFT_FIELDS_BYTES - 1),
				 errhint("Index fewer sections, or give them shorter names; sift.field_query weighs the ones that remain.")));
}

static void
config_hash(SiftAnalyzerConfig *cfg)
{
	uint8		key[12 + SIFT_TSCONFIG_BYTES] = {SIFT_ANALYZER_VERSION, cfg->case_fold, cfg->accent_fold, cfg->stemmer};
	int			n = 4;

	if (cfg->stemmer == SIFT_STEM_TS)
	{
		memcpy(key + n, cfg->tsconfig, strlen(cfg->tsconfig) + 1);
		n += strlen(cfg->tsconfig) + 1;
	}

	if (cfg->prefix_len)
	{
		key[n++] = 2;
		key[n++] = cfg->prefix_len;
	}
	if (cfg->nfields)
	{
		key[n++] = 3;
		key[n++] = cfg->nfields;
	}
	if (cfg->keyword)
		key[n++] = 4;
	cfg->hash = sift_term_hash((const char *) key, n);
}

void
sift_analyzer_config_set_synthetic(SiftAnalyzerConfig *cfg, int prefix_len, const char *fields)
{
	cfg->prefix_len = prefix_len;
	memset(cfg->field_names, 0, sizeof(cfg->field_names));
	strlcpy(cfg->field_names, fields, sizeof(cfg->field_names));
	cfg->nfields = *fields ? 1 : 0;
	for (const char *c = fields; *c; c++)
		cfg->nfields += *c == ',';
	config_hash(cfg);
}

void
sift_analyzer_config_set_keyword(SiftAnalyzerConfig *cfg, bool keyword)
{
	cfg->keyword = keyword;
	config_hash(cfg);
}

void
sift_analyzer_config_set(SiftAnalyzerConfig *cfg, bool case_fold, bool accent_fold,
						 SiftStemmer stemmer)
{
	cfg->case_fold = case_fold;
	cfg->accent_fold = accent_fold;
	cfg->stemmer = stemmer;
	cfg->tsconfig[0] = '\0';
	cfg->keyword = false;
	sift_analyzer_config_set_synthetic(cfg, 0, "");
}

void
sift_analyzer_config_default(SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_set(cfg, true, true, SIFT_STEM_NONE);
}

void
sift_analyzer_config_set_accent_fold(SiftAnalyzerConfig *cfg, bool accent_fold)
{
	cfg->accent_fold = accent_fold;
	config_hash(cfg);
}

void
sift_analyzer_config_set_stemmer(SiftAnalyzerConfig *cfg, const char *name)
{
	SiftStemmer id;

	if (builtin_stemmer(name, &id))
		cfg->tsconfig[0] = '\0';
	else
	{
		id = SIFT_STEM_TS;
		strlcpy(cfg->tsconfig, name, sizeof(cfg->tsconfig));
	}
	cfg->stemmer = id;
	config_hash(cfg);
}

static void
config_with_fields(const SiftOptions *opts, const char *fields, SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_set(cfg, opts->case_folding, opts->accent_folding, SIFT_STEM_NONE);
	sift_analyzer_config_set_stemmer(cfg, sift_options_stemmer(opts));
	/* a configuration's own dictionaries decide what is folded, as for to_tsvector */
	if (cfg->stemmer == SIFT_STEM_TS && !opts->accent_folding_set)
		sift_analyzer_config_set_accent_fold(cfg, false);
	sift_analyzer_config_set_synthetic(cfg, opts->prefix_index, fields);
	if (opts->analyzer == SIFT_ANALYZER_KEYWORD)
	{
		if (cfg->stemmer != SIFT_STEM_NONE || cfg->prefix_len || cfg->nfields)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("analyzer = keyword cannot be combined with stemmer, prefix_index or fields"),
					 errhint("Use analyzer = 'text' for stemming, prefixes or weighted fields, or drop those options for exact keyword matching.")));
		sift_analyzer_config_set_keyword(cfg, true);
	}
}

void
sift_analyzer_config_from_options(const SiftOptions *opts, SiftAnalyzerConfig *cfg)
{
	config_with_fields(opts, sift_options_fields(opts), cfg);
}

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
	config_with_fields(opts, IndexRelationGetNumberOfKeyAttributes(index) > 1 ? key_fields(index, sift_options_fields(opts), &names)
					   : sift_options_fields(opts), cfg);
	pfree(names.data);
}

SiftStemmer
sift_analyze_stemmer(const char *name)
{
	SiftStemmer id;

	return builtin_stemmer(name, &id) ? id : SIFT_STEM_TS;
}

bool
sift_analyzer_config_agrees(const SiftAnalyzerConfig *a, const SiftAnalyzerConfig *b)
{
	return a->case_fold == b->case_fold && a->accent_fold == b->accent_fold && a->stemmer == b->stemmer && strcmp(a->tsconfig, b->tsconfig) == 0 &&
		a->keyword == b->keyword && strcmp(a->field_names, b->field_names) == 0;
}
