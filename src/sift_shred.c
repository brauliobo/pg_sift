/*
 * sift_shred.c - the path spec of sift_col, the shredder state machine and the wildcard dictionary (docs/COL-SHRED.md).
 *
 * A spec is a tree of nodes: OBJ (a JSON object with declared members), LEAF (a typed column), LIST (key[]: an array whose
 * elements are the node `elem`, a group of its own) and MAP (key.*: every member of an object, a group of its own). The
 * shredder walks the document as events; the top frame says which node the next value binds to.
 */
#include "sift_host.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sift_analyze.h"
#include "sift_shred.h"

typedef enum NodeKind
{
	N_OBJ,
	N_LEAF,
	N_LIST,
	N_MAP
} NodeKind;

typedef struct Member
{
	char	   *key;
	int			len;
	SiftShredNode *node;
} Member;

struct SiftShredNode
{
	NodeKind	kind;
	int			col;			/* LEAF */
	SiftShredType type;			/* LEAF */
	uint32		maxlen;			/* LEAF text */
	int			group;			/* OBJ, LEAF: the group whose element they sit in; LIST, MAP: their own group */
	int			pgroup;			/* LIST, MAP: the group of the element the list hangs from */
	SiftShredNode *elem;	/* LIST, MAP: what each element is */
	int			nmembers;		/* OBJ, sorted by (len, bytes) like the keys of a jsonb */
	int			capmembers;
	Member	   *members;
};

#define IS_NUMERIC(t)	((t) >= SIFT_SHRED_INT2 && (t) <= SIFT_SHRED_FLOAT8)

/* ======================================================================== */
/* the spec parser */
/* ======================================================================== */

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
	{NULL, 0}
};

const char *
sift_shred_type_name(SiftShredType t)
{
	static const char *const names[] = {"bool", "int2", "int4", "int8", "float4", "float8", "text"};

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
static Member *
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

	expect_word(P, "a type after the path (bool, int2, int4, int8, float4, float8 or text)");
	if (P->quoted)
		SPEC_SYNTAX(P->loc, "expected a type after the path");
	for (i = 0; type_names[i].name; i++)
		if (pg_strcasecmp(P->word, type_names[i].name) == 0)
			break;
	if (!type_names[i].name)
		SPEC_SYNTAX(P->loc, "unknown type \"%s\" (bool, int2, int4, int8, float4, float8 or text)", P->word);
	a->type = type_names[i].type;
	a->maxlen = a->type == SIFT_SHRED_TEXT ? SIFT_SHRED_DEFAULT_TEXT : 0;
	a->explicit_len = false;
	lex(P);
	if (pg_strcasecmp(type_names[i].name, "double") == 0 && P->tok == T_WORD && pg_strcasecmp(P->word, "precision") == 0)
		lex(P);
	if (a->type == SIFT_SHRED_TEXT && P->tok == T_LPAREN)
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
						errmsg("sift shred paths: the spec has more than %d bytes", SIFT_SHRED_MAX_SPEC_BYTES)));
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

/* ======================================================================== */
/* the wildcard dictionary */
/* ======================================================================== */

struct SiftShredDict
{
	MemoryContext mcxt;
	uint32		max_keys;
	uint32		n;
	uint32		tabsize;		/* power of two, at least twice n */
	uint32	   *tab;			/* id + 1; 0 = empty */
	uint32		idcap;
	uint32	   *off;
	uint32	   *len;
	char	   *bytes;
	uint32		nbytes;
	uint32		bytescap;
};

SiftShredDict *
sift_shred_dict_create(uint32 max_keys)
{
	SiftShredDict *d = palloc0(sizeof(SiftShredDict));

	d->mcxt = CurrentMemoryContext;
	d->max_keys = max_keys;
	d->tabsize = 64;
	d->tab = palloc0(d->tabsize * sizeof(uint32));
	return d;
}

static uint32
key_hash(const char *key, uint32 len)
{
	return sift_term_hash(key, len);
}

uint32
sift_shred_dict_find(const SiftShredDict *d, const char *key, uint32 len)
{
	for (uint32 i = key_hash(key, len) & (d->tabsize - 1);; i = (i + 1) & (d->tabsize - 1))
	{
		uint32		id = d->tab[i];

		if (id == 0)
			return SIFT_SHRED_NO_KEY;
		id--;
		if (d->len[id] == len && memcmp(d->bytes + d->off[id], key, len) == 0)
			return id;
	}
}

static void
dict_grow_table(SiftShredDict *d)
{
	uint32	   *old = d->tab;
	uint32		oldsize = d->tabsize;

	d->tabsize *= 2;
	d->tab = MemoryContextAllocZero(d->mcxt, d->tabsize * sizeof(uint32));
	for (uint32 j = 0; j < oldsize; j++)
	{
		uint32		id = old[j];
		uint32		i;

		if (id == 0)
			continue;
		i = key_hash(d->bytes + d->off[id - 1], d->len[id - 1]) & (d->tabsize - 1);
		while (d->tab[i])
			i = (i + 1) & (d->tabsize - 1);
		d->tab[i] = id;
	}
	pfree(old);
}

uint32
sift_shred_dict_intern(SiftShredDict *d, const char *key, uint32 len)
{
	uint32		id = sift_shred_dict_find(d, key, len);
	uint32		i;

	if (id != SIFT_SHRED_NO_KEY)
		return id;
	if (d->n >= d->max_keys || len > SIFT_SHRED_MAX_KEY_BYTES)
		return SIFT_SHRED_NO_KEY;
	if (d->n == d->idcap)
	{
		d->idcap = d->idcap ? d->idcap * 2 : 64;
		d->off = d->off ? repalloc(d->off, d->idcap * sizeof(uint32)) : MemoryContextAlloc(d->mcxt, d->idcap * sizeof(uint32));
		d->len = d->len ? repalloc(d->len, d->idcap * sizeof(uint32)) : MemoryContextAlloc(d->mcxt, d->idcap * sizeof(uint32));
	}
	if (d->bytes == NULL || d->nbytes + len > d->bytescap)
	{
		d->bytescap = Max(d->bytescap * 2, Max(1024, d->nbytes + len));
		d->bytes = d->bytes ? repalloc(d->bytes, d->bytescap) : MemoryContextAlloc(d->mcxt, d->bytescap);
	}
	memcpy(d->bytes + d->nbytes, key, len);
	d->off[d->n] = d->nbytes;
	d->len[d->n] = len;
	d->nbytes += len;
	id = d->n++;
	if (d->n * 2 > d->tabsize)
		dict_grow_table(d);
	for (i = key_hash(key, len) & (d->tabsize - 1); d->tab[i]; i = (i + 1) & (d->tabsize - 1))
		;
	d->tab[i] = id + 1;
	return id;
}

const char *
sift_shred_dict_key(const SiftShredDict *d, uint32 id, uint32 *len)
{
	if (id >= d->n)
		return NULL;
	*len = d->len[id];
	return d->bytes + d->off[id];
}

uint32
sift_shred_dict_count(const SiftShredDict *d)
{
	return d->n;
}

/* ======================================================================== */
/* numbers: the decimal text of a jsonb numeric (or any JSON number) to the declared type */
/* ======================================================================== */

typedef struct Dec
{
	bool		neg;
	const char *ip;				/* integer digits */
	int			nint;
	const char *fp;				/* fraction digits */
	int			nfrac;
	long		ex;				/* exponent, clamped to +-100000 */
} Dec;

#define ISDIG(c)	((c) >= '0' && (c) <= '9')

/* [+-]digits[.digits][(e|E)[+-]digits] covering the whole text, at least one digit */
static bool
dec_parse(const char *s, int len, Dec *d)
{
	const char *p = s,
			   *e = s + len;

	memset(d, 0, sizeof(*d));
	if (p < e && (*p == '-' || *p == '+'))
		d->neg = *p++ == '-';
	d->ip = p;
	while (p < e && ISDIG(*p))
		p++;
	d->nint = p - d->ip;
	d->fp = p;
	if (p < e && *p == '.')
	{
		d->fp = ++p;
		while (p < e && ISDIG(*p))
			p++;
		d->nfrac = p - d->fp;
	}
	if (d->nint + d->nfrac == 0)
		return false;
	if (p < e && (*p == 'e' || *p == 'E'))
	{
		bool		eneg = false;

		p++;
		if (p < e && (*p == '+' || *p == '-'))
			eneg = *p++ == '-';
		if (p >= e || !ISDIG(*p))
			return false;
		for (; p < e && ISDIG(*p); p++)
			if (d->ex < 100000)
				d->ex = d->ex * 10 + (*p - '0');
		if (eneg)
			d->ex = -d->ex;
	}
	return p == e;
}

/* The magnitude when the number is an integer below 10^19; false for a fraction or a larger number */
static bool
dec_integral(const Dec *d, uint64 *mag)
{
	int			total = d->nint + d->nfrac,
				lead = 0,
				end;
	long		scale;

#define DIG(i)	((i) < d->nint ? d->ip[i] : d->fp[(i) - d->nint])
	while (lead < total && DIG(lead) == '0')
		lead++;
	*mag = 0;
	if (lead == total)
		return true;
	for (end = total; DIG(end - 1) == '0'; end--)
		;
	scale = d->ex - d->nfrac + (total - end);
	if (scale < 0 || (end - lead) + scale > 19)
		return false;
	for (int i = lead; i < end; i++)
		*mag = *mag * 10 + (DIG(i) - '0');
	for (long k = 0; k < scale; k++)
		*mag *= 10;
#undef DIG
	return true;
}

static bool
int_fits(SiftShredType t, bool neg, uint64 mag, int64 *out)
{
	static const int64 max[] = {0, PG_INT16_MAX, PG_INT32_MAX, PG_INT64_MAX};

	if (mag > (uint64) max[t] + neg)
		return false;
	*out = neg ? (mag == 0 ? 0 : -(int64) (mag - 1) - 1) : (int64) mag;
	return true;
}

/*
 * float4 / float8 of the decimal text: the correctly rounded value strtof / strtod give, which is what float4in / float8in return
 * (the backend keeps LC_NUMERIC at "C"). Like them, a result that is infinite or rounds to zero is out of range; denormals are not.
 * Integers that are exact in the type skip the library.
 */
static bool
dec_float(const Dec *d, const char *text, int len, bool is4, double *out)
{
	double		v;
	char	   *end;

	if (d->nfrac == 0 && d->ex == 0 && d->nint <= 15)
	{
		uint64		m = 0;

		for (int i = 0; i < d->nint; i++)
			m = m * 10 + (d->ip[i] - '0');
		if (!is4 || m <= (UINT64CONST(1) << 24))
		{
			*out = d->neg && m ? -(double) m : (double) m;
			return true;
		}
	}
	errno = 0;
	v = is4 ? (double) strtof(text, &end) : strtod(text, &end);
	if (end != text + len || isinf(v) || (errno == ERANGE && v == 0))
		return false;
	*out = v == 0 ? 0.0 : v;
	return true;
}

/* ======================================================================== */
/* the shredder */
/* ======================================================================== */

#define MAX_FRAMES	(2 * SIFT_SHRED_MAX_STEPS + 2)

static void *
regrow(SiftShred *sh, void *p, size_t size)
{
	return p ? repalloc(p, size) : MemoryContextAlloc(sh->mcxt, size);
}

SiftShred *
sift_shred_create(const SiftShredSpec *spec, const SiftShredLimits *limits)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift shred", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftShred  *sh = palloc0(sizeof(SiftShred));

	sift_shred_init();
	sh->mcxt = cx;
	sh->spec = spec;
	sh->limits.max_elems = limits && limits->max_elems ? limits->max_elems : SIFT_SHRED_DEFAULT_ELEMS;
	sh->limits.max_bytes = limits && limits->max_bytes ? limits->max_bytes : SIFT_SHRED_DEFAULT_BYTES;
	sh->limits.max_keys = limits && limits->max_keys ? limits->max_keys : SIFT_SHRED_DEFAULT_KEYS;
	sh->doc.col = palloc0(spec->ncols * sizeof(SiftShredCol));
	sh->doc.grp = palloc0(spec->ngroups * sizeof(SiftShredGroup));
	sh->frames = palloc(MAX_FRAMES * sizeof(SiftShredFrame));
	sh->maxframes = MAX_FRAMES;
	sh->dict = sift_shred_dict_create(sh->limits.max_keys);
	MemoryContextSwitchTo(old);
	return sh;
}

void
sift_shred_free(SiftShred *sh)
{
	MemoryContextDelete(sh->mcxt);
}

/* Room for `need` elements in group g: the slots of its columns and the member ids of a map */
static void
reserve_elems(SiftShred *sh, int g, uint32 need)
{
	SiftShredGroup *gr = &sh->doc.grp[g];
	const SiftShredGroupDef *gd = &sh->spec->groups[g];
	uint32		cap;

	if (need <= gr->cap)
		return;
	for (cap = Max(16, gr->cap * 2); cap < need;)
		cap *= 2;
	for (int i = 0; i < gd->ncols; i++)
	{
		SiftShredCol *col = &sh->doc.col[gd->cols[i]];

		col->state = regrow(sh, col->state, cap);
		col->val = regrow(sh, col->val, cap * sizeof(SiftShredVal));
	}
	if (gd->kind == SIFT_SHRED_MAP)
		gr->keyid = regrow(sh, gr->keyid, cap * sizeof(uint32));
	gr->cap = cap;
}

/* Room for `need` parent elements in group g: the list under each of them */
static void
reserve_parents(SiftShred *sh, int g, uint32 need)
{
	SiftShredGroup *gr = &sh->doc.grp[g];
	uint32		cap;

	if (need <= gr->pcap)
		return;
	for (cap = Max(16, gr->pcap * 2); cap < need;)
		cap *= 2;
	gr->end = regrow(sh, gr->end, cap * sizeof(uint32));
	gr->lstate = regrow(sh, gr->lstate, cap);
	gr->pcap = cap;
}

/* Appends an element to group g: its columns absent, its lists empty and absent */
static void
open_element(SiftShred *sh, int g, uint32 keyid)
{
	SiftShredGroup *gr = &sh->doc.grp[g];
	const SiftShredGroupDef *gd = &sh->spec->groups[g];
	uint32		i = gr->n;

	reserve_elems(sh, g, i + 1);
	for (int k = 0; k < gd->ncols; k++)
	{
		SiftShredCol *col = &sh->doc.col[gd->cols[k]];

		col->state[i] = SIFT_SHRED_ABSENT;
		col->val[i].i = 0;
	}
	if (gd->kind == SIFT_SHRED_MAP)
		gr->keyid[i] = keyid;
	gr->n = i + 1;
	for (int k = 0; k < gd->nchildren; k++)
	{
		SiftShredGroup *ch = &sh->doc.grp[gd->children[k]];

		reserve_parents(sh, gd->children[k], i + 1);
		ch->end[i] = ch->n;
		ch->lstate[i] = SIFT_SHRED_ABSENT;
		ch->pn = i + 1;
	}
}

static void
release_large(SiftShred *sh)
{
	for (int g = 0; g < sh->spec->ngroups; g++)
	{
		SiftShredGroup *gr = &sh->doc.grp[g];
		const SiftShredGroupDef *gd = &sh->spec->groups[g];

		if (gr->cap > SIFT_SHRED_RETAIN)
		{
			for (int i = 0; i < gd->ncols; i++)
			{
				pfree(sh->doc.col[gd->cols[i]].state);
				pfree(sh->doc.col[gd->cols[i]].val);
				sh->doc.col[gd->cols[i]].state = NULL;
				sh->doc.col[gd->cols[i]].val = NULL;
			}
			if (gr->keyid)
				pfree(gr->keyid);
			gr->keyid = NULL;
			gr->cap = 0;
		}
		if (gr->pcap > SIFT_SHRED_RETAIN)
		{
			pfree(gr->end);
			pfree(gr->lstate);
			gr->end = NULL;
			gr->lstate = NULL;
			gr->pcap = 0;
		}
	}
	if (sh->doc.textcap > SIFT_SHRED_RETAIN * 16)
	{
		pfree(sh->doc.text);
		sh->doc.text = NULL;
		sh->doc.textcap = 0;
	}
}

void
sift_shred_begin(SiftShred *sh)
{
	release_large(sh);
	for (int g = 0; g < sh->spec->ngroups; g++)
		sh->doc.grp[g].n = sh->doc.grp[g].pn = 0;
	for (int c = 0; c < sh->spec->ncols; c++)
		sh->doc.col[c].ovf = 0;
	sh->doc.ntext = 0;
	sh->doc.nelems = 0;
	sh->nframes = 0;
	sh->root_done = false;
	open_element(sh, 0, 0);
}

/* ---------- where the next value goes ---------- */

typedef const SiftShredNode *NodeP;

static NodeP
peek_target(const SiftShred *sh)
{
	const SiftShredFrame *f;

	if (sh->nframes == 0)
		return sh->root_done ? NULL : sh->spec->root;
	f = &sh->frames[sh->nframes - 1];
	if (f->node->kind == N_OBJ)
		return f->pending;
	return f->truncated ? NULL : f->node->elem;
}

static void
list_set(SiftShred *sh, NodeP n, SiftShredState st)
{
	sh->doc.grp[n->group].lstate[sh->doc.grp[n->pgroup].n - 1] = st;
}

static void
leaf_set(SiftShred *sh, NodeP n, SiftShredState st, uint32 reason)
{
	sh->doc.col[n->col].state[sh->doc.grp[n->group].n - 1] = st;
	sh->doc.col[n->col].ovf |= reason;
}

static void
flag_leaves(SiftShred *sh, NodeP n, uint32 reason)
{
	switch (n->kind)
	{
		case N_LEAF:
			sh->doc.col[n->col].ovf |= reason;
			break;
		case N_OBJ:
			for (int i = 0; i < n->nmembers; i++)
				flag_leaves(sh, n->members[i].node, reason);
			break;
		default:
			flag_leaves(sh, n->elem, reason);
	}
}

/* Everything the node declares in the current element cannot be represented */
static void
mark_overflow(SiftShred *sh, NodeP n, uint32 reason)
{
	switch (n->kind)
	{
		case N_LEAF:
			leaf_set(sh, n, SIFT_SHRED_OVERFLOW, reason);
			break;
		case N_OBJ:
			for (int i = 0; i < n->nmembers; i++)
				mark_overflow(sh, n->members[i].node, reason);
			break;
		default:
			list_set(sh, n, SIFT_SHRED_OVERFLOW);
			flag_leaves(sh, n->elem, reason);
	}
}

/* The node the next value binds to; opens the element when the value is one of a list or map (NULL: nothing declared / cut) */
static NodeP
take_target(SiftShred *sh)
{
	SiftShredFrame *f;
	NodeP		n;

	if (sh->nframes == 0)
	{
		NodeP		root = peek_target(sh);

		sh->root_done = true;
		return root;
	}
	f = &sh->frames[sh->nframes - 1];
	n = f->node;
	if (n->kind == N_OBJ)
	{
		NodeP		t = f->pending;

		f->pending = NULL;
		return t;
	}
	if (f->truncated)
		return NULL;
	if (sh->doc.nelems >= sh->limits.max_elems)
	{
		list_set(sh, n, SIFT_SHRED_OVERFLOW);
		flag_leaves(sh, n->elem, SIFT_SHRED_OVF_TRUNC);
		f->truncated = true;
		return NULL;
	}
	sh->doc.nelems++;
	open_element(sh, n->group, f->keyid);
	if (f->keyid == SIFT_SHRED_NO_KEY)
	{
		mark_overflow(sh, n->elem, SIFT_SHRED_OVF_KEYS);
		return NULL;
	}
	return n->elem;
}

/* ---------- events ---------- */

SiftShredKey
sift_shred_key(SiftShred *sh, const char *key, int len)
{
	SiftShredFrame *f = &sh->frames[sh->nframes - 1];
	const Member *m;

	if (f->node->kind == N_MAP)
	{
		f->keyid = sift_shred_dict_intern(sh->dict, key, len);
		return SIFT_SHRED_TAKE;
	}
	m = find_member(f->node, key, len, NULL);
	if (!m)
		return SIFT_SHRED_SKIP;
	f->pending = m->node;
	return ++f->matched == (uint32) f->node->nmembers ? SIFT_SHRED_STOP : SIFT_SHRED_TAKE;
}

bool
sift_shred_enter(SiftShred *sh, bool is_array)
{
	NodeP		t = take_target(sh);
	SiftShredFrame *f;

	if (t == NULL)
		return false;
	if (t->kind == N_LEAF || (t->kind == N_LIST) != is_array)
	{
		mark_overflow(sh, t, SIFT_SHRED_OVF_SHAPE);
		return false;
	}
	if (t->kind != N_OBJ)
		list_set(sh, t, SIFT_SHRED_VALUE);
	if (sh->nframes == sh->maxframes)
		elog(ERROR, "sift shred: nesting deeper than the spec allows");
	f = &sh->frames[sh->nframes++];
	f->node = t;
	f->pending = NULL;
	f->keyid = 0;
	f->matched = 0;
	f->truncated = false;
	return true;
}

void
sift_shred_leave(SiftShred *sh)
{
	const SiftShredFrame *f = &sh->frames[--sh->nframes];

	if (f->node->kind != N_OBJ)
		sh->doc.grp[f->node->group].end[sh->doc.grp[f->node->pgroup].n - 1] = sh->doc.grp[f->node->group].n;
}

bool
sift_shred_full(const SiftShred *sh)
{
	return sh->nframes > 0 && sh->frames[sh->nframes - 1].truncated;
}

int
sift_shred_object_keys(const SiftShred *sh)
{
	const SiftShredNode *n = sh->frames[sh->nframes - 1].node;

	return n->kind == N_OBJ ? n->nmembers : 0;
}

const char *
sift_shred_object_key(const SiftShred *sh, int i, int *len)
{
	const Member *m = &sh->frames[sh->nframes - 1].node->members[i];

	*len = m->len;
	return m->key;
}

bool
sift_shred_wants_number(const SiftShred *sh)
{
	NodeP		t = peek_target(sh);

	return t && t->kind == N_LEAF && IS_NUMERIC(t->type);
}

/* A scalar where a list or map is declared: null is its null, anything else cannot be a list */
static void
scalar_in_container(SiftShred *sh, NodeP t, bool is_null)
{
	if (is_null)
		list_set(sh, t, SIFT_SHRED_NULL);
	else
		mark_overflow(sh, t, SIFT_SHRED_OVF_SHAPE);
}

void
sift_shred_null(SiftShred *sh)
{
	NodeP		t = take_target(sh);

	if (t == NULL || t->kind == N_OBJ)
		return;
	if (t->kind == N_LEAF)
		leaf_set(sh, t, SIFT_SHRED_NULL, 0);
	else
		scalar_in_container(sh, t, true);
}

void
sift_shred_bool(SiftShred *sh, bool b)
{
	NodeP		t = take_target(sh);

	if (t == NULL || t->kind == N_OBJ)
		return;
	if (t->kind != N_LEAF)
		scalar_in_container(sh, t, false);
	else if (t->type != SIFT_SHRED_BOOL)
		leaf_set(sh, t, SIFT_SHRED_OVERFLOW, SIFT_SHRED_OVF_TYPE);
	else
	{
		sh->doc.col[t->col].val[sh->doc.grp[t->group].n - 1].i = b;
		leaf_set(sh, t, SIFT_SHRED_VALUE, 0);
	}
}

static void
leaf_text(SiftShred *sh, NodeP t, const char *s, int len)
{
	SiftShredDoc *doc = &sh->doc;
	SiftShredVal *v = &doc->col[t->col].val[doc->grp[t->group].n - 1];

	if (t->type != SIFT_SHRED_TEXT)
		leaf_set(sh, t, SIFT_SHRED_OVERFLOW, SIFT_SHRED_OVF_TYPE);
	else if ((uint32) len > t->maxlen)
		leaf_set(sh, t, SIFT_SHRED_OVERFLOW, SIFT_SHRED_OVF_LENGTH);
	else if ((uint64) doc->ntext + len > sh->limits.max_bytes)
		leaf_set(sh, t, SIFT_SHRED_OVERFLOW, SIFT_SHRED_OVF_TRUNC);
	else
	{
		if (doc->text == NULL || doc->ntext + len > doc->textcap)
		{
			doc->textcap = Max(Max(4096, doc->textcap * 2), doc->ntext + len);
			doc->text = regrow(sh, doc->text, doc->textcap);
		}
		memcpy(doc->text + doc->ntext, s, len);
		v->s.off = doc->ntext;
		v->s.len = len;
		doc->ntext += len;
		leaf_set(sh, t, SIFT_SHRED_VALUE, 0);
	}
}

void
sift_shred_string(SiftShred *sh, const char *s, int len)
{
	NodeP		t = take_target(sh);

	if (t == NULL || t->kind == N_OBJ)
		return;
	if (t->kind != N_LEAF)
		scalar_in_container(sh, t, false);
	else
		leaf_text(sh, t, s, len);
}

static void
leaf_number(SiftShred *sh, NodeP t, const char *text, int len)
{
	SiftShredVal *v = &sh->doc.col[t->col].val[sh->doc.grp[t->group].n - 1];
	Dec			d;
	uint64		mag;

	if (!IS_NUMERIC(t->type) || text == NULL || !dec_parse(text, len, &d))
		leaf_set(sh, t, SIFT_SHRED_OVERFLOW, SIFT_SHRED_OVF_TYPE);
	else if (t->type <= SIFT_SHRED_INT8 ? !(dec_integral(&d, &mag) && int_fits(t->type, d.neg, mag, &v->i))
			 : !dec_float(&d, text, len, t->type == SIFT_SHRED_FLOAT4, &v->f))
		leaf_set(sh, t, SIFT_SHRED_OVERFLOW, SIFT_SHRED_OVF_RANGE);
	else
		leaf_set(sh, t, SIFT_SHRED_VALUE, 0);
}

void
sift_shred_number(SiftShred *sh, const char *text, int len)
{
	NodeP		t = take_target(sh);

	if (t == NULL || t->kind == N_OBJ)
		return;
	if (t->kind != N_LEAF)
		scalar_in_container(sh, t, false);
	else
		leaf_number(sh, t, text, len);
}

void
sift_shred_finish(SiftShred *sh)
{
	if (sh->nframes != 0)
		elog(ERROR, "sift shred: finished with %d containers open", sh->nframes);
}

void
sift_shred_expand_parents(const SiftShredDoc *doc, int g, uint32 *parent)
{
	sift_shred_kern->expand_parents(doc->grp[g].end, doc->grp[g].pn, parent);
}
