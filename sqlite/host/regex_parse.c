/*
 * regex_parse.c - the parser of the regex engine (regex.c): bracket expressions, escapes, groups, quantifiers and the
 * leading embedded options, building the syntax tree of a Comp.
 */
#include "regex_int.h"

#define CHR_MAX		0x7ffffffe
#define DUPMAX		255
#define MAXDEPTH	100					/* nesting of groups */
#define E_BACK		(-1000)				/* escape() result: a back-reference, its number in Comp.num */

static const char *const ccname[NCC] = {"alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower",
										"print", "punct", "space", "upper", "xdigit", "word"};

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
			fail(c, RX_EESCAPE);
	}
	if (n < min)
		fail(c, RX_EESCAPE);
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
		fail(c, RX_EESCAPE);
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
				fail(c, RX_EESCAPE);
			return *c->s++ & 037;
		case 'x': return digits(c, 16, 1, 255);
		case 'u': return digits(c, 16, 4, 4);
		case 'U': return digits(c, 16, 8, 8);
		case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
			return -(long) ch;
		case 'm': case 'M': case 'y': case 'Y': case 'A': case 'Z':
			if (inbr)
				fail(c, RX_EESCAPE);
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
					fail(c, RX_EESCAPE);
				c->num = n;
				return E_BACK;
			}
			c->s = save - 1;
			return digits(c, 8, 1, 3);
		default:
			if (isalpha_(ch))
				fail(c, RX_EESCAPE);
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
			fail(c, RX_EBRACK);
		n = p - name;
		c->s = p + 2;
		if (kind == ':')
		{
			if ((k = classid(name, n)) < 0)
				fail(c, RX_ECTYPE);
			if (endpoint)
				fail(c, RX_ERANGE);
			c->sets[c->nsets - 1].cls |= 1u << k;
			return -1;
		}
		if (n != 1)
			fail(c, RX_ECOLLATE);
		if (kind == '=')
		{
			if (endpoint)
				fail(c, RX_ERANGE);
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
		fail(c, RX_ERANGE);
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
			fail(c, RX_EBRACK);
		if (ch == ']' && !first)
		{
			c->s++;
			return mk(c, N_SET, si, -1);
		}
		if (ch == '-' && !first && c->s + 1 < c->end && c->s[1] != ']')
			fail(c, RX_ERANGE);
		first = false;
		if ((lo = belem(c, false)) < 0)
			continue;
		hi = lo;
		if (cur(c) == '-' && c->s + 1 < c->end && c->s[1] != ']')
		{
			c->s++;
			if ((hi = belem(c, true)) < lo)
				fail(c, RX_ERANGE);
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
			fail(c, RX_ESUBREG);
		c->backref = true;
		return mk(c, N_BACK, c->num, -1);
	}
	if (strchr("dDsSwW", -v))
		return classnode(c, -v);
	*quant = false;
	return mk(c, N_ASSERT, strchr("mMyYAZ", -v) - "mMyYAZ", -1);
}


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
			fail(c, RX_BADRPT);
		switch (ch = *c->s++)
		{
			case ':': break;
			case '=': lk = 0; break;
			case '!': lk = 1; break;
			case '<':
				if (c->s >= c->end || (*c->s != '=' && *c->s != '!'))
					fail(c, RX_BADRPT);
				lk = *c->s++ == '=' ? 2 : 3;
				break;
			default: fail(c, RX_BADRPT);
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
		fail(c, RX_EPAREN);
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
				fail(c, RX_BADRPT);
			return mk(c, N_CHR, ch, -1);
	}
}

static int
number(Comp *c)
{
	int			n = 0;

	for (; isdig(cur(c)); c->s++)
		if ((n = n * 10 + (*c->s - '0')) > DUPMAX)
			fail(c, RX_BADBR);
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
				fail(c, RX_BADBR);
		}
		if (cur(c) != '}')
			fail(c, RX_BADBR);
		c->s++;
	}
	if ((*lazy = look(c) == '?'))
		c->s++;
	if (quantstart(c))
		fail(c, RX_BADRPT);
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
		fail(c, RX_BADRPT);
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


int
alt(Comp *c)
{
	int			first,
				last;

	if (++c->depth > MAXDEPTH)
		fail(c, RX_ETOOBIG);
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

void
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
			default: fail(c, RX_BADOPT);
		}
	if (cur(c) != ')')
		fail(c, RX_BADOPT);
	c->s++;
}
