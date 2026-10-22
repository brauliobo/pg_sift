/*
 * col/codec_policy.c - the compression policy of a column: the parser of the reloption and its canonical name (docs/COL-CODEC.md).
 */
#include "core/host.h"

#include "col/codec_int.h"

static const struct
{
	const char *name;
	int			tag;
}			cc_enc_names[] = {{"for", SIFT_CCN_FORBIT}, {"delta", SIFT_CCN_DELTA}, {"rle", SIFT_CCN_RLE}, {"dict", SIFT_CCN_DICT}, {"alp", SIFT_CCN_ALP}, {"scale", SIFT_CCN_SCALE}};

void
sift_cc_policy_default(SiftCcPolicy *p)
{
	memset(p, 0, sizeof(*p));
	p->mode = SIFT_CCM_AUTO;
	p->alg = SIFT_CC_ALG_AUTO;
}

/* NULL: not an algorithm; "": taken; else an error message */
static const char *
parse_alg(const char *tok, SiftCcPolicy *p)
{
	const char *colon = strchr(tok, ':');
	size_t		len = colon ? (size_t) (colon - tok) : strlen(tok);
	int			alg = len == 3 && !strncmp(tok, "lz4", 3) ? SIFT_CC_ALG_LZ4 : len == 4 && !strncmp(tok, "zstd", 4) ? SIFT_CC_ALG_ZSTD : 0;
	int			max = alg == SIFT_CC_ALG_LZ4 ? 12 : 22;
	long		level = 0;

	if (!alg)
		return NULL;
	if (p->mode == SIFT_CCM_LAYER)
		return "more than one general-purpose algorithm";
	if (colon)
	{
		char	   *end;

		level = strtol(colon + 1, &end, 10);
		if (end == colon + 1 || *end || level < 1 || level > max)
			return alg == SIFT_CC_ALG_LZ4 ? "lz4 level must be a number between 1 and 12" : "zstd level must be a number between 1 and 22";
	}
	p->alg = (uint8) alg;
	p->level = (uint8) level;
	p->mode = SIFT_CCM_LAYER;
	return "";
}

const char *
sift_cc_policy_parse(const char *spec, SiftCcPolicy *p)
{
	char	   *buf = pstrdup(spec),
			   *tok,
			   *save = NULL;
	const char *err = NULL;
	int			ntok = 0;

	sift_cc_policy_default(p);
	for (tok = strtok_r(buf, "+", &save); tok && !err; tok = strtok_r(NULL, "+", &save), ntok++)
	{
		const char *r;
		bool		found = false;
		char	   *end;

		while (*tok == ' ')
			tok++;
		for (end = tok + strlen(tok); end > tok && end[-1] == ' ';)
			*--end = '\0';
		for (size_t i = 0; i < lengthof(cc_enc_names); i++)
			if (!strcmp(tok, cc_enc_names[i].name))
			{
				p->explicit_encs = 1;
				p->encs |= CC_BIT(cc_enc_names[i].tag);
				found = true;
			}
		if (found || (!strcmp(tok, "auto") && ntok == 0))
			continue;
		if (!strcmp(tok, "none") && ntok == 0)
		{
			p->mode = SIFT_CCM_NONE;
			p->alg = SIFT_CC_ALG_NONE;
			continue;
		}
		r = parse_alg(tok, p);
		if (r == NULL)
			err = "unknown compression: expected auto, none, lz4[:N], zstd[:N] or for, delta, rle, dict, alp joined by '+'";
		else if (*r)
			err = r;
	}
	if (!err && ntok == 0)
		err = "empty compression";
	if (!err && p->mode == SIFT_CCM_NONE && ntok > 1)
		err = "none cannot be combined with other compression settings";
	if (!err && p->explicit_encs && p->mode == SIFT_CCM_AUTO)
		p->alg = SIFT_CC_ALG_NONE;
	pfree(buf);
	return err;
}

const char *
sift_cc_policy_for_column(const char *spec, const char *colname, SiftCcPolicy *p)
{
	char	   *buf = pstrdup(spec),
			   *ent,
			   *save = NULL;
	const char *err = NULL;
	bool		have_default = false;

	sift_cc_policy_default(p);
	for (ent = strtok_r(buf, ";", &save); ent && !err; ent = strtok_r(NULL, ";", &save))
	{
		char	   *eq;

		while (*ent == ' ')
			ent++;
		eq = strchr(ent, '=');
		if (!eq)
		{
			if (have_default)
				err = "more than one default compression";
			else
			{
				err = sift_cc_policy_parse(ent, p);
				have_default = true;
			}
		}
		else
		{
			size_t		l = (size_t) (eq - ent);

			while (l && ent[l - 1] == ' ')
				l--;
			if (l == strlen(colname) && !strncmp(ent, colname, l))
			{
				err = sift_cc_policy_parse(eq + 1, p);
				pfree(buf);
				return err;
			}
		}
	}
	pfree(buf);
	return err;
}

char *
sift_cc_policy_name(const SiftCcPolicy *p)
{
	StringInfoData s;

	if (p->mode == SIFT_CCM_NONE)
		return pstrdup("none");
	initStringInfo(&s);
	if (p->explicit_encs)
		for (size_t i = 0; i < lengthof(cc_enc_names); i++)
			if (p->encs & CC_BIT(cc_enc_names[i].tag))
				appendStringInfo(&s, "%s%s", s.len ? "+" : "", cc_enc_names[i].name);
	if (p->mode == SIFT_CCM_LAYER)
	{
		appendStringInfo(&s, "%s%s", s.len ? "+" : "", p->alg == SIFT_CC_ALG_LZ4 ? "lz4" : "zstd");
		if (p->level)
			appendStringInfo(&s, ":%d", p->level);
	}
	if (!s.len)
		appendStringInfoString(&s, "auto");
	return s.data;
}
