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

/* The strings of a constant or parameter array of text, sorted and without repeats; has_null: an element is NULL. From SIFT_VSTR_HASH_MIN strings on they are also in a hash table, which a lookup probes (a string whose length no element has is not even hashed): tab holds the number of a string + 1, 0 for a free slot */
#define SIFT_VSTR_HASH_MIN	8

typedef struct SiftVstrSet
{
	uint32		n;
	bool		has_null;
	SiftStrView *v;
	char	   *bytes;
	uint32		mask;			/* slots of tab - 1 */
	uint32	   *tab;
	uint64		len_mask;		/* bit (length mod 64) is set for the length of an element */
} SiftVstrSet;


#endif							/* SIFT_AGG_VEXPR_STR_H */
