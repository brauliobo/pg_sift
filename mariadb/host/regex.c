/*
 * regex.c - pg_regcomp, pg_regexec, pg_regprefix, pg_regfree and pg_regerror (host.h) for the engine of src/
 * without the PostgreSQL backend: a backtracking matcher for the advanced regular expressions (AREs) that SiftQL
 * regex leaves compile to, "[(?flags)]^(?:PATTERN)$", with the semantics of PostgreSQL in the C collation:
 *
 *   - literals, ".", bracket expressions (ranges, [:class:], [.c.], [=c=], escapes), \d \D \s \S \w \W, \a \b \B \cX
 *     \e \f \n \r \t \v \xH.. \uHHHH \UHHHHHHHH \0 \ooo, anchors ^ $ \A \Z, word boundaries \m \M \y \Y [[:<:]] [[:>:]]
 *   - groups, (?:), lookahead (?= (?!, lookbehind (?<= (?<!, (?#comments), alternation, * + ? {n} {n,} {n,m} and
 *     their lazy forms, back-references, the leading embedded options (?cimnpstw)
 *   - the character properties and the case folding of the C locale: ASCII only, as PostgreSQL does for the C
 *     collation (a character above 127 is no letter, no digit, no space and has no other case)
 *
 * Not supported, and rejected with an error: the syntax options x (expanded), q (quoted), b and e (BRE, ERE), and
 * collating-element names of more than one character ([[.space.]]). Matching a pattern without back-references
 * is linear (a state is explored once, a lookaround is run once per position); with them it is bounded by a step
 * budget (RX_ESPACE).
 * All memory comes from palloc: the engine drops regexes with their memory context and seldom calls pg_regfree.
 * The parser is in regex_parse.c, the matcher in regex_exec.c; regex_int.h has what they share.
 */

#include "regex_int.h"

#define REMAGIC		0xfed7
#define MAXPROG		(1 << 16)			/* instructions */

static const char *const errtext[] = {
	"no errors detected", "failed to match", "invalid regular expression", "invalid collating element",
	"invalid character class", "invalid escape \\ sequence", "invalid backreference number",
	"brackets [] not balanced", "parentheses () not balanced", "braces {} not balanced",
	"invalid repetition count(s)", "invalid character range", "out of memory", "quantifier operand invalid",
	"unknown error", "\"cannot happen\" -- you found a bug", "invalid argument to regex function",
	"character widths of regex and string differ", "invalid embedded option", "regular expression is too complex",
	"too many colors"
};

/* ------------------------------------------------ code generation */

static int
emit(Comp *c, int op, int x, int y)
{
	if (c->nprog >= MAXPROG)
		fail(c, RX_ETOOBIG);
	GROW(c->prog, c->nprog, c->nprogcap);
	c->prog[c->nprog] = (Inst) {op, x, y};
	return c->nprog++;
}

/* Points the chain of instructions through their x (or y) field at to */
static void
patch(Comp *c, int head, int to, bool useY)
{
	while (head >= 0)
	{
		int		   *f = useY ? &c->prog[head].y : &c->prog[head].x;

		head = *f;
		*f = to;
	}
}

static bool
nullable(const Comp *c, int n)
{
	const Node *nd = &c->nodes[n];
	int			k;

	switch (nd->t)
	{
		case N_CHR: case N_ANY: case N_SET:
			return false;
		case N_CAT:
			for (k = nd->kid; k >= 0; k = c->nodes[k].next)
				if (!nullable(c, k))
					return false;
			return true;
		case N_ALT:
			for (k = nd->kid; k >= 0; k = c->nodes[k].next)
				if (nullable(c, k))
					return true;
			return false;
		case N_REP:
			return nd->min == 0 || nullable(c, nd->kid);
		case N_GROUP:
			return nullable(c, nd->kid);
		default:
			return true;
	}
}

static void gen(Comp *c, int n);

static void
gen_alt(Comp *c, const Node *nd)
{
	int			k,
				split,
				jmps = -1;

	for (k = nd->kid; k >= 0; k = c->nodes[k].next)
	{
		bool		last = c->nodes[k].next < 0;

		split = last ? -1 : emit(c, I_SPLIT, c->nprog + 1, -1);
		gen(c, k);
		if (last)
			break;
		jmps = emit(c, I_JMP, jmps, 0);
		c->prog[split].y = c->nprog;
	}
	patch(c, jmps, c->nprog, false);
}

/* x{min,max}: min copies, then max - min optional ones nested by jumps to the end, or a loop */
static void
gen_rep(Comp *c, const Node *nd)
{
	int			i,
				loop,
				reg,
				chk,
				chain = -1;
	bool		mark = (!c->memo || c->inlook) && nullable(c, nd->kid);

	/* a quantified back-reference needs its group set, even to repeat zero times */
	if (c->nodes[nd->kid].t == N_BACK && nd->min == 0 && nd->max != 0)
		emit(c, I_BACK, c->nodes[nd->kid].a, 1);
	for (i = 0; i < nd->min; i++)
		gen(c, nd->kid);
	if (nd->max >= 0)
	{
		for (i = nd->min; i < nd->max; i++)
		{
			chain = emit(c, I_SPLIT, nd->lazy ? chain : c->nprog + 1, nd->lazy ? c->nprog + 1 : chain);
			gen(c, nd->kid);
		}
		patch(c, chain, c->nprog, !nd->lazy);
		return;
	}
	/* a body that matched nothing ends the loop, else it never would */
	reg = 2 * (c->ncap + 1) + c->nregs;
	c->nregs += mark;
	loop = emit(c, I_SPLIT, 0, 0);
	if (mark)
		emit(c, I_MARK, reg, 0);
	gen(c, nd->kid);
	if (mark)
		chk = emit(c, I_CHK, reg, 0);
	emit(c, I_JMP, loop, 0);
	if (mark)
		c->prog[chk].y = c->nprog;
	c->prog[loop].x = nd->lazy ? c->nprog : loop + 1;
	c->prog[loop].y = nd->lazy ? loop + 1 : c->nprog;
}

static void
gen(Comp *c, int n)
{
	const Node *nd = &c->nodes[n];
	int			k,
				s;

	switch (nd->t)
	{
		case N_CHR: emit(c, c->icase ? I_CHARI : I_CHAR, c->icase ? lowc(nd->a) : nd->a, 0); break;
		case N_ANY: emit(c, I_ANY, c->nlstop, 0); break;
		case N_SET: emit(c, I_SET, nd->a, 0); break;
		case N_ASSERT: emit(c, I_ASSERT, nd->a, 0); break;
		case N_BACK: emit(c, I_BACK, nd->a, 0); break;
		case N_CAT:
			for (k = nd->kid; k >= 0; k = c->nodes[k].next)
				gen(c, k);
			break;
		case N_ALT: gen_alt(c, nd); break;
		case N_REP: gen_rep(c, nd); break;
		case N_GROUP:
			if (c->saves)
				emit(c, I_SAVE, 2 * nd->a, 0);
			gen(c, nd->kid);
			if (c->saves)
				emit(c, I_SAVE, 2 * nd->a + 1, 0);
			break;
		case N_LOOK:
			s = emit(c, I_LOOK, nd->a, 0);
			c->inlook++;
			gen(c, nd->kid);
			c->inlook--;
			emit(c, I_MATCH, 0, 0);
			c->prog[s].y = c->nprog;
			break;
	}
}

/* ------------------------------------------------ prefix */

typedef struct Pfx					/* every match of a node starts with w; whole: it matches nothing else */
{
	pg_wchar   *w;
	int			n;
	bool		whole;
} Pfx;

static Pfx
pcat(Pfx a, Pfx b)
{
	if (b.n)
	{
		a.w = a.n ? repalloc(a.w, (a.n + b.n) * sizeof(pg_wchar)) : palloc(b.n * sizeof(pg_wchar));
		memcpy(a.w + a.n, b.w, b.n * sizeof(pg_wchar));
		a.n += b.n;
		pfree(b.w);
	}
	a.whole = b.whole;
	return a;
}

static Pfx
pcommon(Pfx a, Pfx b)
{
	int			m = 0;

	while (m < a.n && m < b.n && a.w[m] == b.w[m])
		m++;
	a.whole = a.whole && b.whole && m == a.n && m == b.n;
	a.n = m;
	if (b.n)
		pfree(b.w);
	return a;
}

/* Assertions and lookarounds only narrow the matches, so they keep the prefix */
static Pfx
pfx(const Comp *c, int n)
{
	const Node *nd = &c->nodes[n];
	Pfx			r = {NULL, 0, false};
	int			i;

	switch (nd->t)
	{
		case N_EMPTY: case N_ASSERT: case N_LOOK:
			r.whole = true;
			break;
		case N_CHR:
			if (!c->icase || flip(nd->a) == nd->a)
			{
				r.w = palloc(sizeof(pg_wchar));
				r.w[0] = nd->a;
				r.n = 1;
				r.whole = true;
			}
			break;
		case N_GROUP:
			return pfx(c, nd->kid);
		case N_REP:
			if (nd->min > 0)
				r = pfx(c, nd->kid);
			r.whole = false;
			break;
		case N_CAT:
			r.whole = true;
			for (i = nd->kid; r.whole && i >= 0; i = c->nodes[i].next)
				r = pcat(r, pfx(c, i));
			break;
		case N_ALT:
			r = pfx(c, nd->kid);
			for (i = c->nodes[nd->kid].next; i >= 0; i = c->nodes[i].next)
				r = pcommon(r, pfx(c, i));
			break;
	}
	return r;
}

/* The node can only match at the start (end) of the string */
static bool
anchored(const Comp *c, int n, bool end)
{
	const Node *nd = &c->nodes[n];
	int			k;

	switch (nd->t)
	{
		case N_ASSERT:
			return nd->a == (end ? A_EOL : A_BOL);
		case N_GROUP:
			return anchored(c, nd->kid, end);
		case N_REP:
			return nd->min > 0 && anchored(c, nd->kid, end);
		case N_CAT:
			for (k = nd->kid; end && c->nodes[k].next >= 0;)
				k = c->nodes[k].next;
			return anchored(c, k, end);
		case N_ALT:
			for (k = nd->kid; k >= 0; k = c->nodes[k].next)
				if (!anchored(c, k, end))
					return false;
			return true;
		default:
			return false;
	}
}

/* ------------------------------------------------ interface */

static void
drop(void *p)
{
	if (p)
		pfree(p);
}

static void
release(Comp *c)
{
	drop(c->nodes);
	drop(c->closed);
	drop(c->sets);
	drop(c->rng);
	drop(c->prog);
	pfree(c);
}

static Rx *
finish(Comp *c, int root)
{
	Rx		   *x = palloc0(sizeof(Rx));
	Pfx			p = {NULL, 0, false};

	x->prog = c->prog;
	x->nprog = c->nprog;
	x->sets = c->sets;
	x->rng = c->rng;
	x->icase = c->icase;
	x->nlstop = c->nlstop;
	x->nlanch = c->nlanch;
	x->memo = c->memo;
	x->nslots = 2 * (c->ncap + 1) + c->nregs;
	x->slots = palloc(x->nslots * sizeof(int));
	x->stkcap = 64;
	x->stk = palloc(x->stkcap * sizeof(Ent));
	c->prog = NULL;
	c->sets = NULL;
	c->rng = NULL;
	if (!c->nlanch && anchored(c, root, false))
		p = pfx(c, root);
	x->pfx = p.w;
	x->npfx = p.n;
	x->exact = p.whole && anchored(c, root, true);
	return x;
}

int
pg_regcomp(regex_t *re, const pg_wchar *string, size_t len, int flags, Oid collation)
{
	Comp	   *c;
	Rx		   *x;
	int			root;

	if (!(flags & REG_ADVANCED))
		return RX_INVARG;
	c = palloc0(sizeof(Comp));
	if (setjmp(c->jb))
	{
		int			err = c->err;

		release(c);
		return err;
	}
	c->s = string;
	c->end = string + len;
	c->nosub = flags & REG_NOSUB;
	options(c);
	root = alt(c);
	if (c->s < c->end)
		fail(c, RX_EPAREN);
	c->memo = !c->backref;
	c->saves = c->backref || !c->nosub;
	gen(c, root);
	emit(c, I_MATCH, 0, 0);
	x = finish(c, root);
	re->magic = REMAGIC;
	re->posix = x;
	re->re_info = c->backref ? REG_UBACKREF : 0;
	release(c);
	return REG_OKAY;
}

int
pg_regprefix(regex_t *re, pg_wchar **string, size_t *slength)
{
	Rx		   *x = re->posix;

	if (x->npfx == 0)
		return REG_NOMATCH;
	*string = memcpy(palloc(x->npfx * sizeof(pg_wchar)), x->pfx, x->npfx * sizeof(pg_wchar));
	*slength = x->npfx;
	return x->exact ? REG_EXACT : REG_PREFIX;
}

void
pg_regfree(regex_t *re)
{
	Rx		   *x = re->posix;

	drop(x->prog);
	drop(x->sets);
	drop(x->rng);
	drop(x->slots);
	drop(x->stk);
	drop(x->seen);
	drop(x->pfx);
	pfree(x);
	re->magic = 0;
	re->posix = NULL;
}

size_t
pg_regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
	const char *msg = errcode >= 0 && errcode <= RX_ECOLORS ? errtext[errcode] : "unknown error";

	if (errbuf_size > 0)
		snprintf(errbuf, errbuf_size, "%s", msg);
	return strlen(msg) + 1;
}
