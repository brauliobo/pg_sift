/*
 * col/api/leaf.h - 1. Leaf columns and values: what an index is made of; the shredder fills rows with them
 * A piece of col/col.h: include col/col.h, not this file.
 */
/* ---------------------------------------------------------------------------
 * 1. Leaf columns and values
 * ------------------------------------------------------------------------- */

typedef enum SiftColKind
{
	SIFT_COLK_SCALAR = 1,
	SIFT_COLK_GROUP				/* array of objects: its value in a row is the element count; its children are SCALAR leaves */
} SiftColKind;

/* Storage class. Every chunk holds integers in the key domain: INT as is, F4/F8 as the order-preserving key of the IEEE bits, STR a dictionary code */
typedef enum SiftColStore
{
	SIFT_COLS_INT = 1,			/* int2, int4, int8, bool, date, timestamp, timestamptz (and a GROUP's counts) */
	SIFT_COLS_F4,
	SIFT_COLS_F8,
	SIFT_COLS_STR,				/* text, varchar, char, name; per-segment dictionary */
	/*
	 * Opaque bytes of any length (up to 1 GB) with an exact round trip: numeric, uuid, long text, jsonb, arrays. The engine never
	 * interprets them; the owner of the segment (the table AM) defines the image. Not filterable except IS NULL, no dictionary, zones
	 * carry only nvals/nnull. The index AM does not offer it (CREATE INDEX refuses those types).
	 */
	SIFT_COLS_VAR
} SiftColStore;

/* The table AM keeps its next virtual block in the metapage field the BM25 side uses for the total document length (always 0 for col segments) */
#define SIFT_COL_META_NEXT_VBLOCK(meta)		((meta)->sum_dl)

#define SIFT_COL_MAX_SCALE		18	/* numeric precision (and so scale) the INT class holds exactly */
#define SIFT_COL_MAX_LANES		3	/* SIFT_COL_MAX_WIDE_SCALE (col/wide.h) is the same limit for a scaled 128-bit integer of two lanes */

#define SIFT_COLD_ASTEXT	0x0002	/* a shredded text leaf that holds the `->>` text of any scalar (astext): not the string a jsonpath or `@>` compares; set in the index definition, the segments are those of a text leaf */
#define SIFT_COLD_NOIMAGE	0x0008	/* the image of an array that keeps its lengths is not stored: the head is NULL in every row (and lossy), the lanes and the elements hold the array; set in the index definition (option array_images) */
#define SIFT_COLD_SHAPE		0x0004	/* a range or network head and its lanes: told from the lengths of an array, which has as many VARLENA lanes */
#define SIFT_COLD_NET		0x0010	/* with SIFT_COLD_SHAPE: the head of a network (inet, cidr): the node of its block and the host bits inside it, a range has the start and the end */
#define SIFT_COLD_FOUND	0x0020	/* the flag of a connection (col/conn.h, leaf "<fk>#found"): the parent row of the key was found; not a column of the table, its attno is the attribute that holds the key */
#define SIFT_COLD_SCOPE	0x0040	/* the derived key of a scope (col/scope.h, leaf "<key_name>#scope"): the scope key of the row through its scope relation, an int8; NULL in every row an entity segment or the write buffer holds, filled in the scope segments; not a column of the table, its attno is the attribute that holds the member key */
#define SIFT_COLD_CODED	0x0080	/* with SIFT_COLD_SCOPE: the scope key or the member of the scope is a text, varchar or uuid column (col/scope_keys.c): column format minor 50. A key's leaf holds its code in the value store of the key column of the scope relation, not the key */
#define SIFT_COLD_DERIVED	(SIFT_COLD_FOUND | SIFT_COLD_SCOPE)	/* a leaf no column of the table holds: the writer of a segment fills it from another table */
#define SIFT_COLD_LOSSY		0x0001	/* values that may not fit the declared type, or whose bytes are not the type's equality (char without a length): predicates on it must not be pushed down */

/*
 * Logical type of a leaf, part of the stored schema and of its hash. It is a tag of the format, not of any host: a host maps its own types
 * to it at its boundary (the PostgreSQL side is col/pg.h) and keeps what it cannot name here as OTHER, resolving the exact type from
 * its own catalog. The tag never changes how a chunk is stored (that is the storage class).
 */
typedef enum SiftColType
{
	SIFT_COLT_OTHER = 0,
	SIFT_COLT_BOOL,
	SIFT_COLT_INT2,
	SIFT_COLT_INT4,
	SIFT_COLT_INT8,
	SIFT_COLT_FLOAT4,
	SIFT_COLT_FLOAT8,
	/*
	 * Exact decimal, in one of three forms told apart by the head leaf: a scaled integer of the INT class (SiftColDef.scale, precision <= 18),
	 * a scaled 128-bit integer of two INT lanes (precision <= 38), or the canonical image of the number in a STR head with its display scale
	 * as the INT lane after it.
	 */
	SIFT_COLT_NUMERIC,
	SIFT_COLT_TEXT,				/* text, varchar, char(n), name */
	SIFT_COLT_BYTEA,
	SIFT_COLT_DATE,
	SIFT_COLT_TIMESTAMP,
	SIFT_COLT_TIMESTAMPTZ,
	SIFT_COLT_UUID,
	SIFT_COLT_JSONB,
	SIFT_COLT_MONEY,			/* INT class: the int64 count of the smallest currency unit */
	SIFT_COLT_INTERVAL,			/* three INT lanes: the canonical total as 128 bits (two lanes) and the month and day counts */
	SIFT_COLT_TIME,				/* INT class: the int64 microseconds since midnight */
	SIFT_COLT_ENUM,				/* INT class: the OID of the enum value (its order is the catalog's, read when a query runs) */
	SIFT_COLT_VARLENA,			/* STR class: the image of a varlena datum that no other tag names (an array, a range, a multirange, inet, cidr, bit, varbit), the varlena itself */
	SIFT_COLT_MACADDR,			/* STR class: the 6 bytes of the datum */
	SIFT_COLT_MACADDR8,			/* STR class: the 8 bytes of the datum */
	SIFT_COLT_TIMETZ,			/* STR class: the 12 bytes of the datum (the microseconds, then the zone in seconds), never compared as bytes */
	SIFT_COLT_END
} SiftColType;

typedef struct SiftColDef
{
	int16		id;				/* position in the leaf list, 0..ncols-1 */
	int16		parent;			/* id of the GROUP leaf this is a child of, -1 for a top level leaf */
	uint8		kind;			/* SiftColKind */
	uint8		store;			/* SiftColStore */
	uint16		flags;			/* SIFT_COLD_* */
	int16		attno;			/* 1-based attribute of the index the leaf comes from */
	uint8		type;			/* SiftColType: the logical type of the values the leaf yields to the host */
	const char *path;			/* "" for a plain attribute, else the shredder's path text (e.g. items[].category) */
	uint8		scale;			/* numeric(p, s) of the INT class: digits after the decimal point; the integer value is the number times 10^scale. 0 otherwise */
	/*
	 * Wide leaves: a value of more than 64 bits is `lanes` consecutive leaves of one attribute, each an ordinary leaf
	 * with its own chunk and zone. The first (the head, lane 0) is the leaf a planner addresses: lanes is 2 or 3 and the next lanes - 1 leaves
	 * have lane = 1, 2. A wide integer is lane 0 = the high word (signed) and lane 1 = the low word with the sign bit flipped, so comparing
	 * (lane 0, lane 1) in turn is the order of the value; any lane after those two is payload that equality and order ignore. Every lane has the
	 * NULL and overflow bits of the value. 0 and 0 for a leaf of one lane.
	 */
	uint8		lanes;
	uint8		lane;
} SiftColDef;

/* a leaf the shredder fills from a jsonb value (a path is declared): its NULL slots are JSON nulls or missing keys, and an astext leaf holds numbers and booleans as text */
static inline bool
sift_col_def_has_kinds(const SiftColDef *d)
{
	return d->store != SIFT_COLS_VAR && d->path != NULL && d->path[0] != 0 && d->lane == 0 && !(d->flags & SIFT_COLD_DERIVED);	/* a lane after the head of a wide leaf has a path of its own (name#k) */
}

static inline bool
sift_col_def_is_wide(const SiftColDef *d)
{
	return d->lanes > 1;
}

/* a leaf that is the image of a datum: uuid and bytea (as images: the exceptions leaf of a sift_table is a plain minor 3 one) or another datum, whose value only its type's own functions know */
static inline bool
sift_col_def_is_image(const SiftColDef *d)
{
	return d->type == SIFT_COLT_VARLENA || d->type == SIFT_COLT_MACADDR || d->type == SIFT_COLT_MACADDR8 || d->type == SIFT_COLT_TIMETZ ||
		(d->store == SIFT_COLS_STR && (d->type == SIFT_COLT_UUID || d->type == SIFT_COLT_BYTEA));
}

/* the leaf "<column>#present" of a jsonb document (true, NULL where the document is SQL NULL): a flag of the column, no dimension or measure of the pre-aggregates */
static inline bool
sift_col_def_is_present(const SiftColDef *d)
{
	size_t		n = strlen(d->path);

	return n > 8 && strcmp(d->path + n - 8, "#present") == 0;
}

/* a leaf that only a 3.2 segment holds: a lane of a wide value, money, interval, time, enum or an image */
static inline bool
sift_col_def_needs_wide(const SiftColDef *d)
{
	return d->lanes > 0 || d->lane > 0 || d->type == SIFT_COLT_MONEY || d->type == SIFT_COLT_INTERVAL || d->type == SIFT_COLT_TIME ||
		d->type == SIFT_COLT_ENUM || sift_col_def_is_image(d);
}

/*
 * The head of a range or a network: the image of the datum (a STR leaf) followed by the INT lanes its containment operators test (col/shape.c):
 * a range has the start and the end of the set it holds, a network the node of its block (first address, mask and family) and the offset of an IPv4 address inside it (3 leaves each). Reads of the value use the head alone.
 */
#define SIFT_COL_SHAPE_RANGE	1
#define SIFT_COL_SHAPE_INET		2

static inline int
sift_col_def_shape(const SiftColDef *d)
{
	if (d->lane != 0 || d->type != SIFT_COLT_VARLENA || !(d->flags & SIFT_COLD_SHAPE))
		return 0;
	return d->flags & SIFT_COLD_NET ? SIFT_COL_SHAPE_INET : d->lanes == 3 ? SIFT_COL_SHAPE_RANGE : 0;
}

/* the first lane of the head def[i - 1] of a range or a network: the start of the set, an integer key the units can be cut by */
static inline bool
sift_col_def_shape_start(const SiftColDef *defs, int i)
{
	return defs[i].lane == 1 && sift_col_def_shape(&defs[i - 1]) != 0;
}

/* a leaf whose value the column engine tests on its stored lanes or image (numeric, money, interval: col/widescan.c; an image: its equality and IN, for the types the query side names) */
static inline bool
sift_col_def_lane_tested(const SiftColDef *d)
{
	return d->lane == 0 && (d->lanes > 0 || d->type == SIFT_COLT_MONEY || (sift_col_def_is_image(d) && d->type != SIFT_COLT_TIMETZ));
}

/*
 * The head of an array that keeps its lengths: the image, then the INT lanes cardinality(x) (lane 1) and
 * array_length(x, 1) (lane 2), each NULL where PostgreSQL's function is. A range or inet has the lanes of sift_col_def_shape, flagged SIFT_COLD_SHAPE.
 */
#define SIFT_COL_ARRLEN_LANES	3
#define SIFT_COL_ARR_LANES		1		/* sift_col_att_leaves: an array keeps its lengths ... */
#define SIFT_COL_ARR_NOIMAGE	2		/* ... and not its image (SIFT_COLD_NOIMAGE) */

static inline bool
sift_col_def_arrlen_head(const SiftColDef *d)
{
	return d->type == SIFT_COLT_VARLENA && d->lanes == SIFT_COL_ARRLEN_LANES && !(d->flags & SIFT_COLD_SHAPE);
}

static inline bool
sift_col_def_arrlen_lane(const SiftColDef *d)
{
	return d->type == SIFT_COLT_VARLENA && d->lane > 0 && !(d->flags & SIFT_COLD_SHAPE);
}

/* how many of a head's leaves hold its value: the lanes of an array's lengths are read apart */
static inline int
sift_col_def_value_lanes(const SiftColDef *d)
{
	return sift_col_def_arrlen_head(d) || sift_col_def_shape(d) || d->lanes < 2 ? 1 : d->lanes;
}

/* The JSON type of an overflowing value; the numbers are on disk */
#define SIFT_COLTAG_UNKNOWN		0
#define SIFT_COLTAG_STRING		1
#define SIFT_COLTAG_NUMBER		2
#define SIFT_COLTAG_BOOLEAN		3
#define SIFT_COLTAG_OBJECT		4
#define SIFT_COLTAG_ARRAY		5
#define SIFT_COLTAG_NONE		6		/* the leaf's path leads to no value: an array sits where the path declares an object (jsonb_typeof of the path is NULL) */
#define SIFT_COLTAG_JNULL		7		/* JSON null: a NULL slot that is not a missing key (never an overflow value); its jsonb_typeof is 'null' */
#define SIFT_COLTAG_OVF_MAX		SIFT_COLTAG_NONE	/* the last tag an overflow value has */
#define SIFT_COLTAG_MAX			7
#define SIFT_COLTAG_UNSURE		8		/* memory only: a legacy write-buffer slot did not preserve its kind */

/* A value of a leaf in a row, or a constant of a predicate. INT: v.i. F4/F8: v.f (float4 promoted exactly). STR: v.s with len bytes (not terminated). */
typedef struct SiftColValue
{
	union
	{
		int64		i;
		double		f;
		const char *s;
	}			v;
	uint32		len;
	int64		lo;				/* a constant of a wide integer head (lanes == 2, INT): v.i is the key of lane 0, lo the key of lane 1 (sift_col_wide_hi / _lo) */
	bool		isnull;
	bool		overflow;		/* the value exists but the leaf cannot hold it (the shredder's OVERFLOW): stored as NULL and flagged; isnull is also set */
	uint8		tag;			/* overflow: the JSON type of the value, SIFT_COLTAG_*, 0 when the writer does not know it. Otherwise 0, or what the leaf's
								 * own type does not say: SIFT_COLTAG_JNULL for a JSON null, the type of an astext leaf's number or boolean */
} SiftColValue;

/* The tags a slot can have: an overflow value a type (0: not known), a NULL slot a JSON null, a value a scalar kind */
static inline bool
sift_col_tag_valid(const SiftColValue *v)
{
	return v->tag == SIFT_COLTAG_UNSURE || v->tag == SIFT_COLTAG_UNKNOWN ||
		(v->overflow ? v->tag <= SIFT_COLTAG_OVF_MAX : v->isnull ? v->tag == SIFT_COLTAG_JNULL : v->tag <= SIFT_COLTAG_BOOLEAN);
}

/*
 * One row to index, as ambuild, aminsert and the shredder produce it. val[id] is the value of a top level scalar leaf; for a GROUP leaf
 * val[id].v.i is the element count (isnull: the array is absent). elem[id] of a child leaf points to val[parent].v.i values, NULL for
 * the other leaves. All memory belongs to the caller (a per-row context).
 */
typedef struct SiftColRow
{
	ItemPointerData tid;
	int			ncols;
	SiftColValue *val;
	SiftColValue **elem;
} SiftColRow;

/*
 * Order-preserving key of a float and back. The sign of zero is kept: -0 is the key just below +0 (-1 and 0), so every non-NaN value
 * comes back with the bits it went in with; PostgreSQL compares -0 and +0 equal, which the tests built by sift_ctest_make account
 * for (a constant of zero spans both keys). Every NaN is one NaN above +Inf.
 */
static inline int64
sift_col_f8_key(double f)
{
	uint64		b;

	if (f != f)
		b = UINT64CONST(0x7FF8000000000000);
	else
		memcpy(&b, &f, sizeof(b));
	return (int64) (b ^ ((uint64) ((int64) b >> 63) >> 1)) ;
}

/*
 * Order-preserving key of the first 8 bytes of a string: big endian, zero padded, sign bit flipped. s < t (bytewise) implies
 * prefix(s) <= prefix(t); strings of equal keys differ only after byte 8 or by trailing zero bytes. Zones and filters of inline strings use it.
 */
static inline int64
sift_col_str_prefix(const char *s, uint32 len)
{
	uint64		b = 0;

	for (uint32 i = 0; i < 8; i++)
		b = b << 8 | (i < len ? (uint8) s[i] : 0);
	return (int64) (b ^ UINT64CONST(0x8000000000000000));
}

static inline double
sift_col_f8_from_key(int64 key)
{
	uint64		b = (uint64) key ^ ((uint64) (key >> 63) >> 1);
	double		f;

	memcpy(&f, &b, sizeof(f));
	return f;
}
