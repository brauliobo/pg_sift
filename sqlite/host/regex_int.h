/*
 * regex_int.h - the types and helpers that regex.c, regex_parse.c and regex_exec.c share (see regex.c).
 */
#ifndef SIFT_REGEX_INT_H
#define SIFT_REGEX_INT_H

#include "host.h"

#include <setjmp.h>

/* the error codes of the matcher, the index of errtext (the numbers of PostgreSQL's regex.h); host.h has the results of the API */
enum
{
	RX_OKAY, RX_NOMATCH, RX_BADPAT, RX_ECOLLATE, RX_ECTYPE, RX_EESCAPE, RX_ESUBREG, RX_EBRACK, RX_EPAREN, RX_EBRACE, RX_BADBR,
	RX_ERANGE, RX_ESPACE, RX_BADRPT, RX_EMPTY, RX_ASSERT, RX_INVARG, RX_MIXED, RX_BADOPT, RX_ETOOBIG, RX_ECOLORS
};

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

static inline void
fail(Comp *c, int err)
{
	c->err = err;
	longjmp(c->jb, 1);
}

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

/* regex_exec.c */
extern bool ccis(int k, pg_wchar c);

/* regex_parse.c */
extern int alt(Comp *c);
extern void options(Comp *c);

#endif							/* SIFT_REGEX_INT_H */
