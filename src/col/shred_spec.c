/*
 * col/shred_spec.c - the path spec of sift_col: its parser into the node tree and its canonical form (docs/COL-SHRED.md).
 */
#include "core/host.h"

#include <string.h>

#include "bm25/analyze.h"
#include "col/shred_int.h"

#define SPEC_ERROR(code, loc, ...) \
	ereport(ERROR, (errcode(code), errmsg("sift shred paths: %s at position %d", psprintf(__VA_ARGS__), (loc))))
#define SPEC_SYNTAX(loc, ...)	SPEC_ERROR(ERRCODE_SYNTAX_ERROR, loc, __VA_ARGS__)
#define SPEC_INVALID(loc, ...)	SPEC_ERROR(ERRCODE_INVALID_PARAMETER_VALUE, loc, __VA_ARGS__)
#define SPEC_LIMIT(loc, ...)	SPEC_ERROR(ERRCODE_PROGRAM_LIMIT_EXCEEDED, loc, __VA_ARGS__)

typedef enum TokKind
{
	T_END,
	T_WORD,						/* bare or quoted key, type name, number */
	T_DOT,
	T_ARROW,
	T_LIST,						/* [] */
	T_STAR,
	T_LBRACE,
	T_RBRACE,
	T_COMMA,
	T_LPAREN,
	T_RPAREN
} TokKind;

typedef enum StepKind
{
	S_KEY,
	S_LIST,
	S_WILD
} StepKind;

typedef struct Step
{
	StepKind	kind;
	char	   *key;
	int			len;
	int			loc;
} Step;

typedef struct Parser
{
	const char *src;
	int			pos;
	TokKind		tok;
	int			loc;			/* 1-based position of tok */
	char	   *word;
	bool		quoted;
	SiftShredNode *root;
	SiftShredColDef *cols;
	int			ncols;
	SiftShredGroupDef *groups;
	int			ngroups;
	StringInfoData canonical;
} Parser;

static bool
bare_char(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

static void
lex(Parser *P)
{
	const char *s = P->src;
	int			p = P->pos;

	while (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')
		p++;
	P->loc = p + 1;
	P->word = NULL;
	P->quoted = false;
	switch (s[p])
	{
		case '\0':
			P->tok = T_END;
			break;
		case ',':
			P->tok = T_COMMA, p++;
			break;
		case '{':
			P->tok = T_LBRACE, p++;
			break;
		case '}':
			P->tok = T_RBRACE, p++;
			break;
		case '(':
			P->tok = T_LPAREN, p++;
			break;
		case ')':
			P->tok = T_RPAREN, p++;
			break;
		case '*':
			P->tok = T_STAR, p++;
			break;
		case '.':
			P->tok = T_DOT, p++;
			break;
		case '-':
			if (s[p + 1] != '>')
				SPEC_SYNTAX(p + 1, "unexpected \"-\" (a separator is \".\" or \"->\"; quote keys that contain it)");
			P->tok = T_ARROW, p += 2;
			break;
		case '[':
			p++;
			while (s[p] == ' ' || s[p] == '\t')
				p++;
			if (s[p] != ']')
				SPEC_SYNTAX(P->loc, "expected \"[]\"");
			P->tok = T_LIST, p++;
			break;
		case '"':
			{
				StringInfoData b;

				initStringInfo(&b);
				for (p++;; p++)
				{
					if (s[p] == '\0')
						SPEC_SYNTAX(P->loc, "unterminated quoted key");
					if (s[p] == '"')
					{
						if (s[p + 1] != '"')
							break;
						p++;
					}
					appendStringInfoChar(&b, s[p]);
				}
				p++;
				if (b.len == 0)
					SPEC_SYNTAX(P->loc, "empty key");
				P->tok = T_WORD, P->word = b.data, P->quoted = true;
				break;
			}
		default:
			if (!bare_char((unsigned char) s[p]))
				SPEC_SYNTAX(p + 1, "unexpected character \"%c\"", s[p]);
			{
				int			b = p;

				while (bare_char((unsigned char) s[p]))
					p++;
				P->tok = T_WORD;
				P->word = pnstrdup(s + b, p - b);
			}
	}
	P->pos = p;
}

static const struct
{
	const char *name;
	SiftShredType type;
} type_names[] = {
	{"bool", SIFT_SHRED_BOOL}, {"boolean", SIFT_SHRED_BOOL},
	{"int2", SIFT_SHRED_INT2}, {"smallint", SIFT_SHRED_INT2},
	{"int4", SIFT_SHRED_INT4}, {"int", SIFT_SHRED_INT4}, {"integer", SIFT_SHRED_INT4},
	{"int8", SIFT_SHRED_INT8}, {"bigint", SIFT_SHRED_INT8},
	{"float4", SIFT_SHRED_FLOAT4}, {"real", SIFT_SHRED_FLOAT4},
	{"float8", SIFT_SHRED_FLOAT8}, {"double", SIFT_SHRED_FLOAT8},
	{"text", SIFT_SHRED_TEXT},
	{"astext", SIFT_SHRED_ASTEXT},
	{NULL, 0}
};

const char *
sift_shred_type_name(SiftShredType t)
{
	static const char *const names[] = {"bool", "int2", "int4", "int8", "float4", "float8", "text", "astext"};

	return names[t];
}

static SiftShredNode *
new_node(NodeKind kind, int group)
{
	SiftShredNode *n = palloc0(sizeof(SiftShredNode));

	n->kind = kind;
	n->group = group;
	return n;
}

static void
flat_key(StringInfo flat, const char *key, int len)
{
	bool		bare = len > 0;

	for (int i = 0; i < len; i++)
		bare &= bare_char((unsigned char) key[i]);
	if (flat->len > 0)
		appendStringInfoChar(flat, '.');
	if (bare)
		appendBinaryStringInfo(flat, key, len);
	else
	{
		appendStringInfoChar(flat, '"');
		for (int i = 0; i < len; i++)
		{
			if (key[i] == '"')
				appendStringInfoChar(flat, '"');
			appendStringInfoChar(flat, key[i]);
		}
		appendStringInfoChar(flat, '"');
	}
}

/* The member for key (members are sorted by length, then bytes, like the keys of a jsonb), or where it would go in *pos */
Member *
find_member(const SiftShredNode *n, const char *key, int len, int *pos)
{
	int			lo = 0,
				hi = n->nmembers;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;
		const Member *m = &n->members[mid];
		int			c = m->len != len ? (m->len < len ? -1 : 1) : memcmp(m->key, key, len);

		if (c == 0)
			return (Member *) m;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (pos)
		*pos = lo;
	return NULL;
}

static SiftShredNode **
member_slot(SiftShredNode *n, const char *key, int len)
{
	int			pos;
	Member	   *m = find_member(n, key, len, &pos);

	if (m)
		return &m->node;
	if (n->nmembers == n->capmembers)
	{
		n->capmembers = n->capmembers ? n->capmembers * 2 : 4;
		n->members = n->members ? repalloc(n->members, n->capmembers * sizeof(Member)) : palloc(n->capmembers * sizeof(Member));
	}
	memmove(&n->members[pos + 1], &n->members[pos], (n->nmembers - pos) * sizeof(Member));
	n->members[pos].key = pnstrdup(key, len);
	n->members[pos].len = len;
	n->members[pos].node = NULL;
	n->nmembers++;
	return &n->members[pos].node;
}

static const char *
kind_noun(NodeKind k)
{
	return k == N_OBJ ? "an object" : k == N_LIST ? "an array" : k == N_MAP ? "a wildcard" : "a value";
}

/* What a step makes of the node it creates, looking at the step after it */
static NodeKind
kind_before(const Step *next)
{
	if (next == NULL)
		return N_LEAF;
	return next->kind == S_LIST ? N_LIST : next->kind == S_WILD ? N_MAP : N_OBJ;
}

/* The declaration being added: its type and the state of the descent */
typedef struct Add
{
	SiftShredType type;
	uint32		maxlen;
	bool		explicit_len;
	int			g;				/* group of the element the path is in */
	StringInfoData flat;		/* the path so far, canonical */
} Add;

/* The node of the wanted kind at *slot, created on first use; a node of another kind is a conflict between two items */
static SiftShredNode *
child_at(Parser *P, SiftShredNode **slot, NodeKind kind, Add *a, const Step *at)
{
	SiftShredNode *n = *slot;

	if (n)
	{
		if (n->kind == N_LEAF && kind == N_LEAF)
			SPEC_INVALID(at->loc, "path \"%s\" is declared twice", a->flat.data);
		if (n->kind != kind)
			SPEC_INVALID(at->loc, "path \"%s\" is used as %s and as %s", a->flat.data, kind_noun(n->kind), kind_noun(kind));
		return n;
	}
	n = new_node(kind, a->g);
	*slot = n;
	if (kind == N_LEAF)
	{
		SiftShredColDef *c;

		if (P->ncols == SIFT_SHRED_MAX_COLS)
			SPEC_LIMIT(at->loc, "more than %d columns", SIFT_SHRED_MAX_COLS);
		c = &P->cols[P->ncols];
		c->name = pstrdup(a->flat.data);
		c->type = a->type;
		c->group = a->g;
		c->maxlen = a->maxlen;
		c->explicit_len = a->explicit_len;
		n->col = P->ncols++;
		n->type = a->type;
		n->maxlen = a->maxlen;
	}
	else if (kind == N_LIST || kind == N_MAP)
	{
		StringInfoData name;

		initStringInfo(&name);
		appendStringInfoString(&name, a->flat.data);
		appendStringInfoString(&name, kind == N_LIST ? "[]" : ".*");
		if (P->ngroups == SIFT_SHRED_MAX_GROUPS)
			SPEC_LIMIT(at->loc, "more than %d arrays and wildcards", SIFT_SHRED_MAX_GROUPS - 1);
		P->groups[P->ngroups].kind = kind == N_LIST ? SIFT_SHRED_LIST : SIFT_SHRED_MAP;
		P->groups[P->ngroups].parent = a->g;
		P->groups[P->ngroups].name = name.data;
		n->pgroup = a->g;
		n->group = P->ngroups++;
	}
	return n;
}

/* Walks (and extends) the tree along the steps of one item; the last step creates the column */
static void
add_path(Parser *P, const Step *st, int n, Add *a)
{
	SiftShredNode *cur = P->root;

	for (int i = 0; i < n; i++)
	{
		const Step *s = &st[i];
		NodeKind	want = kind_before(i + 1 < n ? &st[i + 1] : NULL);

		if (s->kind == S_KEY)
		{
			if (cur->kind != N_OBJ)
				SPEC_INVALID(s->loc, "a key cannot follow \"%s\"", a->flat.data);
			flat_key(&a->flat, s->key, s->len);
			cur = child_at(P, member_slot(cur, s->key, s->len), want, a, s);
		}
		else
		{
			/* [] and * go into the element of the list or map the previous step made */
			appendStringInfoString(&a->flat, s->kind == S_LIST ? "[]" : ".*");
			a->g = cur->group;
			cur = child_at(P, &cur->elem, want, a, s);
		}
		if (cur->kind == N_OBJ)
			a->g = cur->group;
	}
}

static void
expect_word(Parser *P, const char *what)
{
	if (P->tok != T_WORD)
		SPEC_SYNTAX(P->loc, "expected %s", what);
}

static int
parse_path(Parser *P, Step *st, int n, bool relative)
{
	for (bool first = true;; first = false)
	{
		Step	   *s;

		if (n == SIFT_SHRED_MAX_STEPS)
			SPEC_LIMIT(P->loc, "a path has at most %d steps", SIFT_SHRED_MAX_STEPS);
		s = &st[n++];
		s->loc = P->loc;
		if (P->tok == T_STAR && (relative || !first))
			s->kind = S_WILD;
		else
		{
			expect_word(P, first && !relative ? "a key at the start of a path" : "a key");
			s->kind = S_KEY;
			s->key = P->word;
			s->len = strlen(P->word);
		}
		lex(P);
		while (P->tok == T_LIST)
		{
			if (n == SIFT_SHRED_MAX_STEPS)
				SPEC_LIMIT(P->loc, "a path has at most %d steps", SIFT_SHRED_MAX_STEPS);
			st[n].kind = S_LIST;
			st[n++].loc = P->loc;
			lex(P);
		}
		if (P->tok != T_DOT && P->tok != T_ARROW)
			return n;
		lex(P);
		relative = true;		/* a "*" may follow a separator */
	}
}

static void
parse_type(Parser *P, Add *a)
{
	int			i;

	expect_word(P, "a type after the path (bool, int2, int4, int8, float4, float8, text or astext)");
	if (P->quoted)
		SPEC_SYNTAX(P->loc, "expected a type after the path");
	for (i = 0; type_names[i].name; i++)
		if (pg_strcasecmp(P->word, type_names[i].name) == 0)
			break;
	if (!type_names[i].name)
		SPEC_SYNTAX(P->loc, "unknown type \"%s\" (bool, int2, int4, int8, float4, float8, text or astext)", P->word);
	a->type = type_names[i].type;
	a->maxlen = SIFT_SHRED_IS_TEXTUAL(a->type) ? SIFT_SHRED_DEFAULT_TEXT : 0;
	a->explicit_len = false;
	lex(P);
	if (pg_strcasecmp(type_names[i].name, "double") == 0 && P->tok == T_WORD && pg_strcasecmp(P->word, "precision") == 0)
		lex(P);
	if (SIFT_SHRED_IS_TEXTUAL(a->type) && P->tok == T_LPAREN)
	{
		uint64		v = 0;
		int			loc;

		lex(P);
		loc = P->loc;
		expect_word(P, "the length in bytes");
		for (const char *c = P->word; *c; c++)
		{
			if (*c < '0' || *c > '9' || v > SIFT_SHRED_MAX_TEXT)
				SPEC_SYNTAX(loc, "the length of a text is a number of bytes");
			v = v * 10 + (*c - '0');
		}
		if (v < 1 || v > SIFT_SHRED_MAX_TEXT)
			SPEC_INVALID(loc, "the length of a text is between 1 and %d bytes", SIFT_SHRED_MAX_TEXT);
		a->maxlen = (uint32) v;
		a->explicit_len = true;
		lex(P);
		if (P->tok != T_RPAREN)
			SPEC_SYNTAX(P->loc, "expected \")\"");
		lex(P);
	}
}

/* item {, item}: an item is a path and a type, or a path ending in [] or * and a group of items relative to its element */
static void
parse_items(Parser *P, const Step *prefix, int nprefix)
{
	for (;;)
	{
		Step		st[SIFT_SHRED_MAX_STEPS];
		int			n = nprefix;

		if (nprefix > 0)
			memcpy(st, prefix, nprefix * sizeof(Step));
		n = parse_path(P, st, n, nprefix > 0);
		if (P->tok == T_LBRACE)
		{
			if (st[n - 1].kind == S_KEY)
				SPEC_SYNTAX(P->loc, "a group of keys needs an array (key[]) or a wildcard (key.*) before it");
			lex(P);
			parse_items(P, st, n);
			if (P->tok != T_RBRACE)
				SPEC_SYNTAX(P->loc, "expected \"}\"");
			lex(P);
		}
		else
		{
			Add			a;

			a.g = 0;
			initStringInfo(&a.flat);
			parse_type(P, &a);
			add_path(P, st, n, &a);
		}
		if (P->tok != T_COMMA)
			return;
		lex(P);
	}
}

SiftShredSpec *
sift_shred_spec_parse(const char *text)
{
	Parser		P;
	SiftShredSpec *spec;

	if (strlen(text) > SIFT_SHRED_MAX_SPEC_BYTES)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("sift shred paths: the spec has more than %d bytes", SIFT_SHRED_MAX_SPEC_BYTES),
						errhint("Declare fewer paths, or a wildcard path for the keys of an object.")));
	memset(&P, 0, sizeof(P));
	P.src = text;
	P.root = new_node(N_OBJ, 0);
	P.cols = palloc0(SIFT_SHRED_MAX_COLS * sizeof(SiftShredColDef));
	P.groups = palloc0(SIFT_SHRED_MAX_GROUPS * sizeof(SiftShredGroupDef));
	P.groups[0].kind = SIFT_SHRED_ROOT;
	P.groups[0].parent = -1;
	P.groups[0].name = "";
	P.ngroups = 1;
	lex(&P);
	parse_items(&P, NULL, 0);
	if (P.tok != T_END)
		SPEC_SYNTAX(P.loc, "expected \",\" or the end of the paths");

	spec = palloc0(sizeof(SiftShredSpec));
	spec->ncols = P.ncols;
	spec->ngroups = P.ngroups;
	spec->cols = P.cols;
	spec->groups = P.groups;
	spec->root = P.root;
	for (int g = 0; g < P.ngroups; g++)
	{
		spec->groups[g].cols = palloc0((P.ncols + 1) * sizeof(int));
		spec->groups[g].children = palloc0(P.ngroups * sizeof(int));
	}
	for (int c = 0; c < P.ncols; c++)
	{
		SiftShredGroupDef *gd = &spec->groups[spec->cols[c].group];

		gd->cols[gd->ncols++] = c;
	}
	for (int g = 1; g < P.ngroups; g++)
	{
		SiftShredGroupDef *pd = &spec->groups[spec->groups[g].parent];

		pd->children[pd->nchildren++] = g;
	}

	initStringInfo(&P.canonical);
	for (int c = 0; c < P.ncols; c++)
	{
		const SiftShredColDef *cd = &spec->cols[c];

		appendStringInfo(&P.canonical, "%s%s %s", c ? ", " : "", cd->name, sift_shred_type_name(cd->type));
		if (cd->explicit_len)
			appendStringInfo(&P.canonical, "(%u)", cd->maxlen);
	}
	spec->canonical = P.canonical.data;
	spec->fingerprint = sift_term_hash64(P.canonical.data, P.canonical.len);
	return spec;
}

const char *
sift_shred_spec_deparse(const SiftShredSpec *spec)
{
	return spec->canonical;
}
