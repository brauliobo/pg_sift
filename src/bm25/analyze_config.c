/*
 * bm25/analyze_config.c - analyzer configuration: options, stemmer names, field names, the configuration hash.
 */
#include "sift_host.h"

#include <ctype.h>

#include "bm25/analyze.h"

static const struct
{
	const char *name;
	SiftStemmer id;
}			stem_names[] = {
	{"", SIFT_STEM_NONE}, {"none", SIFT_STEM_NONE},
	{"en", SIFT_STEM_EN}, {"english", SIFT_STEM_EN}, {"english_stem", SIFT_STEM_EN},
};

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
						SIFT_FIELDS_BYTES - 1)));
}

static void
config_hash(SiftAnalyzerConfig *cfg)
{
	uint8		key[12] = {SIFT_ANALYZER_VERSION, cfg->case_fold, cfg->accent_fold, cfg->stemmer};
	int			n = 4;

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
	cfg->keyword = false;
	sift_analyzer_config_set_synthetic(cfg, 0, "");
}

void
sift_analyzer_config_default(SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_set(cfg, true, true, SIFT_STEM_NONE);
}

void
sift_analyzer_config_from_options(const SiftOptions *opts, SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_set(cfg, opts->case_folding, opts->accent_folding,
							 sift_analyze_stemmer(sift_options_stemmer(opts)));
	sift_analyzer_config_set_synthetic(cfg, opts->prefix_index, sift_options_fields(opts));
	if (opts->analyzer == SIFT_ANALYZER_KEYWORD)
	{
		if (cfg->stemmer != SIFT_STEM_NONE || cfg->prefix_len || cfg->nfields)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("analyzer = keyword cannot be combined with stemmer, prefix_index or fields")));
		sift_analyzer_config_set_keyword(cfg, true);
	}
}

void
sift_analyzer_config_from_index(Relation index, SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_from_options(sift_index_options(index), cfg);
}

SiftStemmer
sift_analyze_stemmer(const char *name)
{
	for (int i = 0; i < lengthof(stem_names); i++)
		if (pg_strcasecmp(name, stem_names[i].name) == 0)
			return stem_names[i].id;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unknown sift stemmer \"%s\"", name),
			 errhint("Valid stemmers are \"en\" and \"none\".")));
	pg_unreachable();
}

void
sift_analyze_validate_stemmer(const char *name)
{
	sift_analyze_stemmer(name);
}

bool
sift_analyzer_config_agrees(const SiftAnalyzerConfig *a, const SiftAnalyzerConfig *b)
{
	return a->case_fold == b->case_fold && a->accent_fold == b->accent_fold && a->stemmer == b->stemmer &&
		a->keyword == b->keyword && strcmp(a->field_names, b->field_names) == 0;
}
