/*
 * agg/vexpr_str.h - the text kernels the expression compiler of SiftAggV6 uses beside the register machine of agg/vexpr.h: lower, btrim and split_part of a text column (a
 * pipeline of steps over the strings of a batch), comparisons and IN lists of text with the three-valued logic of SQL and a pool of the strings a batch makes.
 *
 * A string result is a SiftStrView: a slice of its input when it can be (btrim, split_part), a string of the arena of the batch when it cannot (lower); the arena is reset for every batch of
 * SIFT_VEC_LEN rows. The register machine keeps 8-byte values in its registers and a SiftStrView is 16 bytes, so these results leave as vectors of their own and the booleans as the
 * (true, known) bitmaps of a SiftVexprMaskIn, which the machine composes with AND, OR, NOT and CASE as it does a boolean column.
 *
 * Portable: only core/host.h and agg/vec.h.
 */
#ifndef SIFT_AGG_VEXPR_STR_H
#define SIFT_AGG_VEXPR_STR_H

#include "core/host.h"

#include "agg/vec.h"

#define SIFT_VSTR_ARENA_BLOCK	(64 * 1024)
#define SIFT_VSTR_MAX_STEPS		4

/* ---- strings ---- */

static inline const char *
sift_vstr_bytes(const SiftStrView *v)
{
	return v->len <= 12 ? (const char *) v + 4 : v->u.ptr;
}

static inline SiftStrView
sift_vstr_make(const char *p, uint32 len)
{
	SiftStrView v;

	memset(&v, 0, sizeof(v));
	v.len = len;
	if (len <= 12)
		memcpy((char *) &v + 4, p, len);
	else
	{
		memcpy(v.prefix, p, 4);
		v.u.ptr = p;
	}
	return v;
}

typedef struct SiftVstrBlock
{
	struct SiftVstrBlock *next;
	uint32		cap;
	uint32		used;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} SiftVstrBlock;

/* The strings a batch makes: blocks of SIFT_VSTR_ARENA_BLOCK bytes (a longer string has a block of its own), all but the first freed by a reset */
typedef struct SiftVstrArena
{
	MemoryContext cx;
	SiftVstrBlock *head;
} SiftVstrArena;

extern void sift_vstr_arena_init(SiftVstrArena *ar, MemoryContext cx);
extern char *sift_vstr_arena_alloc(SiftVstrArena *ar, uint32 len);
extern void sift_vstr_arena_reset(SiftVstrArena *ar);
extern void sift_vstr_arena_free(SiftVstrArena *ar);

extern bool sift_vstr_is_ascii(const char *p, uint32 len);
/* dst (room for len bytes) = the string with 'A'..'Z' folded; false (dst unspecified) when a byte is not ASCII */
extern bool sift_vstr_lower_ascii(char *dst, const char *src, uint32 len);
/* split_part(s, d, field) of PostgreSQL: the fields are cut at the occurrences of d, left to right without overlap; a field below zero counts from the last; -1 for field 0, which PostgreSQL rejects */
extern int	sift_vstr_split_part(const char *s, uint32 len, const char *d, uint32 dlen, int32 field, const char **out, uint32 *olen);
/* the set of bytes btrim(s, chars) cuts: false when chars holds a byte that is not ASCII (a character of more than one byte) */
extern bool sift_vstr_trim_set(bool *set, const char *chars, uint32 n);
extern void sift_vstr_btrim(const char *s, uint32 len, const bool *set, const char **out, uint32 *olen);
extern int	sift_vstr_cmp_bytes(const char *a, uint32 alen, const char *b, uint32 blen);

typedef enum SiftVstrOp
{
	SVS_LOWER,
	SVS_BTRIM,
	SVS_SPLIT_PART
} SiftVstrOp;

typedef struct SiftVstrStep
{
	SiftVstrOp	op;
	const char *arg;			/* split_part: the delimiter */
	uint32		arglen;
	int32		field;
	bool		trim[256];		/* btrim: the bytes it cuts */
} SiftVstrStep;

/* A lower of a string with a byte that is not ASCII under a collation that is not C: the owner of the collation makes it (PostgreSQL's str_tolower); the result is in the arena */
typedef char *(*SiftVstrLowerFn) (void *arg, SiftVstrArena *ar, const char *p, uint32 len, uint32 *olen);

typedef struct SiftVstrPipe
{
	int			nsteps;
	SiftVstrStep step[SIFT_VSTR_MAX_STEPS];
	bool		ascii_fold;		/* the collation lowers the ASCII letters as C does (true of C and of most locales; not of tr_TR): a string of ASCII needs no call of the collation */
	bool		c_collation;	/* the collation is C: lower folds the ASCII letters of any string and leaves every other byte (implies ascii_fold) */
	SiftVstrLowerFn lower_fn;
	void	   *lower_arg;
} SiftVstrPipe;

extern void sift_vstr_step_lower(SiftVstrPipe *p);
extern bool sift_vstr_step_btrim(SiftVstrPipe *p, const char *chars, uint32 n);
extern bool sift_vstr_step_split_part(SiftVstrPipe *p, const char *delim, uint32 dlen, int32 field);

/* out[r] and valid_out = the pipeline over the rows of active (the others are left alone); a NULL input is NULL. true unless a row needed a lower this pipeline cannot make */
extern bool sift_vstr_pipe_run(const SiftVstrPipe *p, SiftVstrArena *ar, const SiftVec *in, const uint64 *active, uint16 n, SiftStrView *out, uint64 *valid_out);

typedef enum SiftVstrCmp
{
	SVC_EQ,
	SVC_NE,
	SVC_LT,
	SVC_LE,
	SVC_GT,
	SVC_GE
} SiftVstrCmp;

/* the order of two strings under the collation of the column (NULL: bytes: the C collation, and any deterministic one for = and <>) */
typedef int (*SiftVstrCmpFn) (void *arg, const char *a, uint32 alen, const char *b, uint32 blen);

/* true_out / known_out (n bits): v[r] op c over the rows of active; a NULL row is unknown. v is the strings (valid NULL: every row) */
extern void sift_vstr_cmp_const(const SiftStrView *v, const uint64 *valid, const uint64 *active, uint16 n, SiftVstrCmp op, const char *c, uint32 clen,
								SiftVstrCmpFn fn, void *fn_arg, uint64 *true_out, uint64 *known_out);

/* The strings of a constant or parameter array of text, sorted and without repeats; has_null: an element is NULL */
typedef struct SiftVstrSet
{
	uint32		n;
	bool		has_null;
	SiftStrView *v;
	char	   *bytes;
} SiftVstrSet;

extern SiftVstrSet *sift_vstr_set_build(const char *const *p, const uint32 *len, const bool *isnull, uint32 n);
extern bool sift_vstr_set_has(const SiftVstrSet *s, const char *p, uint32 len);
extern void sift_vstr_set_free(SiftVstrSet *s);
/* bit i of bits (ceil(n / 64) words) = dict[i] is in the set: the strings of a dictionary of a part, once for the part */
extern void sift_vstr_set_dict_bits(const SiftVstrSet *s, const SiftStrView *dict, uint32 n, uint64 *bits);
/* x = ANY(set) with the three-valued logic of SQL: true when x is in it, unknown when x is NULL or it is not and the set holds a NULL, else false */
extern void sift_vstr_in_set(const SiftStrView *v, const uint64 *valid, const uint64 *active, uint16 n, const SiftVstrSet *s, uint64 *true_out, uint64 *known_out);

#endif							/* SIFT_AGG_VEXPR_STR_H */
