/*
 * col/shred.c - the path spec of sift_col, the shredder state machine and its wildcard keys (docs/COL-SHRED.md).
 *
 * A spec is a tree of nodes: OBJ (a JSON object with declared members), LEAF (a typed column), LIST (key[]: an array whose
 * elements are the node `elem`, a group of its own) and MAP (key.*: every member of an object, a group of its own). The
 * shredder walks the document as events; the top frame says which node the next value binds to.
 * The spec parser is col/shred_spec.c; the node tree they share is col/shred_int.h.
 */
#include "core/host.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bm25/analyze.h"
#include "col/kern.h"
#include "col/shred_int.h"

#define IS_NUMERIC(t)	((t) >= SIFT_SHRED_INT2 && (t) <= SIFT_SHRED_FLOAT8)

StaticAssertDecl(SIFT_SHRED_MAX_KEY_BYTES <= SIFT_COL_DICT_MAX_ENTRY, "a wildcard key must fit a col dictionary string");

static uint32
intern_key(SiftShred *sh, const char *key, uint32 len)
{
	int64		id = sift_cdict_find(sh->dict, key, len);

	if (id >= 0)
		return (uint32) id;
	if (sh->dict->n >= sh->limits.max_keys || len > SIFT_SHRED_MAX_KEY_BYTES)
		return SIFT_SHRED_NO_KEY;
	return sift_cdict_intern(sh->dict, key, len);
}

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

#define MAX_FRAMES	(2 * SIFT_SHRED_MAX_STEPS + 2)

static void *
regrow(SiftShred *sh, void *p, size_t size)
{
	return p ? repalloc(p, size) : MemoryContextAlloc(sh->mcxt, size);
}

SiftShred *
sift_shred_create(const SiftShredSpec *spec, const SiftShredLimits *limits, bool cached)
{
	MemoryContext cx = cached ? AllocSetContextCreate(CurrentMemoryContext, "sift cache shred", ALLOCSET_DEFAULT_SIZES) :
		AllocSetContextCreate(CurrentMemoryContext, "sift shred", ALLOCSET_DEFAULT_SIZES);
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
	sh->dict = sift_cdict_create(cx);
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
		col->tag = regrow(sh, col->tag, cap);
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
		col->tag[i] = SIFT_COLTAG_UNKNOWN;
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
				pfree(sh->doc.col[gd->cols[i]].tag);
				pfree(sh->doc.col[gd->cols[i]].val);
				sh->doc.col[gd->cols[i]].state = NULL;
				sh->doc.col[gd->cols[i]].tag = NULL;
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
leaf_set(SiftShred *sh, NodeP n, SiftShredState st, uint32 reason, uint8 tag)
{
	SiftShredCol *col = &sh->doc.col[n->col];
	uint32		i = sh->doc.grp[n->group].n - 1;

	col->state[i] = st;
	col->tag[i] = tag;
	col->ovf |= reason;
}

/* A leaf whose value the declared type cannot hold: the slot is OVERFLOW and keeps the JSON type of the value (SIFT_COLTAG_NONE when the leaf is below the array the event delivered: its path leads to no value) */
static void
leaf_overflow(SiftShred *sh, NodeP n, uint32 reason, uint8 tag)
{
	leaf_set(sh, n, SIFT_SHRED_OVERFLOW, reason, tag);
}

/* The kind of the value an ASTEXT leaf holds: its text does not say whether it was a string, a number or a boolean (a string is the leaf's own) */
static uint8
text_kind(const SiftShred *sh, NodeP t)
{
	return t->type == SIFT_SHRED_ASTEXT && sh->evtag != SIFT_COLTAG_STRING ? sh->evtag : SIFT_COLTAG_UNKNOWN;
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

/* Everything the node declares in the current element cannot be represented; tag: the type of the value, which only a leaf that is the target itself holds */
static void
mark_overflow(SiftShred *sh, NodeP n, uint32 reason, uint8 tag)
{
	switch (n->kind)
	{
		case N_LEAF:
			leaf_overflow(sh, n, reason, tag);
			break;
		case N_OBJ:
			for (int i = 0; i < n->nmembers; i++)
				mark_overflow(sh, n->members[i].node, reason, SIFT_COLTAG_NONE);
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
		mark_overflow(sh, n->elem, SIFT_SHRED_OVF_KEYS, sh->evtag);
		return NULL;
	}
	return n->elem;
}

SiftShredKey
sift_shred_key(SiftShred *sh, const char *key, int len)
{
	SiftShredFrame *f = &sh->frames[sh->nframes - 1];
	const Member *m;

	if (f->node->kind == N_MAP)
	{
		f->keyid = intern_key(sh, key, len);
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
	NodeP		t;
	SiftShredFrame *f;

	sh->evtag = is_array ? SIFT_COLTAG_ARRAY : SIFT_COLTAG_OBJECT;
	if ((t = take_target(sh)) == NULL)
		return false;
	if (t->kind == N_LEAF || (t->kind == N_LIST) != is_array)
	{
		mark_overflow(sh, t, SIFT_SHRED_OVF_SHAPE, sh->evtag);
		return false;
	}
	if (t->kind != N_OBJ)
		list_set(sh, t, SIFT_SHRED_VALUE);
	SIFT_INVARIANT(sh->nframes != sh->maxframes, "sift shred: nesting deeper than the spec allows");
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

bool
sift_shred_wants_number_text(const SiftShred *sh)
{
	NodeP		t = peek_target(sh);

	return t && t->kind == N_LEAF && t->type == SIFT_SHRED_ASTEXT;
}

SiftShredTarget
sift_shred_target(const SiftShred *sh, int *id)
{
	NodeP		t = peek_target(sh);

	if (t == NULL)
		return SIFT_SHRED_T_NONE;
	switch (t->kind)
	{
		case N_LEAF:
			*id = t->col;
			return SIFT_SHRED_T_LEAF;
		case N_OBJ:
			*id = t->group;
			return sh->nframes > 0 && sh->frames[sh->nframes - 1].node->kind != N_OBJ ? SIFT_SHRED_T_ELEM : SIFT_SHRED_T_OBJECT;
		default:
			*id = t->group;
			return SIFT_SHRED_T_LIST;
	}
}

void
sift_shred_value_i64(SiftShred *sh, int64 v)
{
	NodeP		t = take_target(sh);

	sh->doc.col[t->col].val[sh->doc.grp[t->group].n - 1].i = v;
	leaf_set(sh, t, SIFT_SHRED_VALUE, 0, SIFT_COLTAG_UNKNOWN);
}

void
sift_shred_value_f64(SiftShred *sh, double v)
{
	NodeP		t = take_target(sh);

	sh->doc.col[t->col].val[sh->doc.grp[t->group].n - 1].f = v;
	leaf_set(sh, t, SIFT_SHRED_VALUE, 0, SIFT_COLTAG_UNKNOWN);
}

void
sift_shred_poison(SiftShred *sh)
{
	for (int i = sh->nframes - 1; i >= 0; i--)
	{
		NodeP		n = sh->frames[i].node;

		if (n->kind == N_OBJ)
			continue;
		list_set(sh, n, SIFT_SHRED_OVERFLOW);
		flag_leaves(sh, n->elem, SIFT_SHRED_OVF_SHAPE);
		return;
	}
}

/* A scalar where a list or map is declared: null is its null, anything else cannot be a list */
static void
scalar_in_container(SiftShred *sh, NodeP t, bool is_null)
{
	if (is_null)
		list_set(sh, t, SIFT_SHRED_NULL);
	else
		mark_overflow(sh, t, SIFT_SHRED_OVF_SHAPE, sh->evtag);
}

void
sift_shred_null(SiftShred *sh)
{
	NodeP		t;

	sh->evtag = SIFT_COLTAG_UNKNOWN;
	t = take_target(sh);

	if (t == NULL || t->kind == N_OBJ)
		return;
	if (t->kind == N_LEAF)
		leaf_set(sh, t, SIFT_SHRED_NULL, 0, SIFT_COLTAG_JNULL);
	else
		scalar_in_container(sh, t, true);
}

static void leaf_text(SiftShred *sh, NodeP t, const char *s, int len);

void
sift_shred_bool(SiftShred *sh, bool b)
{
	NodeP		t;

	sh->evtag = SIFT_COLTAG_BOOLEAN;
	t = take_target(sh);

	if (t == NULL || t->kind == N_OBJ)
		return;
	if (t->kind != N_LEAF)
		scalar_in_container(sh, t, false);
	else if (t->type == SIFT_SHRED_ASTEXT)
		leaf_text(sh, t, b ? "true" : "false", b ? 4 : 5);
	else if (t->type != SIFT_SHRED_BOOL)
		leaf_overflow(sh, t, SIFT_SHRED_OVF_TYPE, sh->evtag);
	else
	{
		sh->doc.col[t->col].val[sh->doc.grp[t->group].n - 1].i = b;
		leaf_set(sh, t, SIFT_SHRED_VALUE, 0, SIFT_COLTAG_UNKNOWN);
	}
}

static void
leaf_text(SiftShred *sh, NodeP t, const char *s, int len)
{
	SiftShredDoc *doc = &sh->doc;
	SiftShredVal *v = &doc->col[t->col].val[doc->grp[t->group].n - 1];

	if (!SIFT_SHRED_IS_TEXTUAL(t->type))
		leaf_overflow(sh, t, SIFT_SHRED_OVF_TYPE, sh->evtag);
	else if ((uint32) len > t->maxlen && !sh->any_length)
		leaf_overflow(sh, t, SIFT_SHRED_OVF_LENGTH, sh->evtag);
	else if ((uint64) doc->ntext + len > sh->limits.max_bytes)
		leaf_overflow(sh, t, SIFT_SHRED_OVF_TRUNC, sh->evtag);
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
		leaf_set(sh, t, SIFT_SHRED_VALUE, 0, text_kind(sh, t));
	}
}

void
sift_shred_string(SiftShred *sh, const char *s, int len)
{
	NodeP		t;

	sh->evtag = SIFT_COLTAG_STRING;
	t = take_target(sh);

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

	if (t->type == SIFT_SHRED_ASTEXT && text != NULL)
		leaf_text(sh, t, text, len);
	else if (!IS_NUMERIC(t->type) || text == NULL || !dec_parse(text, len, &d))
		leaf_overflow(sh, t, SIFT_SHRED_OVF_TYPE, sh->evtag);
	else if (t->type <= SIFT_SHRED_INT8 ? !(dec_integral(&d, &mag) && int_fits(t->type, d.neg, mag, &v->i))
			 : !dec_float(&d, text, len, t->type == SIFT_SHRED_FLOAT4, &v->f))
		leaf_overflow(sh, t, SIFT_SHRED_OVF_RANGE, sh->evtag);
	else
		leaf_set(sh, t, SIFT_SHRED_VALUE, 0, SIFT_COLTAG_UNKNOWN);
}

void
sift_shred_number(SiftShred *sh, const char *text, int len)
{
	NodeP		t;

	sh->evtag = SIFT_COLTAG_NUMBER;
	t = take_target(sh);

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
	SIFT_INVARIANT(sh->nframes == 0, "sift shred: finished with %d containers open", sh->nframes);
}

void
sift_shred_expand_parents(const SiftShredDoc *doc, int g, uint32 *parent)
{
	sift_colk->expand_parents(doc->grp[g].end, doc->grp[g].pn, parent);
}
