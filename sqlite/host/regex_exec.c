/*
 * regex_exec.c - the matcher of the regex engine (regex.c): the character classes and the backtracking run behind
 * pg_regexec.
 */
#include "regex_int.h"

typedef struct RxMatch
{
	long		rm_so,
				rm_eo;
} RxMatch;

#define MAXSTACK	(1 << 20)			/* backtrack entries */
#define MAXMEMO		((size_t) 1 << 28)	/* bits of the visited set */
#define BUDGET		5000000				/* steps of one pg_regexec */
#define R_FAIL		(-1)
#define R_BUDGET	(-2)

/* ------------------------------------------------ characters */

bool
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
pg_regexec(regex_t *re, const pg_wchar *string, size_t len, size_t search_start, void *details,
		   size_t nmatch, void *matches, int flags)
{
	Rx		   *x = re->posix;
	RxMatch    *pmatch = matches;
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
			return RX_ESPACE;
		if ((bits + 7) / 8 > x->seencap)
		{
			x->seencap = (bits + 7) / 8;
			x->seen = x->seen ? repalloc(x->seen, x->seencap) : palloc(x->seencap);
		}
		memset(x->seen, 0, (bits + 7) / 8);
	}
	for (st = search_start; st <= len && end < 0; st++)
		if ((end = run(x, string, len, 0, st, -1)) == R_BUDGET)
			return RX_ESPACE;
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
