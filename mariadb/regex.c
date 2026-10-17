/*
 * regex.c - pg_regcomp, pg_regexec, pg_regprefix, pg_regfree and pg_regerror (regex/regex.h) for the engine of src/
 * without the PostgreSQL backend: a backtracking matcher for the advanced regular expressions (AREs) that SiftQL
 * regex leaves compile to, "[(?flags)]^(?:PATTERN)$", with the semantics of PostgreSQL in the C collation:
 *
 *   - literals, ".", bracket expressions (ranges, [:class:], [.c.], [=c=], escapes), \d \D \s \S \w \W, \a \b \B \cX
 *     \e \f \n \r \t \v \xH.. \uHHHH \UHHHHHHHH \0 \ooo, anchors ^ $ \A \Z, word boundaries \m \M \y \Y [[:<:]] [[:>:]]
 *   - groups, (?:), lookahead (?= (?!, lookbehind (?<= (?<!, (?#comments), alternation, * + ? {n} {n,} {n,m} and
 *     their lazy forms, back-references, the leading embedded options (?cimnpstw)
 *   - the character attributes and the case folding of the C locale: ASCII only, as PostgreSQL does for the C
 *     collation (a character above 127 is no letter, no digit, no space and has no other case)
 *
 * Not supported, and rejected with an error: the syntax options x (expanded), q (quoted), b and e (BRE, ERE), and
 * collating-element names of more than one character ([[.space.]]). Matching a pattern without back-references
 * is linear (a state is explored once, a lookaround is run once per position); with them it is bounded by a step
 * budget (REG_ESPACE).
 * All memory comes from palloc: the engine drops regexes with their memory context and seldom calls pg_regfree.
 */
#include "postgres.h"

#include <setjmp.h>

#include "regex/regex.h"

#define REMAGIC		0xfed7
#define CHR_MAX		0x7ffffffe
#define DUPMAX		255
#define MAXDEPTH	100					/* nesting of groups */
#define MAXPROG		(1 << 16)			/* instructions */
#define MAXSTACK	(1 << 20)			/* backtrack entries */
#define MAXMEMO		((size_t) 1 << 28)	/* bits of the visited set */
#define BUDGET		5000000				/* steps of one pg_regexec */
#define E_BACK		(-1000)				/* escape() result: a back-reference, its number in Comp.num */
#define R_FAIL		(-1)
#define R_BUDGET	(-2)

#define GROW(a, n, cap) \
	do { \
		if ((n) >= (cap)) \
		{ \
			(cap) = (cap) ? 2 * (cap) : 16; \
			(a) = (a) ? repalloc((a), (cap) * sizeof(*(a))) : palloc((cap) * sizeof(*(a))); \
		} \
	} while (0)

enum {N_EMPTY, N_CHR, N_ANY, N_SET, N_ASSERT, N_BACK, N_CAT, N_ALT, N_REP, N_GROUP, N_LOOK};
enum {A_WBEG, A_WEND, A_WB, A_NWB, A_BOS, A_EOS, A_BOL, A_EOL};	/* \m \M \y \Y \A \Z ^ $ */
enum {I_CHAR, I_CHARI, I_ANY, I_SET, I_ASSERT, I_BACK, I_SPLIT, I_JMP, I_SAVE, I_MARK, I_CHK, I_LOOK, I_MATCH};
enum {CC_ALNUM, CC_ALPHA, CC_ASCII, CC_BLANK, CC_CNTRL, CC_DIGIT, CC_GRAPH, CC_LOWER, CC_PRINT, CC_PUNCT, CC_SPACE,
	  CC_UPPER, CC_XDIGIT, CC_WORD, NCC};

static const char *const ccname[NCC] = {"alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower",
										"print", "punct", "space", "upper", "xdigit", "word"};

static const char *const errtext[] = {
	"no errors detected", "failed to match", "invalid regular expression", "invalid collating element",
	"invalid character class", "invalid escape \\ sequence", "invalid backreference number",
	"brackets [] not balanced", "parentheses () not balanced", "braces {} not balanced",
	"invalid repetition count(s)", "invalid character range", "out of memory", "quantifier operand invalid",
	"unknown error", "\"cannot happen\" -- you found a bug", "invalid argument to regex function",
	"character widths of regex and string differ", "invalid embedded option", "regular expression is too complex",
	"too many colors"
};

typedef struct Node					/* the syntax tree; children are a list through next */
{
	int			t, a, min, max, kid, next;	/* a: char, set, assertion, group, look kind (bit 0 negative, 1 behind) */
	bool		lazy;
} Node;

typedef struct Set					/* a bracket expression or a class escape */
{
	bool		neg;
	uint32		cls, ncls;			/* classes that match, classes whose complement matches */
	int			r0, nr;				/* its [lo, hi] pairs in Rx.rng */
} Set;

typedef struct Inst
{
	int			op, x, y;
} Inst;

typedef struct Ent					/* backtrack stack: a = pc and b = pos, or a < 0: restore slot -1 - a to b */
{
	int			a, b;
} Ent;

typedef struct Rx					/* re_guts */
{
	Inst	   *prog;
	int			nprog;
	Set		   *sets;
	pg_wchar   *rng;
	bool		icase, nlstop, nlanch;
	bool		memo;				/* no back-references: a (pc, pos) state of the main run is explored once */
	int			nslots;				/* capture slots, then the loop marks */
	int		   *slots;
	Ent		   *stk;
	int			sp, stkcap;
	uint8	   *seen;
	size_t		seencap;
	int			len;
	long		steps;
	int			nest;				/* lookaround runs in progress */
	pg_wchar   *pfx;
	int			npfx;
	bool		exact;
} Rx;

typedef struct Comp
{
	jmp_buf		jb;
	int			err;
	const pg_wchar *s, *end;
	bool		icase, nlstop, nlanch, nosub, backref, haslook, memo, saves;
	int			depth, nolook, inlook, ncap, num, nregs;
	Node	   *nodes;
	int			nn, nncap;
	Set		   *sets;
	int			nsets, nsetcap;
	pg_wchar   *rng;
	int			nrng, nrngcap;
	bool	   *closed;				/* closed[n]: capture group n is complete, so \n may refer to it */
	int			nclosedcap;
	Inst	   *prog;
	int			nprog, nprogcap;
} Comp;

static void fail(Comp *c, int err) __attribute__((noreturn));

static void
fail(Comp *c, int err)
{
	c->err = err;
	longjmp(c->jb, 1);
}

/* ------------------------------------------------ characters */

static inline pg_wchar
lowc(pg_wchar c)
{
	return c - 'A' < 26u ? c + 32 : c;
}

static inline pg_wchar
flip(pg_wchar c)
{
	return (c | 32) - 'a' < 26u ? c ^ 32 : c;
}

static bool
ccis(int k, pg_wchar c)
{
	bool		dig = c - '0' < 10u,
				low = c - 'a' < 26u,
				up = c - 'A' < 26u;

	switch (k)
	{
		case CC_ALNUM: return dig || low || up;
		case CC_ALPHA: return low || up;
		case CC_ASCII: return c < 128;
		case CC_BLANK: return c == ' ' || c == '\t';
		case CC_CNTRL: return c < 32 || c == 127;
		case CC_DIGIT: return dig;
		case CC_GRAPH: return c > 32 && c < 127;
		case CC_LOWER: return low;
		case CC_PRINT: return c >= 32 && c < 127;
		case CC_PUNCT: return c > 32 && c < 127 && !dig && !low && !up;
		case CC_SPACE: return c == ' ' || c - 9 < 5u;
		case CC_UPPER: return up;
		case CC_XDIGIT: return dig || (c | 32) - 'a' < 6u;
		default: return dig || low || up || c == '_';
	}
}

static bool
inrng(const Rx *x, const Set *st, pg_wchar ch)
{
	int			i,
				k;

	for (i = 0; i < st->nr; i++)
		if (ch >= x->rng[2 * (st->r0 + i)] && ch <= x->rng[2 * (st->r0 + i) + 1])
			return true;
	if (st->cls | st->ncls)
		for (k = 0; k < NCC; k++)
			if ((st->cls >> k & 1 && ccis(k, ch)) || (st->ncls >> k & 1 && !ccis(k, ch)))
				return true;
	return false;
}

static bool
inset(const Rx *x, const Set *st, pg_wchar ch)
{
	bool		in = inrng(x, st, ch) || (x->icase && flip(ch) != ch && inrng(x, st, flip(ch)));

	return st->neg ? !in && !(x->nlstop && ch == '\n') : in;
}

/* ------------------------------------------------ parser */

static int
mk(Comp *c, int t, int a, int kid)
{
	GROW(c->nodes, c->nn, c->nncap);
	c->nodes[c->nn] = (Node) {.t = t, .a = a, .kid = kid, .next = -1};
	return c->nn++;
}

static int
cur(const Comp *c)
{
	return c->s < c->end ? (int) *c->s : -1;
}

/* The next character, after (?#comments) */
static int
look(Comp *c)
{
	while (c->end - c->s >= 3 && c->s[0] == '(' && c->s[1] == '?' && c->s[2] == '#')
	{
		for (c->s += 3; c->s < c->end && *c->s != ')'; c->s++)
			;
		if (c->s < c->end)
			c->s++;
	}
	return cur(c);
}

static bool
isdig(int ch)
{
	return ch >= '0' && ch <= '9';
}

static bool
isalpha_(int ch)
{
	return ch >= 0 && ccis(CC_ALPHA, ch);
}

static long
digits(Comp *c, int base, int min, int max)
{
	long		v = 0;
	int			n = 0;

	for (; n < max && c->s < c->end; n++, c->s++)
	{
		pg_wchar	ch = *c->s;
		int			d = isdig(ch) ? ch - '0' : (ch | 32) - 'a' < 6u ? (int) ((ch | 32) - 'a') + 10 : 99;

		if (d >= base)
			break;
		v = v * base + d;
		if (v > CHR_MAX)
			fail(c, REG_EESCAPE);
	}
	if (n < min)
		fail(c, REG_EESCAPE);
	return v;
}

/*
 * After a backslash: the character it stands for, -(letter) for the class (dDsSwW) and assertion (mMyYAZ) escapes,
 * E_BACK for a back-reference. A number is a back-reference when it is a single digit or names a group opened so far,
 * else an octal character code.
 */
static long
escape(Comp *c, bool inbr)
{
	pg_wchar	ch;
	const pg_wchar *save;
	long		n;

	if (c->s >= c->end)
		fail(c, REG_EESCAPE);
	switch (ch = *c->s++)
	{
		case 'a': return 7;
		case 'b': return 8;
		case 'B': return '\\';
		case 'e': return 27;
		case 'f': return 12;
		case 'n': return 10;
		case 'r': return 13;
		case 't': return 9;
		case 'v': return 11;
		case 'c':
			if (c->s >= c->end)
				fail(c, REG_EESCAPE);
			return *c->s++ & 037;
		case 'x': return digits(c, 16, 1, 255);
		case 'u': return digits(c, 16, 4, 4);
		case 'U': return digits(c, 16, 8, 8);
		case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
			return -(long) ch;
		case 'm': case 'M': case 'y': case 'Y': case 'A': case 'Z':
			if (inbr)
				fail(c, REG_EESCAPE);
			return -(long) ch;
		case '0':
			c->s--;
			return digits(c, 8, 1, 3);
		case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
			save = c->s;
			c->s--;
			n = digits(c, 10, 1, 255);
			if (c->s == save || n <= c->ncap)
			{
				if (inbr)
					fail(c, REG_EESCAPE);
				c->num = n;
				return E_BACK;
			}
			c->s = save - 1;
			return digits(c, 8, 1, 3);
		default:
			if (isalpha_(ch))
				fail(c, REG_EESCAPE);
			return ch;
	}
}

static int
newset(Comp *c)
{
	GROW(c->sets, c->nsets, c->nsetcap);
	c->sets[c->nsets] = (Set) {.r0 = c->nrng};
	return c->nsets++;
}

static void
addrange(Comp *c, pg_wchar lo, pg_wchar hi)
{
	GROW(c->rng, 2 * c->nrng + 1, c->nrngcap);
	c->rng[2 * c->nrng] = lo;
	c->rng[2 * c->nrng + 1] = hi;
	c->nrng++;
	c->sets[c->nsets - 1].nr++;
}

/* The class bit of \d \s \w and its complement escapes, in the last set */
static void
addclass(Comp *c, int letter)
{
	Set		   *st = &c->sets[c->nsets - 1];
	uint32		bit = 1u << (lowc(letter) == 'd' ? CC_DIGIT : lowc(letter) == 's' ? CC_SPACE : CC_WORD);

	if (letter >= 'a')
		st->cls |= bit;
	else
		st->ncls |= bit;
}

static int
classid(const pg_wchar *w, int n)
{
	int			k,
				i;

	for (k = 0; k < NCC; k++)
	{
		for (i = 0; i < n && w[i] == (pg_wchar) ccname[k][i]; i++)
			;
		if (i == n && !ccname[k][n])
			return k;
	}
	return -1;
}

/*
 * One bracket element, c->s at its first character: its character, or -1 for a class that was added to the last set.
 * A range end (endpoint) cannot be a class.
 */
static long
belem(Comp *c, bool endpoint)
{
	int			ch = cur(c);
	long		v;

	if (ch == '[' && c->s + 1 < c->end && (c->s[1] == ':' || c->s[1] == '.' || c->s[1] == '='))
	{
		pg_wchar	kind = c->s[1];
		const pg_wchar *name = c->s + 2,
				   *p = name;
		int			n,
					k;

		while (p + 1 < c->end && !(p[0] == kind && p[1] == ']'))
			p++;
		if (p + 1 >= c->end)
			fail(c, REG_EBRACK);
		n = p - name;
		c->s = p + 2;
		if (kind == ':')
		{
			if ((k = classid(name, n)) < 0)
				fail(c, REG_ECTYPE);
			if (endpoint)
				fail(c, REG_ERANGE);
			c->sets[c->nsets - 1].cls |= 1u << k;
			return -1;
		}
		if (n != 1)
			fail(c, REG_ECOLLATE);
		if (kind == '=')
		{
			if (endpoint)
				fail(c, REG_ERANGE);
			addrange(c, name[0], name[0]);
			return -1;
		}
		return name[0];
	}
	c->s++;
	if (ch != '\\')
		return ch;
	if ((v = escape(c, true)) >= 0)
		return v;
	if (endpoint)
		fail(c, REG_ERANGE);
	addclass(c, -v);
	return -1;
}

static int
bracket(Comp *c)
{
	int			si = newset(c);
	bool		first = true;

	if (cur(c) == '^')
	{
		c->sets[si].neg = true;
		c->s++;
	}
	for (;;)
	{
		int			ch = cur(c);
		long		lo,
					hi;

		if (ch < 0)
			fail(c, REG_EBRACK);
		if (ch == ']' && !first)
		{
			c->s++;
			return mk(c, N_SET, si, -1);
		}
		if (ch == '-' && !first && c->s + 1 < c->end && c->s[1] != ']')
			fail(c, REG_ERANGE);
		first = false;
		if ((lo = belem(c, false)) < 0)
			continue;
		hi = lo;
		if (cur(c) == '-' && c->s + 1 < c->end && c->s[1] != ']')
		{
			c->s++;
			if ((hi = belem(c, true)) < lo)
				fail(c, REG_ERANGE);
		}
		addrange(c, lo, hi);
	}
}

static int
classnode(Comp *c, int letter)
{
	int			si = newset(c);

	addclass(c, letter);
	return mk(c, N_SET, si, -1);
}

static int
escape_atom(Comp *c, bool *quant)
{
	long		v = escape(c, false);

	if (v >= 0)
		return mk(c, N_CHR, v, -1);
	if (v == E_BACK)
	{
		if (c->nolook || c->num > c->ncap || !c->closed[c->num])
			fail(c, REG_ESUBREG);
		c->backref = true;
		return mk(c, N_BACK, c->num, -1);
	}
	if (strchr("dDsSwW", -v))
		return classnode(c, -v);
	*quant = false;
	return mk(c, N_ASSERT, strchr("mMyYAZ", -v) - "mMyYAZ", -1);
}

static int alt(Comp *c);

/* After "(": a group, "(?:", or a lookaround; the lookarounds take no quantifier and their groups do not capture */
static int
group(Comp *c, bool *quant)
{
	int			lk = -1,
				cap = 0,
				kid;

	if (cur(c) == '?')
	{
		int			ch;

		if (++c->s >= c->end)
			fail(c, REG_BADRPT);
		switch (ch = *c->s++)
		{
			case ':': break;
			case '=': lk = 0; break;
			case '!': lk = 1; break;
			case '<':
				if (c->s >= c->end || (*c->s != '=' && *c->s != '!'))
					fail(c, REG_BADRPT);
				lk = *c->s++ == '=' ? 2 : 3;
				break;
			default: fail(c, REG_BADRPT);
		}
	}
	else if (!c->nolook)
	{
		cap = ++c->ncap;
		GROW(c->closed, cap, c->nclosedcap);
		c->closed[cap] = false;
	}
	c->nolook += lk >= 0;
	c->haslook |= lk >= 0;
	kid = alt(c);
	if (look(c) != ')')
		fail(c, REG_EPAREN);
	c->s++;
	c->nolook -= lk >= 0;
	if (lk >= 0)
	{
		*quant = false;
		return mk(c, N_LOOK, lk, kid);
	}
	if (!cap)
		return kid;
	c->closed[cap] = true;
	return mk(c, N_GROUP, cap, kid);
}

static int
atom(Comp *c, bool *quant)
{
	int			ch = look(c);

	*quant = true;
	c->s++;
	switch (ch)
	{
		case '(': return group(c, quant);
		case '.': return mk(c, N_ANY, 0, -1);
		case '^': case '$':
			*quant = false;
			return mk(c, N_ASSERT, ch == '^' ? A_BOL : A_EOL, -1);
		case '\\': return escape_atom(c, quant);
		case '[':
			if (c->end - c->s >= 6 && c->s[0] == '[' && c->s[1] == ':' && (c->s[2] == '<' || c->s[2] == '>') &&
				c->s[3] == ':' && c->s[4] == ']' && c->s[5] == ']')
			{
				*quant = false;
				ch = c->s[2];
				c->s += 6;
				return mk(c, N_ASSERT, ch == '<' ? A_WBEG : A_WEND, -1);
			}
			return bracket(c);
		default:
			if (ch == '*' || ch == '+' || ch == '?' || (ch == '{' && isdig(cur(c))))
				fail(c, REG_BADRPT);
			return mk(c, N_CHR, ch, -1);
	}
}

static int
number(Comp *c)
{
	int			n = 0;

	for (; isdig(cur(c)); c->s++)
		if ((n = n * 10 + (*c->s - '0')) > DUPMAX)
			fail(c, REG_BADBR);
	return n;
}

static bool
quantstart(Comp *c)
{
	int			ch = look(c);

	return ch == '*' || ch == '+' || ch == '?' || (ch == '{' && c->s + 1 < c->end && isdig(c->s[1]));
}

/* A quantifier after an atom: its bounds (max < 0: no limit) and laziness */
static bool
quantifier(Comp *c, int *min, int *max, bool *lazy)
{
	int			ch;

	if (!quantstart(c))
		return false;
	ch = *c->s++;
	*min = ch == '+';
	*max = ch == '?' ? 1 : -1;
	if (ch == '{')
	{
		*max = *min = number(c);
		if (cur(c) == ',')
		{
			c->s++;
			*max = isdig(cur(c)) ? number(c) : -1;
			if (*max >= 0 && *max < *min)
				fail(c, REG_BADBR);
		}
		if (cur(c) != '}')
			fail(c, REG_BADBR);
		c->s++;
	}
	if ((*lazy = look(c) == '?'))
		c->s++;
	if (quantstart(c))
		fail(c, REG_BADRPT);
	return true;
}

static int
piece(Comp *c)
{
	bool		quant,
				lazy;
	int			min,
				max,
				n = atom(c, &quant);

	if (!quantifier(c, &min, &max, &lazy))
		return n;
	if (!quant)
		fail(c, REG_BADRPT);
	n = mk(c, N_REP, 0, n);
	c->nodes[n].min = min;
	c->nodes[n].max = max;
	c->nodes[n].lazy = lazy;
	return n;
}

/* A node holding the list first.. as its children, or the node itself when it is alone */
static int
wrap(Comp *c, int t, int first)
{
	return first < 0 ? mk(c, N_EMPTY, 0, -1) : c->nodes[first].next < 0 ? first : mk(c, t, 0, first);
}

static int
cat(Comp *c)
{
	int			first = -1,
				last = -1,
				ch;

	while ((ch = look(c)) >= 0 && ch != '|' && ch != ')')
	{
		int			n = piece(c);

		if (last < 0)
			first = n;
		else
			c->nodes[last].next = n;
		last = n;
	}
	return wrap(c, N_CAT, first);
}

static int
alt(Comp *c)
{
	int			first,
				last;

	if (++c->depth > MAXDEPTH)
		fail(c, REG_ETOOBIG);
	first = last = cat(c);
	while (look(c) == '|')
	{
		int			n;

		c->s++;
		n = cat(c);
		c->nodes[last].next = n;
		last = n;
	}
	c->depth--;
	return wrap(c, N_ALT, first);
}

/* The embedded options at the very start; the case and newline ones only */
static void
options(Comp *c)
{
	if (!(c->end - c->s >= 3 && c->s[0] == '(' && c->s[1] == '?' && isalpha_(c->s[2])))
		return;
	for (c->s += 2; c->s < c->end && isalpha_(*c->s); c->s++)
		switch (*c->s)
		{
			case 'c': c->icase = false; break;
			case 'i': c->icase = true; break;
			case 'm': case 'n': c->nlstop = c->nlanch = true; break;
			case 'p': c->nlstop = true; c->nlanch = false; break;
			case 's': c->nlstop = c->nlanch = false; break;
			case 't': break;
			case 'w': c->nlstop = false; c->nlanch = true; break;
			default: fail(c, REG_BADOPT);
		}
	if (cur(c) != ')')
		fail(c, REG_BADOPT);
	c->s++;
}

/* ------------------------------------------------ code generation */

static int
emit(Comp *c, int op, int x, int y)
{
	if (c->nprog >= MAXPROG)
		fail(c, REG_ETOOBIG);
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

	if ((flags & (REG_ADVANCED | REG_QUOTE | REG_EXPANDED)) != REG_ADVANCED)
		return REG_INVARG;
	c = palloc0(sizeof(Comp));
	if (setjmp(c->jb))
	{
		int			err = c->err;

		release(c);
		return err;
	}
	c->s = string;
	c->end = string + len;
	c->icase = flags & REG_ICASE;
	c->nlstop = flags & REG_NLSTOP;
	c->nlanch = flags & REG_NLANCH;
	c->nosub = flags & REG_NOSUB;
	options(c);
	root = alt(c);
	if (c->s < c->end)
		fail(c, REG_EPAREN);
	c->memo = !c->backref;
	c->saves = c->backref || !c->nosub;
	gen(c, root);
	emit(c, I_MATCH, 0, 0);
	x = finish(c, root);
	re->re_magic = REMAGIC;
	re->re_nsub = c->ncap;
	re->re_info = (c->backref ? REG_UBACKREF : 0) | (c->haslook ? REG_ULOOKAROUND : 0);
	re->re_csize = sizeof(pg_wchar);
	re->re_endp = NULL;
	re->re_collation = collation;
	re->re_guts = (char *) x;
	re->re_fns = NULL;
	release(c);
	return REG_OKAY;
}

/* ------------------------------------------------ matching */

static bool
push(Rx *x, int a, int b)
{
	if (x->sp == x->stkcap)
	{
		if (x->stkcap >= MAXSTACK)
			return false;
		x->stkcap *= 2;
		x->stk = repalloc(x->stk, x->stkcap * sizeof(Ent));
	}
	x->stk[x->sp++] = (Ent) {a, b};
	return true;
}

/* Marks the state visited; true when it was */
static bool
visited(Rx *x, int pc, int pos)
{
	size_t		bit = (size_t) pc * (x->len + 1) + pos;
	uint8		m = 1 << (bit & 7);
	bool		was = x->seen[bit >> 3] & m;

	x->seen[bit >> 3] |= m;
	return was;
}

static bool
assertion(const Rx *x, int kind, const pg_wchar *s, int len, int pos)
{
	bool		prev = pos > 0 && ccis(CC_WORD, s[pos - 1]),
				next = pos < len && ccis(CC_WORD, s[pos]);

	switch (kind)
	{
		case A_WBEG: return !prev && next;
		case A_WEND: return prev && !next;
		case A_WB: return prev != next;
		case A_NWB: return prev == next;
		case A_BOS: return pos == 0;
		case A_EOS: return pos == len;
		case A_BOL: return pos == 0 || (x->nlanch && s[pos - 1] == '\n');
		default: return pos == len || (x->nlanch && s[pos] == '\n');
	}
}

static long run(Rx *x, const pg_wchar *s, int len, int pc, int pos, int target);

/* 1 when the lookaround at pc holds at pos (behind: some match of its body ends there), 0, or R_BUDGET */
static long
look_holds(Rx *x, const pg_wchar *s, int len, int pc, int pos)
{
	long		r = R_FAIL;
	int			j;

	x->nest++;
	if (!(x->prog[pc].x & 2))
		r = run(x, s, len, pc + 1, pos, -1);
	else
		for (j = pos; j >= 0 && r == R_FAIL; j--)
			r = run(x, s, len, pc + 1, j, pos);
	x->nest--;
	return r == R_BUDGET ? r : r >= 0;
}

/*
 * Runs the program from pc at pos until I_MATCH (at target, when it is not negative), backtracking through the
 * stack above its current top. Returns the end of the match, R_FAIL or R_BUDGET.
 */
static long
run(Rx *x, const pg_wchar *s, int len, int pc, int pos, int target)
{
	int			base = x->sp;

	for (;;)
	{
		const Inst *in = &x->prog[pc];

		if (++x->steps > BUDGET)
			return R_BUDGET;
		if (x->memo && !x->nest && visited(x, pc, pos))
			goto fail;
		switch (in->op)
		{
			case I_CHAR:
				if (pos < len && s[pos] == (pg_wchar) in->x)
					goto next;
				break;
			case I_CHARI:
				if (pos < len && lowc(s[pos]) == (pg_wchar) in->x)
					goto next;
				break;
			case I_ANY:
				if (pos < len && !(in->x && s[pos] == '\n'))
					goto next;
				break;
			case I_SET:
				if (pos < len && inset(x, &x->sets[in->x], s[pos]))
					goto next;
				break;
			case I_ASSERT:
				if (assertion(x, in->x, s, len, pos))
				{
					pc++;
					continue;
				}
				break;
			case I_BACK:
			{
				int			b = x->slots[2 * in->x],
							n = in->y ? 0 : x->slots[2 * in->x + 1] - b,
							i = 0;

				if (b < 0 || pos + n > len)
					break;
				while (i < n && (x->icase ? lowc(s[b + i]) == lowc(s[pos + i]) : s[b + i] == s[pos + i]))
					i++;
				if (i < n)
					break;
				pos += n;
				pc++;
				continue;
			}
			case I_SPLIT:
				if (!push(x, in->y, pos))
					return R_BUDGET;
				pc = in->x;
				continue;
			case I_JMP:
				pc = in->x;
				continue;
			case I_SAVE: case I_MARK:
				if (!push(x, -1 - in->x, x->slots[in->x]))
					return R_BUDGET;
				x->slots[in->x] = pos;
				pc++;
				continue;
			case I_CHK:
				pc = x->slots[in->x] != pos ? pc + 1 : in->y;
				continue;
			case I_LOOK:
			{
				long		r = look_holds(x, s, len, pc, pos);

				if (r == R_BUDGET)
					return r;
				if (r == !(in->x & 1))
				{
					pc = in->y;
					continue;
				}
				break;
			}
			case I_MATCH:
				if (target >= 0 && pos != target)
					break;
				x->sp = base;
				return pos;
		}
fail:
		for (;;)
		{
			Ent			e;

			if (x->sp == base)
				return R_FAIL;
			e = x->stk[--x->sp];
			if (e.a >= 0)
			{
				pc = e.a;
				pos = e.b;
				break;
			}
			x->slots[-1 - e.a] = e.b;
		}
		continue;
next:
		pos++;
		pc++;
	}
}

int
pg_regexec(regex_t *re, const pg_wchar *string, size_t len, size_t search_start, rm_detail_t *details,
		   size_t nmatch, regmatch_t pmatch[], int flags)
{
	Rx		   *x = (Rx *) re->re_guts;
	size_t		bits = (size_t) x->nprog * (len + 1),
				st,
				i;
	long		end = R_FAIL;

	x->len = len;
	x->sp = 0;
	x->steps = 0;
	x->nest = 0;
	memset(x->slots, -1, x->nslots * sizeof(int));
	if (x->memo)
	{
		if (bits > MAXMEMO)
			return REG_ESPACE;
		if ((bits + 7) / 8 > x->seencap)
		{
			x->seencap = (bits + 7) / 8;
			x->seen = x->seen ? repalloc(x->seen, x->seencap) : palloc(x->seencap);
		}
		memset(x->seen, 0, (bits + 7) / 8);
	}
	for (st = search_start; st <= len && end < 0; st++)
		if ((end = run(x, string, len, 0, st, -1)) == R_BUDGET)
			return REG_ESPACE;
	if (end < 0)
		return REG_NOMATCH;
	for (i = 0; i < nmatch; i++)
	{
		bool		set = i > 0 && i * 2 + 1 < (size_t) x->nslots && x->slots[2 * i] >= 0 && x->slots[2 * i + 1] >= 0;

		pmatch[i].rm_so = i == 0 ? st - 1 : set ? x->slots[2 * i] : -1;
		pmatch[i].rm_eo = i == 0 ? end : set ? x->slots[2 * i + 1] : -1;
	}
	return REG_OKAY;
}

int
pg_regprefix(regex_t *re, pg_wchar **string, size_t *slength)
{
	Rx		   *x = (Rx *) re->re_guts;

	if (x->npfx == 0)
		return REG_NOMATCH;
	*string = memcpy(palloc(x->npfx * sizeof(pg_wchar)), x->pfx, x->npfx * sizeof(pg_wchar));
	*slength = x->npfx;
	return x->exact ? REG_EXACT : REG_PREFIX;
}

void
pg_regfree(regex_t *re)
{
	Rx		   *x = (Rx *) re->re_guts;

	drop(x->prog);
	drop(x->sets);
	drop(x->rng);
	drop(x->slots);
	drop(x->stk);
	drop(x->seen);
	drop(x->pfx);
	pfree(x);
	re->re_magic = 0;
	re->re_guts = NULL;
}

size_t
pg_regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
	const char *msg = errcode >= 0 && errcode <= REG_ECOLORS ? errtext[errcode] : "unknown error";

	if (errbuf_size > 0)
		snprintf(errbuf, errbuf_size, "%s", msg);
	return strlen(msg) + 1;
}
