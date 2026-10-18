/*
 * sift_shred.h - shreds a JSON document into typed columns for declared paths (docs/COL-SHRED.md).
 *
 * Portable (src/portable-col.list): the document arrives as events (sift_shred_key / enter / leave / scalars), so any JSON
 * source can drive it; src/sift_shred_pg.c drives it from a JsonbContainer. The output is a SiftShredDoc of typed arrays
 * with Dremel / DuckDB LIST semantics: per column one slot per element of its group, per group the element ranges of
 * every parent element.
 */
#ifndef SIFT_SHRED_H
#define SIFT_SHRED_H

#include "sift.h"
#include "sift_col_enc.h"
#include "sift_cpu.h"

#define SIFT_SHRED_MAX_SPEC_BYTES	(64 * 1024)
#define SIFT_SHRED_MAX_COLS			256
#define SIFT_SHRED_MAX_GROUPS		64
#define SIFT_SHRED_MAX_STEPS		16
#define SIFT_SHRED_DEFAULT_TEXT		SIFT_MAX_TERM_BYTES	/* what a text dictionary of sift_col holds */
#define SIFT_SHRED_MAX_TEXT			(1024 * 1024)
#define SIFT_SHRED_DEFAULT_ELEMS	(1 << 20)
#define SIFT_SHRED_DEFAULT_BYTES	(16 << 20)
#define SIFT_SHRED_DEFAULT_KEYS		65536
#define SIFT_SHRED_MAX_KEY_BYTES	255
#define SIFT_SHRED_NO_KEY			PG_UINT32_MAX
/* the arrays of a document that grew past this many elements are released by the next sift_shred_begin */
#define SIFT_SHRED_RETAIN			(64 * 1024)

typedef enum SiftShredType
{
	SIFT_SHRED_BOOL,
	SIFT_SHRED_INT2,
	SIFT_SHRED_INT4,
	SIFT_SHRED_INT8,
	SIFT_SHRED_FLOAT4,
	SIFT_SHRED_FLOAT8,
	SIFT_SHRED_TEXT
} SiftShredType;

/* State of a slot or of a list; the values index SiftShredCounts */
typedef enum SiftShredState
{
	SIFT_SHRED_ABSENT = 0,		/* the path is missing (or a scalar / null is on the way) */
	SIFT_SHRED_NULL = 1,		/* JSON null */
	SIFT_SHRED_VALUE = 2,		/* a typed value (lists: an array / object, possibly empty) */
	SIFT_SHRED_OVERFLOW = 3		/* exists but not representable: the heap has to be asked */
} SiftShredState;

/* Why a value overflowed (OR-ed in SiftShredCol.ovf) */
#define SIFT_SHRED_OVF_SHAPE	0x01	/* a container where a scalar, or the wrong kind of container, is declared */
#define SIFT_SHRED_OVF_TYPE		0x02	/* another JSON scalar than the declared type takes */
#define SIFT_SHRED_OVF_RANGE	0x04	/* number out of range of the type, or not integral for an integer type */
#define SIFT_SHRED_OVF_LENGTH	0x08	/* text longer than the column allows */
#define SIFT_SHRED_OVF_TRUNC	0x10	/* list cut by max_elems / max_bytes */
#define SIFT_SHRED_OVF_KEYS		0x20	/* wildcard key beyond the dictionary limits */

typedef enum SiftShredGroupKind
{
	SIFT_SHRED_ROOT,
	SIFT_SHRED_LIST,			/* key[] */
	SIFT_SHRED_MAP				/* key.* */
} SiftShredGroupKind;

typedef struct SiftShredColDef
{
	const char *name;			/* canonical flat path: items[].category */
	SiftShredType type;
	int			group;
	uint32		maxlen;			/* text: bytes allowed */
	bool		explicit_len;	/* text(N) was written */
} SiftShredColDef;

typedef struct SiftShredGroupDef
{
	const char *name;			/* "" for the root, else the path of the list: items[], metrics.* */
	SiftShredGroupKind kind;
	int			parent;			/* -1 for the root */
	int			ncols;
	int		   *cols;			/* columns of the group, declaration order */
	int			nchildren;
	int		   *children;		/* groups whose parent this is */
} SiftShredGroupDef;

typedef struct SiftShredNode SiftShredNode;

typedef struct SiftShredSpec
{
	int			ncols;
	int			ngroups;
	SiftShredColDef *cols;
	SiftShredGroupDef *groups;
	char	   *canonical;		/* sift_shred_spec_deparse */
	uint64		fingerprint;	/* sift_term_hash64 of canonical */
	SiftShredNode *root;
} SiftShredSpec;

/* Parses and validates; raises ERRCODE_SYNTAX_ERROR (with the 1-based position), INVALID_PARAMETER_VALUE or PROGRAM_LIMIT_EXCEEDED. */
extern SiftShredSpec *sift_shred_spec_parse(const char *text);
extern const char *sift_shred_spec_deparse(const SiftShredSpec *spec);
extern const char *sift_shred_type_name(SiftShredType t);

/* ---------- the output ---------- */

typedef union SiftShredVal
{
	int64		i;				/* bool, int2, int4, int8 */
	double		f;				/* float4 (exact, widened), float8 */
	struct
	{
		uint32		off;		/* text: bytes at SiftShredDoc.text + off */
		uint32		len;
	}			s;
} SiftShredVal;

typedef struct SiftShredCol
{
	uint8	   *state;			/* [group n] SiftShredState */
	SiftShredVal *val;			/* [group n] */
	uint32		ovf;			/* SIFT_SHRED_OVF_* seen in this document, also by lists below that overflowed */
} SiftShredCol;

typedef struct SiftShredGroup
{
	uint32		n;				/* elements in this document (root: 1) */
	uint32		pn;				/* elements of the parent group: the length of end and lstate */
	uint32	   *end;			/* [pn] exclusive end of the elements of each parent element */
	uint8	   *lstate;			/* [pn] SiftShredState of the list under each parent element */
	uint32	   *keyid;			/* [n] maps: dictionary id of the member name, or SIFT_SHRED_NO_KEY */
	uint32		cap;			/* slots allocated for n (columns, keyid) */
	uint32		pcap;			/* slots allocated for pn (end, lstate) */
} SiftShredGroup;

typedef struct SiftShredDoc
{
	SiftShredCol *col;			/* [spec->ncols] */
	SiftShredGroup *grp;		/* [spec->ngroups] */
	char	   *text;			/* the bytes of the text values */
	uint32		ntext;
	uint32		textcap;
	uint32		nelems;			/* list and map elements so far, against max_elems */
} SiftShredDoc;

typedef struct SiftShredLimits
{
	uint32		max_elems;		/* list / map elements per document */
	uint32		max_bytes;		/* text bytes per document */
	uint32		max_keys;		/* distinct wildcard keys of the dictionary */
} SiftShredLimits;

/* ---------- the shredder ---------- */

typedef enum SiftShredKey
{
	SIFT_SHRED_SKIP,			/* not declared: ignore the value */
	SIFT_SHRED_TAKE,			/* deliver the value */
	SIFT_SHRED_STOP				/* deliver the value, then the rest of the object is irrelevant */
} SiftShredKey;

typedef struct SiftShredFrame
{
	const SiftShredNode *node;
	const SiftShredNode *pending;
	uint32		keyid;
	uint32		matched;
	bool		truncated;
} SiftShredFrame;

typedef struct SiftShred
{
	const SiftShredSpec *spec;
	SiftShredDoc doc;			/* the output of the document being shredded / last shredded */
	SiftCDict  *dict;			/* wildcard keys: the id of a map member is its code */
	SiftShredLimits limits;
	MemoryContext mcxt;
	SiftShredFrame *frames;
	int			nframes;
	int			maxframes;
	bool		root_done;
} SiftShred;

extern SiftShred *sift_shred_create(const SiftShredSpec *spec, const SiftShredLimits *limits, bool cached);
extern void sift_shred_free(SiftShred *sh);

extern void sift_shred_begin(SiftShred *sh);
extern SiftShredKey sift_shred_key(SiftShred *sh, const char *key, int len);
extern bool sift_shred_enter(SiftShred *sh, bool is_array);
extern void sift_shred_leave(SiftShred *sh);
extern bool sift_shred_full(const SiftShred *sh);
/* The keys the object being shredded declares, in jsonb order (none for a map): a driver whose objects are indexed looks them up
 * instead of walking every member, and then calls sift_shred_key for each one it found */
extern int	sift_shred_object_keys(const SiftShred *sh);
extern const char *sift_shred_object_key(const SiftShred *sh, int i, int *len);
extern bool sift_shred_wants_number(const SiftShred *sh);
extern void sift_shred_null(SiftShred *sh);
extern void sift_shred_bool(SiftShred *sh, bool b);
extern void sift_shred_string(SiftShred *sh, const char *s, int len);
extern void sift_shred_number(SiftShred *sh, const char *text, int len);
extern void sift_shred_finish(SiftShred *sh);

/* parent[i] for the n elements of group g (n = doc->grp[g].n): the ordinal of the parent element each belongs to (sift_colk->expand_parents) */
extern void sift_shred_expand_parents(const SiftShredDoc *doc, int g, uint32 *parent);

/* Elements [lo, hi) of group g that belong to element p of its parent group */
static inline void
sift_shred_group_range(const SiftShredDoc *doc, int g, uint32 p, uint32 *lo, uint32 *hi)
{
	*lo = p == 0 ? 0 : doc->grp[g].end[p - 1];
	*hi = doc->grp[g].end[p];
}

/* ---------- SIMD kernels ---------- */

/*
 * Kept apart from the column and aggregation kernels: these work on the shredder's state bytes with no bit vector and no padding of
 * the value arrays, and the generic composition (sift_colk->range on the bytes, SiftAggKernels.minmax_* under its bitmap) is more than
 * 3 percent slower at the scalar level for every kernel and at AVX2 for state_bits, state_counts and minmax_f64
 */
typedef struct SiftShredKernels
{
	/* counts[s] = number of bytes equal to s, s < 4; every byte of state is < 4 */
	void		(*state_counts) (const uint8 *state, uint32 n, uint32 counts[4]);

	/*
	 * Bitmaps of a column for the engine: bit i of nulls is set unless state[i] is VALUE (absent, null and overflow are NULL of
	 * the typed array), bit i of ovf if state[i] is OVERFLOW; ceil(n / 64) words each, the bits past n clear. The typed values of
	 * a slot that is not VALUE are zero and carry no meaning: NULL is never a sentinel value.
	 */
	void		(*state_bits) (const uint8 *state, uint32 n, uint64 *nulls, uint64 *ovf);

	/*
	 * Minimum and maximum of val[i] over the i < n with state[i] == SIFT_SHRED_VALUE; false (outputs untouched) if there is
	 * none. The doubles are never NaN and never -0.0 (the shredder guarantees it).
	 */
	bool		(*minmax_i64) (const uint8 *state, const int64 *val, uint32 n, int64 *min, int64 *max);
	bool		(*minmax_f64) (const uint8 *state, const double *val, uint32 n, double *min, double *max);

} SiftShredKernels;

extern PGDLLIMPORT const SiftShredKernels *sift_shred_kern;
extern const SiftShredKernels *sift_shred_kernels_for(SiftSimdLevel level);
extern void sift_shred_init(void);		/* registers the dispatch rebind; called from _PG_init */

extern const SiftShredKernels sift_shred_kernels_scalar;
extern const SiftShredKernels sift_shred_kernels_avx2;
extern const SiftShredKernels sift_shred_kernels_avx512;

#endif							/* SIFT_SHRED_H */
