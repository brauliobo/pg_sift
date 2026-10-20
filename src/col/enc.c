/*
 * col/enc.c - L1 of the column engine: schema, dictionaries, batches, column chunks, zones,
 * chunk tests and unit records. Values and bytes only; the segment writer and reader (col/seg.c) and the sources of rows sit above.
 *
 *   1. schema         2. dictionaries      3. batches
 *   4. zones          5. codec seam        6. chunks
 *   7. chunk tests    8. units
 */
#include "sift_host.h"

#include "sift_cmp.h"
#include "col/enc.h"
#include "sift_popcount.h"

#define COLS_MAGIC		0x534C4F43	/* bytes 'C' 'O' 'L' 'S' */
#define COLU_MAGIC		0x554C4F43	/* "COLU" */

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static inline uint32
words_of(uint32 n)
{
	return (n + 63) / 64;
}

/* Appends n zero bytes */
static void
put_zero(StringInfo out, Size n)
{
	enlargeStringInfo(out, (int) n);
	memset(out->data + out->len, 0, n);
	out->len += (int) n;
	out->data[out->len] = '\0';
}

static void
put_pad(StringInfo out, Size align)
{
	put_zero(out, (align - (Size) out->len % align) % align);
}

static inline void
put_bytes(StringInfo out, const void *p, Size n)
{
	appendBinaryStringInfo(out, (const char *) p, (int) n);
}

#define put_val(out, v)	put_bytes(out, &(v), sizeof(v))

/* ---------------------------------------------------------------------------
 * 1. Schema
 * ------------------------------------------------------------------------- */

typedef struct SiftColDefDisk
{
	int16		id;
	int16		parent;
	uint8		kind;
	uint8		store;
	uint16		flags;
	int16		attno;
	uint16		pathlen;
	uint8		type;
	uint8		scale;
	uint8		lanes;			/* 0 in every schema before 3.2 (these were the two reserved bytes) */
	uint8		lane;
} SiftColDefDisk;

/* NULL when the leaf list is valid, else why not */
static const char *
schema_check(const SiftColDef *defs, int n)
{
	if (n < 1 || n > SIFT_COL_MAX_COLS)
		return "the number of leaves is out of range";
	for (int i = 0; i < n; i++)
	{
		const SiftColDef *d = &defs[i];

		if (d->id != i)
			return "leaf ids must be 0..n-1 in order";
		if (d->kind != SIFT_COLK_SCALAR && d->kind != SIFT_COLK_GROUP)
			return "unknown leaf kind";
		if (d->store < SIFT_COLS_INT || d->store > SIFT_COLS_VAR)
			return "unknown storage class";
		if (d->type >= SIFT_COLT_END)
			return "unknown leaf type";
		if (d->store == SIFT_COLS_VAR && (d->kind != SIFT_COLK_SCALAR || d->parent != -1))
			return "a variable-length leaf is a top level scalar";
		if (d->path != NULL && strlen(d->path) > PG_UINT16_MAX)
			return "leaf path too long";
		if (d->scale > (d->lanes > 1 || d->lane > 0 ? SIFT_COL_MAX_WIDE_SCALE : SIFT_COL_MAX_SCALE) || (d->scale > 0 && d->store != SIFT_COLS_INT))
			return "a scale belongs to an integer leaf and is at most 18 (38 for a wide one)";
		if (d->lanes > SIFT_COL_MAX_LANES || d->lanes == 1 || (d->lanes > 1 && (d->lane != 0 || i + d->lanes > n)))
			return "a wide leaf has 2 or 3 lanes, all inside the leaf list";
		if (d->lanes > 1 || d->lane > 0)
		{
			if (d->kind != SIFT_COLK_SCALAR || d->parent != -1 || d->store == SIFT_COLS_VAR || (d->lane > 0 && d->store != SIFT_COLS_INT))
				return "a wide leaf is a top level scalar of storage class INT or STR, its other lanes INT";
			for (int k = 1; k < d->lanes; k++)
				if (defs[i + k].lane != k)
					return "the leaves after a wide head are its lanes";
			if (d->lane > 0 && (d->lane >= SIFT_COL_MAX_LANES || i < d->lane || defs[i - d->lane].lanes <= d->lane ||
								defs[i - d->lane].attno != d->attno || defs[i - d->lane].type != d->type || defs[i - d->lane].scale != d->scale))
				return "a lane must follow its head and agree with it";
		}
		if (d->kind == SIFT_COLK_GROUP && (d->parent != -1 || d->store != SIFT_COLS_INT))
			return "a group is a top level leaf of storage class INT";
		if (d->kind == SIFT_COLK_SCALAR && d->parent != -1 &&
			(d->parent < 0 || d->parent >= i || defs[d->parent].kind != SIFT_COLK_GROUP))
			return "the parent of a child must be an earlier group";
	}
	return NULL;
}

static void
schema_put_def(StringInfo out, const SiftColDef *d, bool with_flags)
{
	const char *path = d->path ? d->path : "";
	SiftColDefDisk disk = {d->id, d->parent, d->kind, d->store, with_flags ? d->flags : 0, d->attno, (uint16) strlen(path), d->type, d->scale, d->lanes, d->lane};

	put_val(out, disk);
	put_bytes(out, path, disk.pathlen);
}

static uint32
schema_hash(const SiftColDef *defs, int n)
{
	StringInfoData buf;
	uint32		h;

	initStringInfo(&buf);
	for (int i = 0; i < n; i++)
		schema_put_def(&buf, &defs[i], false);
	h = hash_bytes((const unsigned char *) buf.data, buf.len);
	pfree(buf.data);
	return h;
}

static SiftColSchema *
schema_build(MemoryContext mcxt, const SiftColDef *defs, int n)
{
	SiftColSchema *s = MemoryContextAllocZero(mcxt, sizeof(SiftColSchema));

	s->ncols = n;
	s->cols = MemoryContextAlloc(mcxt, n * sizeof(SiftColDef));
	for (int i = 0; i < n; i++)
	{
		const char *path = defs[i].path ? defs[i].path : "";
		size_t		len = strlen(path);
		char	   *copy = MemoryContextAlloc(mcxt, len + 1);

		memcpy(copy, path, len + 1);
		s->cols[i] = defs[i];
		s->cols[i].path = copy;
		s->ngroups += defs[i].kind == SIFT_COLK_GROUP;
		s->wide |= sift_col_def_needs_wide(&defs[i]);
		s->kinds |= sift_col_def_has_kinds(&defs[i]);
	}
	s->hash = schema_hash(defs, n);
	return s;
}

SiftColSchema *
sift_cschema_create(MemoryContext mcxt, const SiftColDef *defs, int n)
{
	const char *why = schema_check(defs, n);

	if (why)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid sift_col leaf list: %s", why)));
	return schema_build(mcxt, defs, n);
}

void
sift_cschema_serialize(const SiftColSchema *s, StringInfo out)
{
	uint32		head[3] = {COLS_MAGIC, (uint32) s->ncols, s->hash};

	put_val(out, head);
	for (int i = 0; i < s->ncols; i++)
		schema_put_def(out, &s->cols[i], true);
}

SiftColSchema *
sift_cschema_parse(MemoryContext mcxt, const void *data, Size len)
{
	const uint8 *p = data;
	uint32		head[3];
	SiftColDef *defs;
	SiftColSchema *s = NULL;
	Size		pos = sizeof(head);

	if (len < sizeof(head))
		return NULL;
	memcpy(head, p, sizeof(head));
	if (head[0] != COLS_MAGIC || head[1] < 1 || head[1] > SIFT_COL_MAX_COLS)
		return NULL;
	defs = palloc0(head[1] * sizeof(SiftColDef));
	for (uint32 i = 0; i < head[1]; i++)
	{
		SiftColDefDisk disk;
		char	   *path;

		if (len - pos < sizeof(disk))
			goto out;
		memcpy(&disk, p + pos, sizeof(disk));
		pos += sizeof(disk);
		if (len - pos < disk.pathlen)
			goto out;
		path = palloc(disk.pathlen + 1);
		memcpy(path, p + pos, disk.pathlen);
		path[disk.pathlen] = '\0';
		pos += disk.pathlen;
		defs[i] = (SiftColDef) {disk.id, disk.parent, disk.kind, disk.store, disk.flags, disk.attno, disk.type, path, disk.scale, disk.lanes, disk.lane};
	}
	if (schema_check(defs, (int) head[1]) == NULL && schema_hash(defs, (int) head[1]) == head[2])
		s = schema_build(mcxt, defs, (int) head[1]);
out:
	for (uint32 i = 0; i < head[1]; i++)
		if (defs[i].path)
			pfree((char *) defs[i].path);
	pfree(defs);
	return s;
}

/* ---------------------------------------------------------------------------
 * 2. Dictionaries
 * ------------------------------------------------------------------------- */

#define DICT_MINCAP		16

SiftCDict *
sift_cdict_create(MemoryContext mcxt)
{
	SiftCDict  *d = MemoryContextAllocZero(mcxt, sizeof(SiftCDict));

	d->mcxt = mcxt;
	d->cap = DICT_MINCAP;
	d->off = MemoryContextAllocZero(mcxt, (d->cap + 1) * sizeof(uint32));
	d->bytescap = 256;
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	return d;
}

static void
cdict_reset(SiftCDict *d)
{
	d->n = 0;
	d->nbytes = 0;
	d->off[0] = 0;
	if (d->slots)
		memset(d->slots, 0, d->nslots * sizeof(uint32));
}

static inline uint32
cdict_len(const SiftCDict *d, uint32 code)
{
	return d->off[code + 1] - d->off[code];
}

static uint32 *
cdict_probe(const SiftCDict *d, const char *s, uint32 len)
{
	uint32		mask = d->nslots - 1;

	for (uint32 i = hash_bytes((const unsigned char *) s, (int) len) & mask;; i = (i + 1) & mask)
	{
		uint32		c = d->slots[i];

		if (c == 0 || (cdict_len(d, c - 1) == len && memcmp(d->bytes + d->off[c - 1], s, len) == 0))
			return &d->slots[i];
	}
}

/* The hash table exists and has room for one more code at load <= 1/2 (rebuilt by doubling) */
static void
cdict_ready(SiftCDict *d)
{
	if (d->slots != NULL && (d->n + 1) * 2 <= d->nslots)
		return;
	if (d->slots)
		pfree(d->slots);
	d->nslots = pg_nextpower2_32(Max(DICT_MINCAP, d->n + 1) * 4);
	d->slots = MemoryContextAllocZero(d->mcxt, d->nslots * sizeof(uint32));
	for (uint32 c = 0; c < d->n; c++)
		*cdict_probe(d, d->bytes + d->off[c], cdict_len(d, c)) = c + 1;
}

uint32
sift_cdict_intern(SiftCDict *d, const char *s, uint32 len)
{
	uint32	   *slot;

	if (len > SIFT_COL_MAX_VALUE || d->nbytes + len > SIFT_COL_MAX_VALUE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("value of %u bytes does not fit a sift_col column (the strings of a unit are at most %d bytes)", len, SIFT_COL_MAX_VALUE)));
	cdict_ready(d);
	slot = cdict_probe(d, s, len);
	if (*slot)
		return *slot - 1;
	if (d->n == d->cap)
	{
		d->cap *= 2;
		d->off = repalloc(d->off, (d->cap + 1) * sizeof(uint32));
	}
	if (d->nbytes + len > d->bytescap)
	{
		d->bytescap = Max(d->bytescap * 2, d->nbytes + len);
		d->bytes = repalloc(d->bytes, d->bytescap);
	}
	memcpy(d->bytes + d->nbytes, s, len);
	d->nbytes += len;
	d->off[d->n + 1] = (uint32) d->nbytes;
	*slot = d->n + 1;
	return d->n++;
}

int64
sift_cdict_find(SiftCDict *d, const char *s, uint32 len)
{
	if (d->n == 0)
		return -1;
	cdict_ready(d);
	return (int64) *cdict_probe(d, s, len) - 1;
}

const char *
sift_cdict_str(const SiftCDict *d, uint32 code, uint32 *len)
{
	*len = cdict_len(d, code);
	return d->bytes + d->off[code];
}

/*
 * A serialized dictionary starts with u32 form, u32 n. The plain form (0) goes on with u32 off[n + 1] and the bytes; the string list form
 * (1) with the codec's list (the lengths as an integer cascade, the bytes under the cold wrapper rule), which starts 8-aligned.
 */
#define DICT_PLAIN		0
#define DICT_STRS		1
#define DICT_HEAD		(2 * sizeof(uint32))

void
sift_cdict_serialize(const SiftCDict *d, const SiftCcPolicy *pol, StringInfo out)
{
	Size		plain = sizeof(uint32) * (d->n + 1) + d->nbytes;
	uint32		form = DICT_PLAIN;
	StringInfoData z;

	z.data = NULL;
	if (pol != NULL && pol->mode != SIFT_CCM_NONE && d->n <= SIFT_CC_MAXROWS && plain >= SIFT_CC_MIN_WRAP)
	{
		initStringInfo(&z);
		sift_cc_encode_strs(&z, d->off, d->bytes, d->n, pol);
		if ((Size) z.len < plain)
			form = DICT_STRS;
	}
	put_val(out, form);
	put_val(out, d->n);
	if (form == DICT_STRS)
		put_bytes(out, z.data, z.len);
	else
	{
		put_bytes(out, d->off, (d->n + 1) * sizeof(uint32));
		put_bytes(out, d->bytes, d->nbytes);
	}
	if (z.data)
		pfree(z.data);
}

/* A dictionary of n strings: off[n + 1] and bytes[off[n]] are copied */
static SiftCDict *
cdict_from(MemoryContext mcxt, uint32 n, const uint32 *off, const char *bytes)
{
	SiftCDict  *d;

	if (off[0] != 0)
		return NULL;
	d = sift_cdict_create(mcxt);
	d->n = n;
	d->cap = Max(n, DICT_MINCAP);
	d->off = MemoryContextAlloc(mcxt, ((Size) d->cap + 1) * sizeof(uint32));
	memcpy(d->off, off, ((Size) n + 1) * sizeof(uint32));
	d->nbytes = d->off[n];
	for (uint32 c = 0; c < n; c++)
		if (d->off[c + 1] < d->off[c] || cdict_len(d, c) > SIFT_COL_DICT_MAX_ENTRY)
			return NULL;
	d->bytescap = Max(d->nbytes, 256);
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	memcpy(d->bytes, bytes, d->nbytes);
	return d;
}

SiftCDict *
sift_cdict_parse(MemoryContext mcxt, const void *data, Size len, Size *used)
{
	const uint8 *p = data;
	uint32		head[2],
				nbytes;

	if (len < DICT_HEAD)
		return NULL;
	memcpy(head, p, DICT_HEAD);
	p += DICT_HEAD;
	len -= DICT_HEAD;
	if (head[0] == DICT_STRS)
	{
		uint32	   *off;
		char	   *bytes;
		SiftCDict  *d;

		if (sift_cc_decode_strs(mcxt, p, len, &off, &bytes) != head[1])
			return NULL;
		d = cdict_from(mcxt, head[1], off, bytes);
		pfree(off);
		pfree(bytes);
		*used = DICT_HEAD + len;
		return d;
	}
	if (head[0] != DICT_PLAIN || len / sizeof(uint32) < (Size) head[1] + 1)
		return NULL;
	memcpy(&nbytes, p + sizeof(uint32) * head[1], sizeof(nbytes));
	if (len - sizeof(uint32) * ((Size) head[1] + 1) < nbytes)
		return NULL;
	*used = DICT_HEAD + sizeof(uint32) * ((Size) head[1] + 1) + nbytes;
	return cdict_from(mcxt, head[1], (const uint32 *) p, (const char *) (p + sizeof(uint32) * ((Size) head[1] + 1)));
}

void
sift_cdict_view(const SiftCDict *d, uint64 id, SiftColDict *view)
{
	view->n = d->n;
	view->off = d->off;
	view->bytes = d->bytes;
	view->id = id;
}

/* ---------------------------------------------------------------------------
 * 3. Batches
 * ------------------------------------------------------------------------- */

static inline bool
leaf_is_str(const SiftColDef *d)
{
	return d->store == SIFT_COLS_STR && d->kind == SIFT_COLK_SCALAR;
}

SiftColBatch *
sift_cbatch_create(MemoryContext mcxt, const SiftColSchema *schema)
{
	SiftColBatch *b = MemoryContextAllocZero(mcxt, sizeof(SiftColBatch));

	b->mcxt = mcxt;
	b->schema = schema;
	b->rowcap = 256;
	b->keys = MemoryContextAlloc(mcxt, b->rowcap * sizeof(SiftKey));
	b->col = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftCVals));
	b->dict = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftCDict *));
	for (int i = 0; i < schema->ncols; i++)
	{
		if (leaf_is_str(&schema->cols[i]))
			b->dict[i] = sift_cdict_create(mcxt);
		if (schema->cols[i].kind == SIFT_COLK_GROUP)
			b->col[i].eoff = MemoryContextAllocZero(mcxt, (b->rowcap + 1) * sizeof(uint32));
	}
	return b;
}

void
sift_cbatch_reset(SiftColBatch *b, uint32 sb)
{
	b->sb = sb;
	b->nrows = 0;
	b->hasver = false;
	for (int i = 0; i < b->schema->ncols; i++)
	{
		SiftCVals  *v = &b->col[i];

		if (v->nulls)
			memset(v->nulls, 0, words_of(v->n) * sizeof(uint64));
		if (v->ovf)
			memset(v->ovf, 0, words_of(v->n) * sizeof(uint64));
		if (v->otag)
			memset(v->otag, 0, v->n);
		v->n = 0;
		if (v->eoff)
			v->eoff[0] = 0;
		if (v->voff)
			v->voff[0] = 0;
		if (b->dict[i])
			cdict_reset(b->dict[i]);
	}
}

static void
bitmap_set(MemoryContext mcxt, uint64 **bits, uint32 cap, uint32 i)
{
	if (*bits == NULL)
		*bits = MemoryContextAllocZero(mcxt, words_of(cap) * sizeof(uint64));
	(*bits)[i >> 6] |= UINT64CONST(1) << (i & 63);
}

/* Appends one slot of a leaf: the key (0 for a NULL), whether it is NULL, whether it overflowed (then also NULL) and its tag: the JSON type of an overflow value (0: not known), else its kind (0: the leaf's own) */
static void
vals_put(MemoryContext mcxt, SiftCVals *v, int64 key, bool isnull, bool ovf, uint8 tag)
{
	if (v->n == v->cap)
	{
		uint32		cap = Max(64, v->cap * 2);

		v->key = v->key ? repalloc(v->key, cap * sizeof(int64)) : MemoryContextAlloc(mcxt, cap * sizeof(int64));
		if (v->nulls)
		{
			v->nulls = repalloc(v->nulls, words_of(cap) * sizeof(uint64));
			memset(v->nulls + words_of(v->cap), 0, (words_of(cap) - words_of(v->cap)) * sizeof(uint64));
		}
		if (v->ovf)
		{
			v->ovf = repalloc(v->ovf, words_of(cap) * sizeof(uint64));
			memset(v->ovf + words_of(v->cap), 0, (words_of(cap) - words_of(v->cap)) * sizeof(uint64));
		}
		if (v->otag)
		{
			v->otag = repalloc(v->otag, cap);
			memset(v->otag + v->cap, 0, cap - v->cap);
		}
		v->cap = cap;
	}
	v->key[v->n] = isnull ? 0 : key;
	if (isnull)
		bitmap_set(mcxt, &v->nulls, v->cap, v->n);
	if (ovf)
		bitmap_set(mcxt, &v->ovf, v->cap, v->n);
	if (tag != SIFT_COLTAG_UNKNOWN)
	{
		if (v->otag == NULL)
			v->otag = MemoryContextAllocZero(mcxt, v->cap);
		v->otag[v->n] = tag;
	}
	v->n++;
}

/* Appends one slot of a VAR leaf: the bytes of the value (none for a NULL) */
static void
vals_put_var(MemoryContext mcxt, SiftCVals *v, const char *s, uint32 len, bool isnull, bool ovf, uint8 tag)
{
	uint32		start = v->n == 0 ? 0 : v->voff[v->n];

	if (isnull)
		len = 0;
	if ((uint64) start + len > (uint64) PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift_col: the variable-length values of a unit exceed 2 GB")));
	if (v->voff == NULL)
	{
		v->voff = MemoryContextAlloc(mcxt, sizeof(uint32));
		v->voff[0] = 0;
	}
	if (v->n == v->cap)
		v->voff = repalloc(v->voff, ((Size) Max(64, v->cap * 2) + 1) * sizeof(uint32));
	vals_put(mcxt, v, len, isnull, ovf, tag);
	if (v->vb == NULL || start + len > v->vcap)
	{
		v->vcap = Max(Max(1024, v->vcap * 2), (Size) start + len);
		v->vb = v->vb ? repalloc(v->vb, v->vcap) : MemoryContextAlloc(mcxt, v->vcap);
	}
	if (len > 0)
		memcpy(v->vb + start, s, len);
	v->voff[v->n] = start + len;
}

/* Validates the key against the batch and makes room for the row; the row is counted by batch_row_end */
static void
batch_row_begin(SiftColBatch *b, SiftKey key)
{
	if (sift_block_sb(sift_key_block(key)) != b->sb)
		elog(ERROR, "sift_col: row %u/%u is outside superblock %u", sift_key_block(key), (uint32) (key & 0xFFFF), b->sb);
	if ((key & 0xFFFF) < 1 || (key & 0xFFFF) > MaxHeapTuplesPerPage)
		elog(ERROR, "sift_col: row offset %u is not a heap offset", (uint32) (key & 0xFFFF));
	if (b->nrows > 0 && key <= b->keys[b->nrows - 1])
		elog(ERROR, "sift_col: rows must be added in ascending TID order");
	if (b->nrows == b->rowcap)
	{
		b->rowcap *= 2;
		b->keys = repalloc(b->keys, b->rowcap * sizeof(SiftKey));
		if (b->xmin)
		{
			b->xmin = repalloc(b->xmin, b->rowcap * sizeof(uint32));
			b->cmin = repalloc(b->cmin, b->rowcap * sizeof(uint32));
		}
		for (int i = 0; i < b->schema->ncols; i++)
			if (b->col[i].eoff)
				b->col[i].eoff = repalloc(b->col[i].eoff, ((Size) b->rowcap + 1) * sizeof(uint32));
	}
	b->keys[b->nrows] = key;
}

void
sift_cbatch_set_ver(SiftColBatch *b, uint32 r, uint32 xmin, uint32 cmin)
{
	if (!b->hasver)
	{
		if (xmin == 0)
			return;
		if (b->xmin == NULL)
		{
			b->xmin = MemoryContextAlloc(b->mcxt, b->rowcap * sizeof(uint32));
			b->cmin = MemoryContextAlloc(b->mcxt, b->rowcap * sizeof(uint32));
		}
		memset(b->xmin, 0, b->rowcap * sizeof(uint32));
		memset(b->cmin, 0, b->rowcap * sizeof(uint32));
		b->hasver = true;
	}
	b->xmin[r] = xmin;
	b->cmin[r] = xmin ? cmin : 0;
}

static void
batch_group_end(SiftColBatch *b, int group, uint32 count)
{
	b->col[group].eoff[b->nrows + 1] = b->col[group].eoff[b->nrows] + count;
}

static void
put_value(SiftColBatch *b, int id, const SiftColValue *v)
{
	bool		isnull = v->isnull || v->overflow;
	int64		key = 0;

	if (b->schema->cols[id].store == SIFT_COLS_VAR)
	{
		vals_put_var(b->mcxt, &b->col[id], v->v.s, v->len, isnull, v->overflow, v->tag);
		return;
	}
	if (!isnull)
		switch (b->schema->cols[id].store)
		{
			case SIFT_COLS_INT:
				key = v->v.i;
				break;
			case SIFT_COLS_STR:
				key = sift_cdict_intern(b->dict[id], v->v.s, v->len);
				break;
			default:
				key = sift_col_f8_key(v->v.f);
		}
	vals_put(b->mcxt, &b->col[id], key, isnull, v->overflow, v->tag);
}

bool
sift_cbatch_kinds_exact(const SiftColBatch *b)
{
	for (int c = 0; c < b->schema->ncols; c++)
		for (uint32 i = 0; b->col[c].otag != NULL && i < b->col[c].n; i++)
			if (b->col[c].otag[i] == SIFT_COLTAG_UNSURE)
				return false;
	return true;
}

void
sift_cbatch_add(SiftColBatch *b, const SiftColRow *row)
{
	sift_cbatch_add_ver(b, row, 0, 0);
}

void
sift_cbatch_add_ver(SiftColBatch *b, const SiftColRow *row, uint32 xmin, uint32 cmin)
{
	const SiftColSchema *s = b->schema;

	batch_row_begin(b, sift_key_from_tid((ItemPointer) &row->tid));
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColValue *v = &row->val[id];
		uint32		count = 0;

		if (s->cols[id].parent >= 0)
			continue;
		put_value(b, id, v);
		if (s->cols[id].kind != SIFT_COLK_GROUP)
			continue;
		if (!v->isnull && !v->overflow)
		{
			if (v->v.i < 0 || v->v.i > PG_INT32_MAX)
				elog(ERROR, "sift_col: array of %lld elements", (long long) v->v.i);
			count = (uint32) v->v.i;
		}
		batch_group_end(b, id, count);
		for (int c = id + 1; c < s->ncols; c++)
			if (s->cols[c].parent == id)
				for (uint32 j = 0; j < count; j++)
					put_value(b, c, &row->elem[c][j]);
	}
	if (b->hasver || xmin)
		sift_cbatch_set_ver(b, b->nrows, xmin, cmin);
	b->nrows++;
}

void
sift_cbatch_value(const SiftColBatch *b, int col, uint32 slot, SiftColValue *v)
{
	const SiftCVals *vals = &b->col[col];
	int64		key = vals->key[slot];

	memset(v, 0, sizeof(*v));
	v->isnull = sift_col_bit_test(vals->nulls, slot);
	v->overflow = sift_col_bit_test(vals->ovf, slot);
	v->tag = vals->otag ? vals->otag[slot] : SIFT_COLTAG_UNKNOWN;
	if (v->isnull)
		return;
	switch (b->schema->cols[col].store)
	{
		case SIFT_COLS_INT:
			v->v.i = key;
			break;
		case SIFT_COLS_STR:
			v->v.s = b->dict[col]->bytes + b->dict[col]->off[key];
			v->len = cdict_len(b->dict[col], (uint32) key);
			break;
		case SIFT_COLS_VAR:
			v->v.s = vals->vb + vals->voff[slot];
			v->len = (uint32) key;
			break;
		default:
			v->v.f = sift_col_f8_from_key(key);
	}
}

/* The elements of a row's arrays live in one block; elem[ncols] points at its values, the header holds the capacity */
typedef struct ElemBlock
{
	Size		cap;
	SiftColValue v[FLEXIBLE_ARRAY_MEMBER];
} ElemBlock;

#define ELEM_BLOCK(p)	((ElemBlock *) ((char *) (p) - offsetof(ElemBlock, v)))

void
sift_cbatch_row(const SiftColBatch *b, uint32 r, SiftColRow *row)
{
	const SiftColSchema *s = b->schema;
	Size		total = 0;
	SiftColValue *next;

	if (row->val == NULL)
	{
		row->val = MemoryContextAllocZero(b->mcxt, s->ncols * sizeof(SiftColValue));
		row->elem = MemoryContextAllocZero(b->mcxt, (s->ncols + 1) * sizeof(SiftColValue *));
	}
	row->ncols = s->ncols;
	sift_key_to_tid(b->keys[r], &row->tid);
	for (int id = 0; id < s->ncols; id++)
		if (s->cols[id].kind == SIFT_COLK_GROUP)
			for (int c = id + 1; c < s->ncols; c++)
				if (s->cols[c].parent == id)
					total += b->col[id].eoff[r + 1] - b->col[id].eoff[r];

	if (total > 0 && (row->elem[s->ncols] == NULL || ELEM_BLOCK(row->elem[s->ncols])->cap < total))
	{
		ElemBlock  *blk;

		if (row->elem[s->ncols])
			pfree(ELEM_BLOCK(row->elem[s->ncols]));
		blk = MemoryContextAlloc(b->mcxt, offsetof(ElemBlock, v) + Max(total, 16) * sizeof(SiftColValue));
		blk->cap = Max(total, 16);
		row->elem[s->ncols] = blk->v;
	}
	next = row->elem[s->ncols];
	memset(row->elem, 0, s->ncols * sizeof(SiftColValue *));
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColDef *d = &s->cols[id];

		if (d->parent >= 0)
		{
			memset(&row->val[id], 0, sizeof(SiftColValue));
			row->val[id].isnull = true;
			continue;
		}
		sift_cbatch_value(b, id, r, &row->val[id]);
		if (d->kind != SIFT_COLK_GROUP)
			continue;
		for (int c = id + 1; c < s->ncols; c++)
			if (s->cols[c].parent == id)
			{
				uint32		first = b->col[id].eoff[r],
							count = b->col[id].eoff[r + 1] - first;

				row->elem[c] = next;
				for (uint32 j = 0; j < count; j++)
					sift_cbatch_value(b, c, first + j, next++);
			}
	}
}

void
sift_cbatch_row_free(SiftColRow *row)
{
	if (row->val == NULL)
		return;
	if (row->elem[row->ncols])
		pfree(ELEM_BLOCK(row->elem[row->ncols]));
	pfree(row->elem);
	pfree(row->val);
	memset(row, 0, sizeof(*row));
}

static void
batch_copy_row(SiftColBatch *dst, const SiftColBatch *src, uint32 r, SiftColRow *scratch)
{
	sift_cbatch_row(src, r, scratch);
	sift_cbatch_add_ver(dst, scratch, src->hasver ? src->xmin[r] : 0, src->hasver ? src->cmin[r] : 0);
}

void
sift_cbatch_add_from(SiftColBatch *dst, const SiftColBatch *src, uint32 r)
{
	SiftColRow	row = {0};

	batch_copy_row(dst, src, r, &row);
	sift_cbatch_row_free(&row);
}

void
sift_cbatch_merge(SiftColBatch *dst, SiftColBatch *const *src, int nsrc)
{
	uint32	   *pos = palloc0(nsrc * sizeof(uint32));
	SiftColRow	row = {0};

	for (;;)
	{
		int			first = -1;
		SiftKey		min = 0;

		for (int i = 0; i < nsrc; i++)
			if (pos[i] < src[i]->nrows && (first < 0 || src[i]->keys[pos[i]] < min))
			{
				first = i;
				min = src[i]->keys[pos[i]];
			}
		if (first < 0)
			break;
		batch_copy_row(dst, src[first], pos[first], &row);
		for (int i = 0; i < nsrc; i++)
			if (pos[i] < src[i]->nrows && src[i]->keys[pos[i]] == min)
				pos[i]++;
	}
	sift_cbatch_row_free(&row);
	pfree(pos);
}

/* ---------------------------------------------------------------------------
 * 4. Zones
 * ------------------------------------------------------------------------- */

/* The one definition of a zone: the encoder and the verification of a stored chunk both come through here */
static void
zone_of_keys(const int64 *key, const uint64 *nulls, const uint64 *ovf, uint32 n, SiftColStore store, SiftCZone *z)
{
	int64		mn = PG_INT64_MAX,
				mx = PG_INT64_MIN,
				carry = 0;		/* the sum is carry * 2^64 + sum (unsigned): exact, whatever the order of the additions */
	uint64		sum = 0;
	uint32		nnull = 0;
	bool		nosum;

	memset(z, 0, sizeof(*z));
	z->nvals = n;
	for (uint32 i = 0; i < n; i++)
	{
		int64		k = key[i];

		if (sift_col_bit_test(nulls, i))
		{
			nnull++;
			continue;
		}
		mn = Min(mn, k);
		mx = Max(mx, k);
		if (store == SIFT_COLS_INT || store == SIFT_COLS_VAR)		/* VAR: the keys are the lengths, their zone is no use but is exact */
		{
			uint64		before = sum;

			sum += (uint64) k;
			carry += (sum < before) - (k < 0);
		}
		else if (store == SIFT_COLS_STR)
			z->set[(k & 255) >> 6] |= UINT64CONST(1) << (k & 63);
		else
		{
			double		d = sift_col_f8_from_key(k);

			z->sum_f += d;
			z->flags |= d != d ? SIFT_COLZ_NAN : 0;
		}
	}
	z->nnull = nnull;
	if (nnull < n)
	{
		z->min = mn;
		z->max = mx;
		z->flags |= mn == mx ? SIFT_COLZ_CONST : 0;
	}
	nosum = !((carry == 0 && (int64) sum >= 0) || (carry == -1 && (int64) sum < 0));	/* the sum is outside int64 */
	z->sum_i = nosum ? 0 : (int64) sum;
	z->flags |= nosum ? SIFT_COLZ_NOSUM : 0;
	for (uint32 w = 0; ovf != NULL && w < words_of(n); w++)
		z->flags |= ovf[w] ? SIFT_COLZ_OVERFLOW : 0;
}

/* ---------------------------------------------------------------------------
 * 5. Codec seam: the plain FOR and PACK encodings of the design notes. col-compress replaces this section (what a chunk of a given span
 * looks like, its size, its validity, and how it is written and read); the rest of the file only goes through these functions.
 * ------------------------------------------------------------------------- */

#define CODEC_BLOCK		4096	/* values unpacked at a time (a multiple of 64) */

static inline uint32
codec_slot(uint8 bits)
{
	return pg_rightmost_one_pos32(bits) - 3;	/* 8, 16, 32, 64 -> SIFT_COLW_8 .. SIFT_COLW_64 */
}

static bool
codec_valid(uint8 enc, uint8 bits)
{
	switch (enc)
	{
		case SIFT_COLENC_CONST:
			return bits == 0;
		case SIFT_COLENC_FOR:
			return bits == 8 || bits == 16 || bits == 32 || bits == 64;
		case SIFT_COLENC_PACK:
			return bits >= 1 && bits <= 31 && bits != 8 && bits != 16;
		case SIFT_COLENC_CC:
		case SIFT_COLENC_STRS:
		case SIFT_COLENC_ISTR:
			return bits == 0;
		default:
			return false;
	}
}

/* Payload bytes of n values */
static uint64
codec_paylen(uint8 enc, uint8 bits, uint32 n)
{
	if (enc == SIFT_COLENC_CONST)
		return 0;
	return enc == SIFT_COLENC_FOR ? (uint64) n * (bits / 8) : (uint64) words_of(n) * 8 * bits;
}

/* The smallest encoding that holds values 0..span */
static void
codec_choose(uint64 span, uint32 n, uint8 *enc, uint8 *bits)
{
	uint8		w = span == 0 ? 0 : pg_leftmost_one_pos64(span) + 1,
				fw = w <= 8 ? 8 : w <= 16 ? 16 : w <= 32 ? 32 : 64;

	*enc = SIFT_COLENC_FOR;
	*bits = fw;
	if (w == 0)
	{
		*enc = SIFT_COLENC_CONST;
		*bits = 0;
	}
	else if (codec_valid(SIFT_COLENC_PACK, w) && codec_paylen(SIFT_COLENC_PACK, w, n) < codec_paylen(SIFT_COLENC_FOR, fw, n))
	{
		*enc = SIFT_COLENC_PACK;
		*bits = w;
	}
}

/* Appends the payload of the n unsigned values u[] (each below 2^bits) */
static void
codec_put(StringInfo out, uint8 enc, uint8 bits, const uint64 *u, uint32 n)
{
	Size		len = codec_paylen(enc, bits, n);
	uint8	   *dst;

	enlargeStringInfo(out, (int) len);
	dst = (uint8 *) out->data + out->len;
	if (enc == SIFT_COLENC_FOR)
		for (uint32 i = 0; i < n; i++)
		{
			switch (bits)
			{
				case 8:
					dst[i] = (uint8) u[i];
					break;
				case 16:
					{
						uint16		x = (uint16) u[i];

						memcpy(dst + 2 * i, &x, sizeof(x));
					}
					break;
				case 32:
					{
						uint32		x = (uint32) u[i];

						memcpy(dst + 4 * i, &x, sizeof(x));
					}
					break;
				default:
					memcpy(dst + 8 * i, &u[i], sizeof(uint64));
			}
		}
	else
		for (uint32 s = 0; s < words_of(n) * 64; s += 64)
		{
			uint64		acc = 0;
			int			nb = 0;

			for (uint32 i = s; i < s + 64; i++)
			{
				acc |= (i < n ? u[i] : 0) << nb;
				nb += bits;
				if (nb >= 32)
				{
					uint32		x = (uint32) acc;

					memcpy(dst, &x, sizeof(x));
					dst += 4;
					acc >>= 32;
					nb -= 32;
				}
			}
		}
	out->len += (int) len;
	out->data[out->len] = '\0';
}

/* The codec's reader of a CC chunk (the blob is validated; verify also checks its check word) */
static SiftCc *
cc_open(const SiftCChunk *c, bool verify)
{
	SiftCc	   *cc = sift_cc_open(CurrentMemoryContext, c->payload, c->paylen, verify);

	if (sift_cc_nrows(cc) != c->n)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: the blob has %u values, the chunk %u",
																 sift_cc_nrows(cc), c->n)));
	return cc;
}

/*
 * The payload of an inline STR chunk: u32 length of the prefix blob, u32 0, the prefix blob (a codec blob of the n keys sift_col_str_prefix of
 * the strings, NULL slots filler: filters answer "which rows can hold this string" on the encoded form), then the string list.
 */
static bool
istr_split(const SiftCChunk *c, const uint8 **pre, uint32 *prelen, const uint8 **strs, uint32 *strslen)
{
	uint32		plen,
				zero;

	if (c->paylen < 8)
		return false;
	memcpy(&plen, c->payload, sizeof(plen));
	memcpy(&zero, c->payload + 4, sizeof(zero));
	if (zero != 0 || plen == 0 || plen % 8 != 0 || (uint64) plen + 8 + 8 > c->paylen)
		return false;
	*pre = c->payload + 8;
	*prelen = plen;
	*strs = c->payload + 8 + plen;
	*strslen = c->paylen - 8 - plen;
	return true;
}

void
sift_cchunk_strs(const SiftCChunk *c, MemoryContext mcxt, uint32 **off, char **bytes)
{
	const uint8 *pl = c->payload;
	uint32		len = c->paylen;

	if (c->enc != SIFT_COLENC_STRS && c->enc != SIFT_COLENC_ISTR)
		elog(ERROR, "sift_col: chunk encoding %d holds no strings", c->enc);
	if (c->enc == SIFT_COLENC_ISTR)
	{
		const uint8 *pre;
		uint32		prelen;

		if (!istr_split(c, &pre, &prelen, &pl, &len))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: the inline text payload is malformed")));
	}
	if (sift_cc_decode_strs(mcxt, pl, len, off, bytes) != c->n)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: the string list does not hold %u values", c->n)));
}

/* The lengths of the strings of a STRS or ISTR chunk, the keys of its VAR zone and of its row tests */
static void
strs_lengths(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	uint32	   *off;
	char	   *bytes;

	sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	for (uint32 k = 0; k < m; k++)
		dst[k] = off[(idx ? idx[k] : k) + 1] - off[idx ? idx[k] : k];
	pfree(off);
	pfree(bytes);
}

/* dst[0..n) = base + payload (the NULL slots hold base) */
static void
codec_decode(const SiftCChunk *c, int64 *dst)
{
	uint32		tmp[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

	switch (c->enc)
	{
		case SIFT_COLENC_STRS:
		case SIFT_COLENC_ISTR:
			strs_lengths(c, NULL, c->n, dst);
			break;
		case SIFT_COLENC_CC:
			{
				SiftCc	   *cc = cc_open(c, false);

				sift_cc_decode(cc, dst);
				sift_cc_close(cc);
			}
			break;
		case SIFT_COLENC_CONST:
			for (uint32 i = 0; i < c->n; i++)
				dst[i] = c->base;
			break;
		case SIFT_COLENC_FOR:
			sift_colk->for_i64[codec_slot(c->bits)](c->payload, c->n, c->base, dst);
			break;
		default:
			for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
			{
				uint32		m = Min(CODEC_BLOCK, c->n - s);

				sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, tmp);
				sift_colk->for_i64[SIFT_COLW_32](tmp, m, c->base, dst + s);
			}
	}
}

static void
codec_decode_sel(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	uint32	   *tmp;

	switch (c->enc)
	{
		case SIFT_COLENC_STRS:
		case SIFT_COLENC_ISTR:
			strs_lengths(c, idx, m, dst);
			break;
		case SIFT_COLENC_CC:
			{
				SiftCc	   *cc = cc_open(c, false);

				sift_cc_decode_sel(cc, idx, m, dst);
				sift_cc_close(cc);
			}
			break;
		case SIFT_COLENC_CONST:
			for (uint32 k = 0; k < m; k++)
				dst[k] = c->base;
			break;
		case SIFT_COLENC_FOR:
			sift_colk->for_i64_sel[codec_slot(c->bits)](c->payload, idx, m, c->base, dst);
			break;
		default:
			tmp = palloc(c->n * sizeof(uint32) + SIFT_COL_PAD);
			sift_colk->unpack(c->payload, c->bits, c->n, tmp);
			sift_colk->for_i64_sel[SIFT_COLW_32](tmp, idx, m, c->base, dst);
			pfree(tmp);
	}
}

/* The widest payload value an encoding can hold */
static uint64
codec_maxval(const SiftCChunk *c)
{
	return c->enc == SIFT_COLENC_CONST ? 0 : c->bits >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << c->bits) - 1;
}

/* ---------------------------------------------------------------------------
 * 6. Chunks
 * ------------------------------------------------------------------------- */

#define CHUNK_HEAD		16
#define CHUNK_MAX_N		(1U << 28)

/* Appends the bitmap of n bits (NULL: all clear), tail bits cleared */
static void
put_bitmap(StringInfo out, const uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < words_of(n); w++)
	{
		uint64		x = bits ? bits[w] : 0;

		if (w == n / 64)
			x &= (UINT64CONST(1) << (n % 64)) - 1;
		put_val(out, x);
	}
}

/* Set bits of the bitmap of n bits */
static uint32
bitmap_count(const uint64 *bits, uint32 n)
{
	uint32		cnt = 0;

	for (uint32 w = 0; w < words_of(n); w++)
		cnt += sift_pc64(w == n / 64 ? bits[w] & ((UINT64CONST(1) << (n % 64)) - 1) : bits[w]);
	return cnt;
}

/* The bytes of the tag nibbles of nov overflow slots: half a byte each, padded to 8 */
static inline Size
tags_bytes(uint32 nov)
{
	return ((Size) (nov + 1) / 2 + 7) & ~(Size) 7;
}

/* Appends the tag nibble of every overflow slot of the leaf (SIFT_COLC_TAGS), in slot order */
static void
put_tags(StringInfo out, const SiftCVals *vals)
{
	Size		at = out->len;
	uint32		k = 0;

	put_zero(out, tags_bytes(bitmap_count(vals->ovf, vals->n)));
	for (uint32 w = 0; w < words_of(vals->n); w++)
		for (uint64 m = vals->ovf[w]; m; m &= m - 1, k++)
			if (vals->otag != NULL)
				out->data[at + k / 2] |= (char) (vals->otag[w * 64 + pg_rightmost_one_pos64(m)] << (k % 2 * 4));
}

/* The kinds of the leaf: bit t set when some slot that does not overflow has tag t */
static uint32
kinds_mask(const SiftCVals *vals)
{
	uint32		mask = 0;

	for (uint32 i = 0; vals->otag != NULL && i < vals->n; i++)
		if (!sift_col_bit_test(vals->ovf, i))
			mask |= 1U << vals->otag[i];
	return mask & ~(1U | (1U << SIFT_COLTAG_UNSURE));
}

/* Appends the kinds (SIFT_COLC_KINDS): the word of the tags that have a bitmap and the bitmaps */
static void
put_kinds(StringInfo out, const SiftCVals *vals, uint32 mask)
{
	uint64		head = mask;
	uint64	   *bits = palloc(words_of(vals->n) * sizeof(uint64));

	put_bytes(out, &head, sizeof(head));
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
	{
		if (!(mask >> tag & 1))
			continue;
		memset(bits, 0, words_of(vals->n) * sizeof(uint64));
		for (uint32 i = 0; i < vals->n; i++)
			if (vals->otag[i] == tag && !sift_col_bit_test(vals->ovf, i))
				bits[i / 64] |= UINT64CONST(1) << (i % 64);
		put_bitmap(out, bits, vals->n);
	}
	pfree(bits);
}

/* The flags of a chunk of the leaf: NULLs and overflow follow the zone, and the overflow comes with its tags and the kinds (kmask) when the unit is written with them */
static uint8
chunk_flags(const SiftCZone *z, bool tags, uint32 kmask)
{
	bool		ovf = (z->flags & SIFT_COLZ_OVERFLOW) != 0;

	return (z->nnull > 0 ? SIFT_COLC_NULLS : 0) | (ovf ? SIFT_COLC_OVF : 0) | (ovf && tags ? SIFT_COLC_TAGS : 0) | (kmask ? SIFT_COLC_KINDS : 0);
}

/* Appends the chunk of n keys (NULL slots hold 0) and returns its zone; pol: the codec's policy of the leaf, NULL for the plain form */
static void
chunk_encode(StringInfo out, SiftColStore store, const SiftCVals *vals, const int64 *key, const SiftCcPolicy *pol, bool tags, SiftCZone *z)
{
	const uint64 *nulls = vals->nulls,
			   *ovf = vals->ovf;
	uint32		n = vals->n;
	uint8		enc,
				bits,
				flags;
	uint32		kmask = tags ? kinds_mask(vals) : 0;
	uint64		span,
			   *u;
	int64		base;
	uint8		head[8];

	zone_of_keys(key, nulls, ovf, n, store, z);
	flags = chunk_flags(z, tags, kmask);
	span = (uint64) z->max - (uint64) z->min;
	codec_choose(span, n, &enc, &bits);
	if (pol != NULL && enc != SIFT_COLENC_CONST)
		enc = SIFT_COLENC_CC, bits = 0;
	if (store == SIFT_COLS_VAR)
		enc = SIFT_COLENC_STRS, bits = 0;
	head[0] = enc;
	head[1] = bits;
	head[2] = flags;
	head[3] = 0;
	memcpy(head + 4, &n, sizeof(n));
	put_bytes(out, head, sizeof(head));
	base = enc == SIFT_COLENC_CC || enc == SIFT_COLENC_STRS ? 0 : z->min;
	put_val(out, base);
	if (flags & SIFT_COLC_NULLS)
		put_bitmap(out, nulls, n);
	if (flags & SIFT_COLC_OVF)
		put_bitmap(out, ovf, n);
	if (flags & SIFT_COLC_TAGS)
		put_tags(out, vals);
	if (flags & SIFT_COLC_KINDS)
		put_kinds(out, vals, kmask);
	if (enc == SIFT_COLENC_STRS)
	{
		SiftCcPolicy dp;

		if (pol == NULL)
			sift_cc_policy_default(&dp);
		sift_cc_encode_strs(out, vals->voff, vals->vb, n, pol ? pol : &dp);
	}
	else if (enc == SIFT_COLENC_CC)
		sift_cc_encode(out, store == SIFT_COLS_F4 ? SIFT_CC_F4 : store == SIFT_COLS_F8 ? SIFT_CC_F8 : SIFT_CC_INT, key, n, nulls, pol, NULL);
	else if (enc != SIFT_COLENC_CONST)
	{
		u = palloc(Max(n, 1) * sizeof(uint64));
		for (uint32 i = 0; i < n; i++)
			u[i] = sift_col_bit_test(nulls, i) ? 0 : (uint64) key[i] - (uint64) z->min;
		codec_put(out, enc, bits, u, n);
		pfree(u);
	}
	put_pad(out, 8);
}

/* The zone of an inline STR chunk: the prefix keys of the smallest and largest value; no set, so none prunes */
static void
istr_zone(const uint64 *nulls, const uint64 *ovf, uint32 n, const uint32 *off, const char *bytes, SiftCZone *z)
{
	int64		mn = PG_INT64_MAX,
				mx = PG_INT64_MIN;

	memset(z, 0, sizeof(*z));
	z->nvals = n;
	for (uint32 i = 0; i < n; i++)
	{
		int64		k;

		if (sift_col_bit_test(nulls, i))
		{
			z->nnull++;
			continue;
		}
		k = sift_col_str_prefix(bytes + off[i], off[i + 1] - off[i]);
		mn = Min(mn, k);
		mx = Max(mx, k);
	}
	if (z->nnull < n)
	{
		z->min = mn;
		z->max = mx;
	}
	z->flags = SIFT_COLZ_INLINE;
	for (uint32 w = 0; ovf != NULL && w < words_of(n); w++)
		z->flags |= ovf[w] ? SIFT_COLZ_OVERFLOW : 0;
	memset(z->set, 0xFF, sizeof(z->set));
}

/* The chunk of an STR leaf stored inline: the strings of the rows (the batch's codes looked up in its dictionary bd), no codes of the segment */
static void
chunk_encode_istr(StringInfo out, const SiftCVals *v, const SiftCDict *bd, const SiftCcPolicy *pol, bool tags, SiftCZone *z)
{
	uint32		n = v->n;
	uint32	   *off = palloc((Size) (n + 1) * sizeof(uint32));
	StringInfoData bytes;
	SiftCcPolicy dp;
	uint8		head[8] = {SIFT_COLENC_ISTR, 0, 0, 0};
	uint32		kmask = tags ? kinds_mask(v) : 0;

	initStringInfo(&bytes);
	off[0] = 0;
	for (uint32 i = 0; i < n; i++)
	{
		if (!sift_col_bit_test(v->nulls, i))
		{
			uint32		len;
			const char *s = sift_cdict_str(bd, (uint32) v->key[i], &len);

			appendBinaryStringInfo(&bytes, s, (int) len);
		}
		off[i + 1] = (uint32) bytes.len;
	}
	istr_zone(v->nulls, v->ovf, n, off, bytes.data, z);
	head[2] = chunk_flags(z, tags, kmask);
	memcpy(head + 4, &n, sizeof(n));
	put_bytes(out, head, sizeof(head));
	put_zero(out, sizeof(int64));		/* base */
	if (head[2] & SIFT_COLC_NULLS)
		put_bitmap(out, v->nulls, n);
	if (head[2] & SIFT_COLC_OVF)
		put_bitmap(out, v->ovf, n);
	if (head[2] & SIFT_COLC_TAGS)
		put_tags(out, v);
	if (head[2] & SIFT_COLC_KINDS)
		put_kinds(out, v, kmask);
	if (pol == NULL)
		sift_cc_policy_default(&dp);
	{
		int64	   *pre = palloc((Size) (n + 1) * sizeof(int64));
		StringInfoData pb;
		uint32		plen;

		for (uint32 i = 0; i < n; i++)
			pre[i] = sift_col_str_prefix(bytes.data + off[i], off[i + 1] - off[i]);
		initStringInfo(&pb);
		sift_cc_encode(&pb, SIFT_CC_INT, pre, n, v->nulls, pol ? pol : &dp, NULL);
		plen = (uint32) pb.len;
		put_val(out, plen);
		put_zero(out, sizeof(uint32));
		put_bytes(out, pb.data, pb.len);
		pfree(pb.data);
		pfree(pre);
	}
	sift_cc_encode_strs(out, off, bytes.data, n, pol ? pol : &dp);
	put_pad(out, 8);
	pfree(off);
	pfree(bytes.data);
}

bool
sift_cbatch_str_inline(const SiftColBatch *b, int col, SiftCDict *seg, Size maxbytes)
{
	const SiftCDict *bd = b->dict[col];
	Size		add = 0;

	if (b->col[col].n == 0)			/* no values (a group without elements): its chunk is empty */
		return false;
	for (uint32 c = 0; c < bd->n; c++)
		if (cdict_len(bd, c) > SIFT_COL_DICT_MAX_ENTRY)
			return true;
	if (seg->nbytes + bd->nbytes <= maxbytes)
		return false;
	for (uint32 c = 0; c < bd->n; c++)
		if (sift_cdict_find(seg, bd->bytes + bd->off[c], cdict_len(bd, c)) < 0)
			add += cdict_len(bd, c);
	return seg->nbytes + add > maxbytes;
}

bool
sift_cchunk_istr_codes(const SiftCChunk *c, MemoryContext mcxt, SiftCDict **dict, SiftCChunk *out)
{
	uint32	   *off,
			   *codes;
	char	   *bytes;

	*out = *c;
	*dict = NULL;
	if (c->enc != SIFT_COLENC_ISTR)
		return false;
	sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	*dict = sift_cdict_create(mcxt);
	codes = MemoryContextAllocZero(mcxt, (Size) c->n * sizeof(uint32) + SIFT_COL_PAD);
	for (uint32 i = 0; i < c->n; i++)
		if (!sift_col_bit_test(c->nulls, i))
			codes[i] = sift_cdict_intern(*dict, bytes + off[i], off[i + 1] - off[i]);
	out->enc = SIFT_COLENC_FOR;
	out->bits = 32;
	out->base = 0;
	out->payload = (const uint8 *) codes;
	out->paylen = c->n * sizeof(uint32);
	pfree(off);
	pfree(bytes);
	return true;
}

static bool tags_valid(const SiftCChunk *c);
static bool kinds_parse(const uint8 *p, uint32 len, uint64 *pos, SiftCChunk *c);
static bool kinds_valid(const SiftCChunk *c);

bool
sift_cchunk_parse(const uint8 *p, uint32 len, SiftCChunk *c)
{
	uint64		pos = CHUNK_HEAD,
				words;
	bool		whole;

	if (len < CHUNK_HEAD || ((uintptr_t) p & 7) != 0)
		return false;
	memset(c, 0, sizeof(*c));
	c->enc = p[0];
	c->bits = p[1];
	c->flags = p[2];
	memcpy(&c->n, p + 4, sizeof(c->n));
	memcpy(&c->base, p + 8, sizeof(c->base));
	if ((c->flags & ~(SIFT_COLC_NULLS | SIFT_COLC_OVF | SIFT_COLC_TAGS | SIFT_COLC_KINDS)) != 0 || ((c->flags & SIFT_COLC_OVF) && !(c->flags & SIFT_COLC_NULLS)) ||
		((c->flags & SIFT_COLC_TAGS) && !(c->flags & SIFT_COLC_OVF)) || !codec_valid(c->enc, c->bits) || c->n > CHUNK_MAX_N)
		return false;
	words = words_of(c->n);
	if (c->flags & SIFT_COLC_NULLS)
	{
		c->nulls = (const uint64 *) (p + pos);
		pos += words * 8;
	}
	if (c->flags & SIFT_COLC_OVF)
	{
		c->ovf = (const uint64 *) (p + pos);
		pos += words * 8;
	}
	if (c->flags & SIFT_COLC_TAGS)
	{
		if (pos > len)
			return false;
		c->tags = p + pos;
		pos += tags_bytes(bitmap_count(c->ovf, c->n));
	}
	if ((c->flags & SIFT_COLC_KINDS) && !kinds_parse(p, len, &pos, c))
		return false;
	whole = c->enc == SIFT_COLENC_CC || c->enc == SIFT_COLENC_STRS || c->enc == SIFT_COLENC_ISTR;
	c->paylen = whole ? (pos <= len ? (uint32) (len - pos) : 0) : (uint32) codec_paylen(c->enc, c->bits, c->n);
	if (pos + c->paylen > len || (whole && (c->paylen == 0 || c->paylen % 8 != 0 || c->n == 0)))
		return false;
	c->payload = p + pos;
	if (c->enc == SIFT_COLENC_ISTR)
	{
		const uint8 *pre,
				   *strs;
		uint32		prelen,
					strslen;

		if (!istr_split(c, &pre, &prelen, &strs, &strslen))
			return false;
	}
	if (c->n % 64 != 0)
	{
		uint64		tail = ~UINT64CONST(0) << (c->n % 64);

		if ((c->nulls && (c->nulls[words - 1] & tail)) || (c->ovf && (c->ovf[words - 1] & tail)))
			return false;
	}
	for (uint64 w = 0; c->ovf != NULL && w < words; w++)
		if (c->ovf[w] & ~c->nulls[w])
			return false;
	return (c->tags == NULL || tags_valid(c)) && kinds_valid(c);
}

static inline uint8
tag_nibble(const uint8 *tags, uint32 k)
{
	return (tags[k / 2] >> (k % 2 * 4)) & 0xF;
}

/* Every nibble is a tag, and the unused ones (the high nibble of an odd count, the padding) are zero */
static bool
tags_valid(const SiftCChunk *c)
{
	uint32		nov = bitmap_count(c->ovf, c->n);

	for (uint32 k = 0; k < nov; k++)
		if (tag_nibble(c->tags, k) > SIFT_COLTAG_OVF_MAX)
			return false;
	for (uint32 k = nov; k < tags_bytes(nov) * 2; k++)
		if (tag_nibble(c->tags, k) != 0)
			return false;
	return true;
}

/* the tags a kind can have: the type of an astext leaf's number, boolean (or string) and JSON null */
#define KINDS_ALLOWED	(1U << SIFT_COLTAG_STRING | 1U << SIFT_COLTAG_NUMBER | 1U << SIFT_COLTAG_BOOLEAN | 1U << SIFT_COLTAG_JNULL)

/* The kinds word and bitmaps at *pos: the word has only the bits of tags 1..MAX and every bitmap is there */
static bool
kinds_parse(const uint8 *p, uint32 len, uint64 *pos, SiftCChunk *c)
{
	uint64		head;
	Size		bytes = words_of(c->n) * 8;

	if (*pos + 8 > len)
		return false;
	memcpy(&head, p + *pos, sizeof(head));
	*pos += 8;
	c->kmask = (uint8) head;
	if (head != c->kmask || head == 0 || (head & ~KINDS_ALLOWED))
		return false;
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
		if (c->kmask >> tag & 1)
		{
			if (*pos + bytes > len)
				return false;
			c->kind[tag] = (const uint64 *) (p + *pos);
			*pos += bytes;
		}
	return true;
}

/* A kind is a slot that does not overflow: JSON null is a NULL slot, the others hold a value, no slot has two, and none is empty */
static bool
kinds_valid(const SiftCChunk *c)
{
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
	{
		uint64		any = 0;

		if (c->kind[tag] == NULL)
			continue;
		for (uint32 w = 0; w < words_of(c->n); w++)
		{
			uint64		m = c->kind[tag][w],
						nul = c->nulls ? c->nulls[w] : 0;

			if ((w == c->n / 64 && c->n % 64 != 0 && (m >> (c->n % 64)) != 0) || (c->ovf && (m & c->ovf[w])) ||
				(tag == SIFT_COLTAG_JNULL ? (m & ~nul) : (m & nul)))
				return false;
			for (uint8 o = 1; o < tag; o++)
				if (c->kind[o] && (m & c->kind[o][w]))
					return false;
			any |= m;
		}
		if (any == 0)
			return false;
	}
	return true;
}

uint8
sift_cchunk_ovf_tag(const SiftCChunk *c, uint32 slot)
{
	uint32		k = 0;

	if (c->tags == NULL || !sift_col_bit_test(c->ovf, slot))
		return SIFT_COLTAG_UNKNOWN;
	for (uint32 w = 0; w < slot / 64; w++)
		k += sift_pc64(c->ovf[w]);
	k += sift_pc64(c->ovf[slot / 64] & ((UINT64CONST(1) << (slot % 64)) - 1));
	return tag_nibble(c->tags, k);
}

bool
sift_cchunk_tags(const SiftCChunk *c, uint8 *dst)
{
	uint32		k = 0;

	if (c->tags == NULL && c->kmask == 0)
		return false;
	memset(dst, 0, c->n);
	for (uint32 w = 0; c->tags != NULL && w < words_of(c->n); w++)
		for (uint64 m = c->ovf[w]; m; m &= m - 1)
			dst[w * 64 + pg_rightmost_one_pos64(m)] = tag_nibble(c->tags, k++);
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
		for (uint32 w = 0; c->kind[tag] != NULL && w < words_of(c->n); w++)
			for (uint64 m = c->kind[tag][w]; m; m &= m - 1)
				dst[w * 64 + pg_rightmost_one_pos64(m)] = tag;
	return true;
}

void
sift_cchunk_tag_bits(const SiftCChunk *c, uint8 tag, uint64 *bits)
{
	uint32		k = 0;

	memset(bits, 0, words_of(c->n) * sizeof(uint64));
	for (uint32 w = 0; c->ovf != NULL && w < words_of(c->n); w++)
		for (uint64 m = c->ovf[w]; m; m &= m - 1, k++)
			if ((c->tags ? tag_nibble(c->tags, k) : SIFT_COLTAG_UNKNOWN) == tag)
				bits[w] |= m & -m;
	for (uint32 w = 0; tag != SIFT_COLTAG_UNKNOWN && c->kind[tag] != NULL && w < words_of(c->n); w++)
		bits[w] |= c->kind[tag][w];
}

/* The NULL slots of a decoded lane (any element type) are 0 */
#define ZERO_NULLS(nulls, n, dst) \
	do { \
		for (uint32 w_ = 0; (nulls) != NULL && w_ < words_of(n); w_++) \
			for (uint64 m_ = (nulls)[w_]; m_; m_ &= m_ - 1) \
				(dst)[w_ * 64 + pg_rightmost_one_pos64(m_)] = 0; \
	} while (0)

void
sift_cchunk_decode(const SiftCChunk *c, int64 *dst)
{
	codec_decode(c, dst);
	ZERO_NULLS(c->nulls, c->n, dst);
}

void
sift_cchunk_decode_u32(const SiftCChunk *c, uint32 *dst)
{
	if (c->enc == SIFT_COLENC_CC)
	{
		SiftCc	   *cc = cc_open(c, false);

		sift_cc_decode_u32(cc, dst);
		sift_cc_close(cc);
	}
	else
	{
		int64	   *tmp = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

		codec_decode(c, tmp);
		for (uint32 i = 0; i < c->n; i++)
			dst[i] = (uint32) tmp[i];
		pfree(tmp);
	}
	ZERO_NULLS(c->nulls, c->n, dst);
}

/* the codes of a dictionary of ndict values, whose NULL code is ndict: the tables the executor indexes by them have ndict + 1 entries */
void
sift_cchunk_decode_codes(const SiftCChunk *c, uint32 ndict, uint32 *dst)
{
	sift_cchunk_decode_u32(c, dst);
	sift_ccodes_below(dst, c->n, ndict + 1);
}

void
sift_cchunk_decode_f8(const SiftCChunk *c, double *dst)
{
	if (c->enc == SIFT_COLENC_CC)
	{
		SiftCc	   *cc = cc_open(c, false);

		sift_cc_decode_f8(cc, dst);
		sift_cc_close(cc);
		ZERO_NULLS(c->nulls, c->n, dst);
	}
	else
	{
		sift_cchunk_decode(c, (int64 *) dst);
		sift_colk->f8_from_keys((int64 *) dst, c->n);
	}
}

void
sift_cchunk_decode_sel(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	codec_decode_sel(c, idx, m, dst);
	for (uint32 k = 0; c->nulls != NULL && k < m; k++)
		if (sift_col_bit_test(c->nulls, idx[k]))
			dst[k] = 0;
}

void
sift_cchunk_zone(const SiftCChunk *c, SiftColStore store, SiftCZone *z)
{
	int64	   *key = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

	if (c->enc == SIFT_COLENC_ISTR)
	{
		uint32	   *off;
		char	   *bytes;

		sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);	/* verified by its decoding */
		istr_zone(c->nulls, c->ovf, c->n, off, bytes, z);
		pfree(off);
		pfree(bytes);
		pfree(key);
		return;
	}
	if (c->enc == SIFT_COLENC_CC)
		sift_cc_close(cc_open(c, true));
	sift_cchunk_decode(c, key);	/* a STRS chunk is verified by its decoding */
	zone_of_keys(key, c->nulls, c->ovf, c->n, store, z);
	pfree(key);
}

/* ---------------------------------------------------------------------------
 * 7. Chunk tests
 *
 * A predicate becomes a test in the key domain. Floats need no rounding of the constant: F4 keys are the F8 keys of the promoted value, so
 * comparing a float4 column against a float8 constant in the key domain is exactly PostgreSQL's float48 comparison.
 * ------------------------------------------------------------------------- */

static bool
set_has(const SiftCTest *t, int64 k)
{
	return bsearch(&k, t->set, t->nset, sizeof(int64), sift_cmp_i64) != NULL;
}

static void
ctest_range(SiftCTest *t, int64 lo, int64 hi)
{
	t->kind = lo > hi ? SIFT_CT_NONE : SIFT_CT_RANGE;
	t->lo = lo;
	t->hi = hi;
}

/* Sorted distinct keys -> NONE/NONNULL (none), a range (one) or a set */
static void
ctest_keys(SiftCTest *t, int64 *keys, uint32 n, bool negate)
{
	uint32		m = 0;

	qsort(keys, n, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < n; i++)
		if (m == 0 || keys[m - 1] != keys[i])
			keys[m++] = keys[i];
	if (m == 0)
		t->kind = negate ? SIFT_CT_NONNULL : SIFT_CT_NONE;
	else if (m == 1)
	{
		ctest_range(t, keys[0], keys[0]);
		t->kind = negate ? SIFT_CT_NRANGE : SIFT_CT_RANGE;
	}
	else
	{
		t->kind = negate ? SIFT_CT_NSET : SIFT_CT_SET;
		t->set = keys;
		t->nset = m;
		t->lo = keys[0];
		t->hi = keys[m - 1];
	}
}

/* The keys a constant stands for: PostgreSQL compares -0 and +0 equal, so a zero spans the two keys of the float domain */
static void
const_span(const SiftColDef *def, const SiftColValue *v, int64 *lo, int64 *hi)
{
	*lo = *hi = def->store == SIFT_COLS_INT ? v->v.i : sift_col_f8_key(v->v.f);
	if (def->store != SIFT_COLS_INT && v->v.f == 0)
	{
		*lo = sift_col_f8_key(-0.0);
		*hi = sift_col_f8_key(0.0);
	}
}

static void
ctest_make_num(const SiftColDef *def, const SiftColPred *p, SiftCTest *t)
{
	int64		lo = 0,
				hi = 0;

	if (p->nvals > 0)
		const_span(def, &p->vals[0], &lo, &hi);
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
			ctest_range(t, lo, hi);
			t->kind = p->op == SIFT_COLOP_EQ ? SIFT_CT_RANGE : SIFT_CT_NRANGE;
			break;
		case SIFT_COLOP_LT:
			if (lo == PG_INT64_MIN)
				t->kind = SIFT_CT_NONE;
			else
				ctest_range(t, PG_INT64_MIN, lo - 1);
			break;
		case SIFT_COLOP_LE:
			ctest_range(t, PG_INT64_MIN, hi);
			break;
		case SIFT_COLOP_GT:
			if (hi == PG_INT64_MAX)
				t->kind = SIFT_CT_NONE;
			else
				ctest_range(t, hi + 1, PG_INT64_MAX);
			break;
		case SIFT_COLOP_GE:
			ctest_range(t, lo, PG_INT64_MAX);
			break;
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			{
				int64	   *keys = palloc(Max(p->nvals, 1) * 2 * sizeof(int64));
				int			m = 0;

				for (int i = 0; i < p->nvals; i++)
				{
					const_span(def, &p->vals[i], &lo, &hi);
					keys[m++] = lo;
					if (hi != lo)
						keys[m++] = hi;
				}
				ctest_keys(t, keys, m, p->op == SIFT_COLOP_NOTIN);
			}
			break;
		default:
			elog(ERROR, "sift_col: operator %d is not defined on a numeric column", p->op);
	}
}

/* Bytewise order of dictionary values (the C collation) */
static int
str_cmp(const char *a, uint32 alen, const char *b, uint32 blen)
{
	int			r = memcmp(a, b, Min(alen, blen));

	return r != 0 ? r : (alen > blen) - (alen < blen);
}

static bool
str_matches(SiftColOp op, const char *s, uint32 len, const SiftColValue *c)
{
	switch (op)
	{
		case SIFT_COLOP_LT:
			return str_cmp(s, len, c->v.s, c->len) < 0;
		case SIFT_COLOP_LE:
			return str_cmp(s, len, c->v.s, c->len) <= 0;
		case SIFT_COLOP_GT:
			return str_cmp(s, len, c->v.s, c->len) > 0;
		case SIFT_COLOP_GE:
			return str_cmp(s, len, c->v.s, c->len) >= 0;
		default:					/* PREFIX */
			return len >= c->len && memcmp(s, c->v.s, c->len) == 0;
	}
}

/* Turns the bit set of matching codes into a LUT test: none -> NONE, all -> NONNULL, else the LUT with its bounds and fold */
static void
ctest_lut_finish(SiftCTest *t, uint64 *lut, uint32 ncodes)
{
	uint32		count = 0;

	t->kind = SIFT_CT_LUT;
	t->lo = PG_INT64_MAX;
	t->hi = PG_INT64_MIN;
	for (uint32 c = 0; c < ncodes; c++)
		if (sift_col_bit_test(lut, c))
		{
			count++;
			t->lo = Min(t->lo, c);
			t->hi = Max(t->hi, c);
			t->fold[(c & 255) >> 6] |= UINT64CONST(1) << (c & 63);
		}
	t->fold_valid = true;
	t->lut = lut;
	t->lutbits = SIFT_COL_NWORDS(ncodes) * 64;
	if (count == 0)
		t->kind = SIFT_CT_NONE;
	else if (count == ncodes)
		t->kind = SIFT_CT_NONNULL;
}

static void
ctest_make_str(SiftCDict *dict, const SiftColPred *p, SiftCTest *t)
{
	uint64	   *lut;
	int64	   *codes = NULL;
	uint32		nfound = 0;

	if (dict == NULL)
		elog(ERROR, "sift_col: a test on a text column needs its dictionary");
	t->pred = p;
	lut = palloc0(SIFT_COL_NWORDS(dict->n) * sizeof(uint64));
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			codes = palloc(Max(p->nvals, 1) * sizeof(int64));
			for (int i = 0; i < (p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_NE ? 1 : p->nvals); i++)
			{
				int64		code = sift_cdict_find(dict, p->vals[i].v.s, p->vals[i].len);

				if (code >= 0)
					codes[nfound++] = code;
			}
			ctest_keys(t, codes, nfound, p->op == SIFT_COLOP_NE || p->op == SIFT_COLOP_NOTIN);
			if (t->kind == SIFT_CT_RANGE)
			{
				t->fold[(t->lo & 255) >> 6] = UINT64CONST(1) << (t->lo & 63);
				t->fold_valid = true;
			}
			if (t->kind == SIFT_CT_SET || t->kind == SIFT_CT_NSET)
			{
				/* several codes: the same answer as a LUT over the dictionary */
				SiftCTest	set = *t;

				memset(t, 0, sizeof(*t));
				for (uint32 c = 0; c < dict->n; c++)
					if (set_has(&set, c) != (set.kind == SIFT_CT_NSET))
						lut[c >> 6] |= UINT64CONST(1) << (c & 63);
				ctest_lut_finish(t, lut, dict->n);
			}
			return;
		default:
			for (uint32 c = 0; c < dict->n; c++)
				if (str_matches(p->op, SIFT_COL_DICT_STR(dict, c), cdict_len(dict, c), &p->vals[0]))
					lut[c >> 6] |= UINT64CONST(1) << (c & 63);
			ctest_lut_finish(t, lut, dict->n);
	}
}

void
sift_ctest_make_strs(const SiftColPred *p, SiftCTest *t)
{
	memset(t, 0, sizeof(*t));
	if (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL)
		t->kind = p->op == SIFT_COLOP_ISNULL ? SIFT_CT_ISNULL : SIFT_CT_NOTNULL;
	else if (p->nvals < 1 && p->op != SIFT_COLOP_IN && p->op != SIFT_COLOP_NOTIN)
		elog(ERROR, "sift_col: predicate without a constant");
	else if (p->nvals < 1)
		t->kind = p->op == SIFT_COLOP_IN ? SIFT_CT_NONE : SIFT_CT_NONNULL;
	else
		t->kind = SIFT_CT_STRS;
	t->pred = p;
}

void
sift_ctest_make(const SiftColDef *def, SiftCDict *dict, const SiftColPred *p, SiftCTest *t)
{
	memset(t, 0, sizeof(*t));
	if (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL)
		t->kind = p->op == SIFT_COLOP_ISNULL ? SIFT_CT_ISNULL : SIFT_CT_NOTNULL;
	else if (def->store == SIFT_COLS_VAR)
		sift_ctest_make_strs(p, t);
	else if (p->nvals < 1 && p->op != SIFT_COLOP_IN && p->op != SIFT_COLOP_NOTIN)
		elog(ERROR, "sift_col: predicate without a constant");
	else if (def->store == SIFT_COLS_STR)
		ctest_make_str(dict, p, t);
	else
		ctest_make_num(def, p, t);
}

/* Can a unit with inline strings, whose smallest and largest prefix keys are z->min and z->max, hold a match of p (STR predicates; conservative) */
static bool
zone_inline_matches(const SiftColPred *p, const SiftCZone *z)
{
	const SiftColValue *c = &p->vals[0];
	int64		k = p->nvals > 0 ? sift_col_str_prefix(c->v.s, c->len) : 0;

	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			return k >= z->min && k <= z->max;
		case SIFT_COLOP_IN:
			for (int i = 0; i < p->nvals; i++)
			{
				k = sift_col_str_prefix(p->vals[i].v.s, p->vals[i].len);
				if (k >= z->min && k <= z->max)
					return true;
			}
			return false;
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
			return z->min <= k;
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
			return z->max >= k;
		case SIFT_COLOP_PREFIX:
			{
				char		pad[8];

				memset(pad, 0xFF, sizeof(pad));
				memcpy(pad, c->v.s, Min(c->len, 8));
				return z->max >= k && z->min <= (c->len >= 8 ? k : sift_col_str_prefix(pad, 8));
			}
		default:					/* NE, NOTIN */
			return true;
	}
}

bool
sift_ctest_zone(const SiftCTest *t, const SiftCZone *z)
{
	if (z->flags & SIFT_COLZ_OVERFLOW)	/* the zone does not describe the heap: the recheck of the unit's overflow rows may find the constant */
		return true;
	if (t->kind == SIFT_CT_NONE && !(z->flags & SIFT_COLZ_INLINE))	/* NONE: no constant is in the dictionary, which an inline unit does not use */
		return false;
	if (t->kind == SIFT_CT_ISNULL)
		return z->nnull > 0;
	if (z->nnull == z->nvals)
		return false;
	if (z->flags & SIFT_COLZ_INLINE)
		return t->pred == NULL || zone_inline_matches(t->pred, z);
	switch (t->kind)
	{
		case SIFT_CT_RANGE:
		case SIFT_CT_LUT:
		case SIFT_CT_SET:
			if (z->max < t->lo || z->min > t->hi)
				return false;
			if (t->kind == SIFT_CT_SET)
			{
				const int64 *first = t->set;
				uint32		n = t->nset;

				while (n > 0)		/* the first member >= z->min */
				{
					uint32		h = n / 2;

					if (first[h] < z->min)
					{
						first += h + 1;
						n -= h + 1;
					}
					else
						n = h;
				}
				return first < t->set + t->nset && *first <= z->max;
			}
			if (t->fold_valid)
				return ((t->fold[0] & z->set[0]) | (t->fold[1] & z->set[1]) | (t->fold[2] & z->set[2]) | (t->fold[3] & z->set[3])) != 0;
			return true;
		case SIFT_CT_NRANGE:
			return z->min < t->lo || z->max > t->hi;
		case SIFT_CT_NSET:
			return !(z->min == z->max && set_has(t, z->min));
		default:					/* NONNULL, NOTNULL */
			return true;
	}
}

/* The test applied to one key: the specification of the vector paths below (and their fallback) */
static bool
key_matches(const SiftCTest *t, int64 k)
{
	switch (t->kind)
	{
		case SIFT_CT_RANGE:
			return k >= t->lo && k <= t->hi;
		case SIFT_CT_NRANGE:
			return k < t->lo || k > t->hi;
		case SIFT_CT_LUT:
			return k >= 0 && k < t->lutbits && sift_col_bit_test(t->lut, (uint32) k);
		case SIFT_CT_SET:
			return set_has(t, k);
		default:					/* NSET */
			return !set_has(t, k);
	}
}

static void
bits_fill(uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < words_of(n); w++)
		bits[w] = w == n / 64 ? (UINT64CONST(1) << (n % 64)) - 1 : ~UINT64CONST(0);
}

/* tmp (padded words of n bits) = rows whose key is in [lo, hi] (null slots hold base and are removed by the caller) */
static void
chunk_range(const SiftCChunk *c, int64 lo, int64 hi, uint64 *tmp)
{
	uint64		ulo,
				uhi,
				maxv = codec_maxval(c);
	uint32		nw = SIFT_COL_NWORDS(c->n);

	memset(tmp, 0, nw * sizeof(uint64));
	if (c->enc == SIFT_COLENC_CC)
	{
		SiftCc	   *cc = cc_open(c, false);

		sift_cc_filter(cc, lo, hi, tmp);
		sift_cc_close(cc);
		return;
	}
	if (lo > hi || hi < c->base)
		return;
	ulo = lo <= c->base ? 0 : (uint64) lo - (uint64) c->base;
	uhi = Min((uint64) hi - (uint64) c->base, maxv);
	if (ulo > uhi)
		return;
	if (c->enc == SIFT_COLENC_CONST)
		bits_fill(tmp, c->n);
	else if (c->enc == SIFT_COLENC_FOR)
		sift_colk->range[codec_slot(c->bits)](c->payload, c->n, ulo, uhi - ulo, tmp);
	else
	{
		uint32		blk[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

		for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
		{
			uint32		m = Min(CODEC_BLOCK, c->n - s);

			sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, blk);
			sift_colk->range[SIFT_COLW_32](blk, m, ulo, uhi - ulo, tmp + s / 64);
		}
	}
}

/* tmp = rows matching, evaluated key by key (sets of many members, and what has no vector path) */
static void
chunk_scalar(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	int64	   *key = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

	memset(tmp, 0, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
	sift_cchunk_decode(c, key);
	for (uint32 i = 0; i < c->n; i++)
		if (key_matches(t, key[i]))
			tmp[i >> 6] |= UINT64CONST(1) << (i & 63);
	pfree(key);
}

/* dst (nbits) = src (srcbits) shifted down by `by` bits, zero past the end of src */
static void
lut_shift(const uint64 *src, uint64 srcbits, uint64 by, uint64 *dst, uint32 nbits)
{
	uint64		srcwords = (srcbits + 63) / 64;

	for (uint32 w = 0; w < words_of(nbits); w++)
	{
		uint64		i = (by >> 6) + w,
					lo = i < srcwords ? src[i] : 0,
					hi = i + 1 < srcwords ? src[i + 1] : 0;

		dst[w] = (by & 63) ? (lo >> (by & 63)) | (hi << (64 - (by & 63))) : lo;
	}
}

void
sift_ccodes_below(const uint32 *codes, uint32 n, uint32 limit)
{
	if (sift_cck->max32(codes, n) >= limit)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: a code is past the table it indexes (%u entries)", limit)));
}

/* chunk_lut of a CC chunk: the codec hands the codes as offsets from its minimum */
static void
cc_lut(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	SiftCc	   *cc = cc_open(c, false);
	int64		min = sift_cc_min(cc);
	uint32	   *off;
	uint64	   *shifted;
	uint32		sbits;

	if (min >= 0 && (uint64) min < t->lutbits)
	{
		off = palloc(SIFT_CC_CAP32(c->n) * sizeof(uint32));
		sbits = t->lutbits - (uint32) min;
		shifted = palloc0(SIFT_COL_NWORDS(sbits) * sizeof(uint64));
		lut_shift(t->lut, t->lutbits, min, shifted, sbits);
		if (sift_cc_decode_off32(cc, off))
		{
			if (sift_cc_off32_bound(cc) >= sbits)
				sift_ccodes_below(off, c->n, sbits);
			sift_colk->lut[2](off, c->n, shifted, tmp);
		}
		else
			chunk_scalar(t, c, tmp);
		pfree(shifted);
		pfree(off);
	}
	sift_cc_close(cc);
}

/* tmp = rows whose code is in the LUT; the chunk holds code - base, so the LUT is shifted by base */
static void
chunk_lut(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	uint32		nw = SIFT_COL_NWORDS(c->n),
				sbits;
	uint64	   *shifted;

	memset(tmp, 0, nw * sizeof(uint64));
	if (c->enc == SIFT_COLENC_CC)
	{
		cc_lut(t, c, tmp);
		return;
	}
	if (c->base < 0 || (uint64) c->base >= t->lutbits)
		return;
	if (c->enc == SIFT_COLENC_CONST)
	{
		if (sift_col_bit_test(t->lut, (uint32) c->base))
			bits_fill(tmp, c->n);
		return;
	}
	if (c->enc == SIFT_COLENC_FOR && c->bits == 64)		/* codes are 32 bits: only a foreign writer makes this */
	{
		chunk_scalar(t, c, tmp);
		return;
	}
	/* a code of at most 16 bits has a bit of its own in the table, whatever the chunk holds; wider codes are checked against the table */
	sbits = c->bits <= 16 ? 1U << c->bits : t->lutbits - (uint32) c->base;
	shifted = palloc0(SIFT_COL_NWORDS(sbits) * sizeof(uint64));
	lut_shift(t->lut, t->lutbits, c->base, shifted, sbits);
	if (c->enc == SIFT_COLENC_FOR && c->bits <= 16)
		sift_colk->lut[codec_slot(c->bits)](c->payload, c->n, shifted, tmp);
	else if (c->enc == SIFT_COLENC_FOR)
	{
		sift_ccodes_below((const uint32 *) c->payload, c->n, sbits);
		sift_colk->lut[2](c->payload, c->n, shifted, tmp);
	}
	else if (c->enc == SIFT_COLENC_PACK)
	{
		uint32		blk[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

		for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
		{
			uint32		m = Min(CODEC_BLOCK, c->n - s);

			sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, blk);
			if (c->bits > 16)
				sift_ccodes_below(blk, m, sbits);
			sift_colk->lut[2](blk, m, shifted, tmp + s / 64);
		}
	}
	pfree(shifted);
}

/* Whole-string comparison of a STRS or ISTR chunk's row with the predicate's constants (EQ and IN: equal to one of them) */
static bool
strs_match(SiftColOp op, const SiftColPred *p, const char *s, uint32 len)
{
	switch (op)
	{
		case SIFT_COLOP_EQ:
			return len == p->vals[0].len && memcmp(s, p->vals[0].v.s, len) == 0;
		case SIFT_COLOP_IN:
			for (int i = 0; i < p->nvals; i++)
				if (len == p->vals[i].len && memcmp(s, p->vals[i].v.s, len) == 0)
					return true;
			return false;
		default:
			return str_matches(op, s, len, &p->vals[0]);
	}
}

/* IN lists longer than this are verified on every row: the prefix filter would run once per constant */
#define STRS_PREFIX_MAXIN	16

/*
 * The rows of an inline chunk that can satisfy op (EQ, IN, a range or PREFIX) according to the prefix blob alone, on the encoded form (the
 * codec's filter: bit-packed keys compare in registers, a constant blob never expands). The string list is not touched. NULL: the operator
 * gives the prefix no use, every row is a candidate.
 */
static uint64 *
istr_candidates(const SiftCChunk *c, SiftColOp op, const SiftColPred *p)
{
	const uint8 *pre,
			   *strs;
	uint32		prelen,
				strslen,
				nw = SIFT_COL_NWORDS(c->n);
	SiftCc	   *cc;
	uint64	   *cand,
			   *one;
	int64		k;

	if ((op == SIFT_COLOP_IN && p->nvals > STRS_PREFIX_MAXIN) || !istr_split(c, &pre, &prelen, &strs, &strslen))
		return NULL;
	cc = sift_cc_open(CurrentMemoryContext, pre, prelen, false);
	if (sift_cc_nrows(cc) != c->n)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: the prefix blob has %u values, the chunk %u", sift_cc_nrows(cc), c->n)));
	cand = palloc0(nw * sizeof(uint64));
	k = p->nvals > 0 ? sift_col_str_prefix(p->vals[0].v.s, p->vals[0].len) : 0;
	switch (op)
	{
		case SIFT_COLOP_EQ:
			sift_cc_filter(cc, k, k, cand);
			break;
		case SIFT_COLOP_IN:
			one = palloc(nw * sizeof(uint64));
			for (int i = 0; i < p->nvals; i++)
			{
				k = sift_col_str_prefix(p->vals[i].v.s, p->vals[i].len);
				sift_cc_filter(cc, k, k, one);
				sift_colk->bits_or(cand, one, nw);
			}
			pfree(one);
			break;
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
			sift_cc_filter(cc, PG_INT64_MIN, k, cand);
			break;
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
			sift_cc_filter(cc, k, PG_INT64_MAX, cand);
			break;
		default:					/* PREFIX: the strings with it have the prefix keys from the constant padded with 0 up to padded with 255 */
			{
				char		pad[8];

				memset(pad, 0xFF, sizeof(pad));
				memcpy(pad, p->vals[0].v.s, Min(p->vals[0].len, 8));
				sift_cc_filter(cc, k, p->vals[0].len >= 8 ? k : sift_col_str_prefix(pad, 8), cand);
			}
	}
	sift_cc_close(cc);
	return cand;
}

/* tmp = rows of a STRS or ISTR chunk whose string satisfies the predicate (NE and NOTIN: its opposite, EQ and IN), NULL slots may be set */
static void
chunk_strs(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	const SiftColPred *p = t->pred;
	SiftColOp	op = p->op == SIFT_COLOP_NE ? SIFT_COLOP_EQ : p->op == SIFT_COLOP_NOTIN ? SIFT_COLOP_IN : p->op;
	uint64	   *cand = c->enc == SIFT_COLENC_ISTR ? istr_candidates(c, op, p) : NULL;
	uint32		nw = SIFT_COL_NWORDS(c->n),
				cw = words_of(c->n),
			   *off;
	char	   *bytes;
	bool		any = cand == NULL;

	memset(tmp, 0, nw * sizeof(uint64));
	for (uint32 w = 0; cand != NULL && w < cw; w++)
		any |= cand[w] != 0;
	if (!any)
		return;
	sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	for (uint32 w = 0; w < cw; w++)
	{
		uint64		m = cand ? cand[w] : ~UINT64CONST(0);

		if (w == c->n / 64)
			m &= (UINT64CONST(1) << (c->n % 64)) - 1;
		for (; m; m &= m - 1)
		{
			uint32		i = w * 64 + pg_rightmost_one_pos64(m);

			if (!sift_col_bit_test(c->nulls, i) && strs_match(op, p, bytes + off[i], off[i + 1] - off[i]))
				tmp[w] |= UINT64CONST(1) << (i % 64);
		}
	}
	pfree(off);
	pfree(bytes);
	if (cand)
		pfree(cand);
}

void
sift_cchunk_strs_length(const SiftCChunk *c, uint32 lo, uint32 hi, uint64 *bits)
{
	int64	   *len = palloc((Size) (c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
	uint32	   *u32 = palloc((Size) c->n * sizeof(uint32) + SIFT_COL_PAD);

	memset(bits, 0, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
	if (lo <= hi)
	{
		sift_cchunk_decode(c, len);
		for (uint32 i = 0; i < c->n; i++)
			u32[i] = (uint32) len[i];
		sift_colk->range[SIFT_COLW_32](u32, c->n, lo, hi - lo, bits);
		if (c->nulls)
			sift_colk->bits_andnot(bits, c->nulls, words_of(c->n));
	}
	pfree(len);
	pfree(u32);
}

/* Sets of up to this many members are OR-ed range tests, larger ones are searched */
#define SET_RANGES_MAX	8

static void
chunk_set(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	uint64	   *one = palloc(SIFT_COL_NWORDS(c->n) * sizeof(uint64));

	memset(tmp, 0, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
	for (uint32 i = 0; i < t->nset; i++)
	{
		chunk_range(c, t->set[i], t->set[i], one);
		sift_colk->bits_or(tmp, one, SIFT_COL_NWORDS(c->n));
	}
	pfree(one);
}

void
sift_ctest_run(const SiftCTest *t, const SiftCChunk *c, uint64 *bits)
{
	uint32		nw = SIFT_COL_NWORDS(c->n),
				cw = words_of(c->n);
	uint64	   *tmp;
	bool		negate = t->kind == SIFT_CT_NRANGE || t->kind == SIFT_CT_NSET ||
	(t->kind == SIFT_CT_STRS && (t->pred->op == SIFT_COLOP_NE || t->pred->op == SIFT_COLOP_NOTIN));

	switch (t->kind)
	{
		case SIFT_CT_NONE:
			memset(bits, 0, nw * sizeof(uint64));
			return;
		case SIFT_CT_ISNULL:
			if (c->nulls)
				sift_colk->bits_and(bits, c->nulls, cw);
			memset(bits + (c->nulls ? cw : 0), 0, (nw - (c->nulls ? cw : 0)) * sizeof(uint64));
			return;
		case SIFT_CT_NONNULL:
		case SIFT_CT_NOTNULL:
			if (c->nulls)
				sift_colk->bits_andnot(bits, c->nulls, cw);
			return;
		default:
			break;
	}
	tmp = palloc(nw * sizeof(uint64));
	if (t->kind == SIFT_CT_RANGE || t->kind == SIFT_CT_NRANGE)
		chunk_range(c, t->lo, t->hi, tmp);
	else if (t->kind == SIFT_CT_LUT)
		chunk_lut(t, c, tmp);
	else if (t->kind == SIFT_CT_STRS)
		chunk_strs(t, c, tmp);
	else if (t->nset > SET_RANGES_MAX)
	{
		chunk_scalar(t, c, tmp);	/* key_matches includes the negation of NSET */
		negate = false;
	}
	else
		chunk_set(t, c, tmp);
	if (negate)
		sift_colk->bits_andnot(bits, tmp, nw);
	else
		sift_colk->bits_and(bits, tmp, nw);
	if (c->nulls)
		sift_colk->bits_andnot(bits, c->nulls, cw);
	pfree(tmp);
}

/* ---------------------------------------------------------------------------
 * 8. Units
 * ------------------------------------------------------------------------- */

#define UNIT_HEAD		(16 + SIFT_PAGEMASK_WORDS * 8)

/* Bytes of the offset bitmap of page p (0 for a dense page) */
static inline uint32
page_sparse_len(const SiftCUnitView *v, uint32 p)
{
	return v->maxoff[p] == v->rowstart[p + 1] - v->rowstart[p] ? 0 : (v->maxoff[p] + 7) / 8;
}

static uint32
page_sparse_off(const SiftCUnitView *v, uint32 p)
{
	uint32		off = 0;

	for (uint32 q = 0; q < p; q++)
		off += page_sparse_len(v, q);
	return off;
}

/* Writes the offsets (1-based) of the rows of page p, which has rowstart[p + 1] - rowstart[p] of them; sparse: its bitmap */
static void
page_offsets(const SiftCUnitView *v, uint32 p, const uint8 *bitmap, uint16 *offs)
{
	uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
				n = 0;

	for (uint32 o = 1; o <= v->maxoff[p]; o++)
		if (bitmap == NULL || (bitmap[(o - 1) / 8] >> ((o - 1) % 8)) & 1)
			offs[n++] = (uint16) o;
	Assert(n == cnt);
	(void) cnt;
}

/* The row directory of the sorted keys of superblock sb, appended to out */
static void
unit_put_directory(StringInfo out, const SiftKey *keys, uint32 nrows, uint32 sb)
{
	uint32		head[4] = {COLU_MAGIC, nrows, 0, 0};
	uint64		pagemask[SIFT_PAGEMASK_WORDS] = {0};
	uint32	   *rowstart = palloc((SIFT_SB_PAGES + 1) * sizeof(uint32));
	uint16	   *maxoff = palloc(SIFT_SB_PAGES * sizeof(uint16));
	StringInfoData sparse;
	uint32		np = 0;

	initStringInfo(&sparse);
	for (uint32 r = 0, j; r < nrows; r = j)
	{
		uint32		page = sift_key_block(keys[r]) - sb * SIFT_SB_PAGES;
		uint32		mo;

		for (j = r; j < nrows && sift_key_block(keys[j]) == sift_key_block(keys[r]); j++)
			;
		mo = (uint32) (keys[j - 1] & 0xFFFF);
		pagemask[page / 64] |= UINT64CONST(1) << (page % 64);
		rowstart[np] = r;
		maxoff[np++] = (uint16) mo;
		if (mo != j - r)
		{
			Size		at = sparse.len;

			put_zero(&sparse, (mo + 7) / 8);
			for (uint32 k = r; k < j; k++)
				sparse.data[at + ((keys[k] & 0xFFFF) - 1) / 8] |= 1 << (((keys[k] & 0xFFFF) - 1) % 8);
		}
	}
	rowstart[np] = nrows;
	head[2] = np;
	put_val(out, head);
	put_bytes(out, pagemask, sizeof(pagemask));
	put_bytes(out, rowstart, (np + 1) * sizeof(uint32));
	put_bytes(out, maxoff, np * sizeof(uint16));
	put_bytes(out, sparse.data, sparse.len);
	put_pad(out, 4);
	pfree(rowstart);
	pfree(maxoff);
	pfree(sparse.data);
}

void
sift_cunit_encode(const SiftColBatch *b, uint32 *const *dictmap, const bool *inl, const SiftCcPolicy *pol, bool tags, StringInfo out,
				  Size *start, SiftCZone *zones)
{
	const SiftColSchema *s = b->schema;
	Size		tab;

	put_pad(out, 8);
	*start = out->len;
	unit_put_directory(out, b->keys, b->nrows, b->sb);
	tab = out->len;
	put_zero(out, 2 * s->ncols * sizeof(uint32));
	for (int col = 0; col < s->ncols; col++)
	{
		const SiftCVals *v = &b->col[col];
		const int64 *key = v->key;
		int64	   *mapped = NULL;
		SiftCZone	zone;
		uint32		off,
					len;

		bool		inline_str = inl && inl[col] && v->n > 0;	/* a leaf without values (a group without elements) has the empty chunk */

		if (dictmap != NULL && dictmap[col] != NULL && !inline_str)
		{
			mapped = palloc(Max(v->n, 1) * sizeof(int64));
			for (uint32 i = 0; i < v->n; i++)
				mapped[i] = sift_col_bit_test(v->nulls, i) ? 0 : dictmap[col][v->key[i]];
			key = mapped;
		}
		put_pad(out, 8);
		off = (uint32) (out->len - *start);
		if (inline_str)
			chunk_encode_istr(out, v, b->dict[col], pol ? &pol[col] : NULL, tags, &zone);
		else
			chunk_encode(out, s->cols[col].store, v, key, pol ? &pol[col] : NULL, tags, &zone);
		len = (uint32) (out->len - *start) - off;
		memcpy(out->data + tab + col * sizeof(uint32), &off, sizeof(off));
		memcpy(out->data + tab + (s->ncols + col) * sizeof(uint32), &len, sizeof(len));
		if (zones)
			zones[col] = zone;
		if (mapped)
			pfree(mapped);
	}
	/* the kernels read SIFT_COL_PAD bytes past the last chunk: a record parsed in place has them */
	enlargeStringInfo(out, SIFT_COL_PAD);
	memset(out->data + out->len, 0, SIFT_COL_PAD);
}

/* The bits of the sparse bitmap of a page: exactly cnt of them, the last at maxoff, none past it */
static bool
sparse_valid(const uint8 *bm, uint32 mo, uint32 cnt)
{
	uint32		pop = 0;

	for (uint32 i = 0; i < (mo + 7) / 8; i++)
		pop += sift_pc64(bm[i]);
	return pop == cnt && ((bm[(mo - 1) / 8] >> ((mo - 1) % 8)) & 1) && (mo % 8 == 0 || (bm[(mo - 1) / 8] >> (mo % 8)) == 0);
}

/* The strings chunks belong to the VAR (STRS) and the STR (ISTR) leaves, and to no other */
static bool
chunk_fits_leaf(const SiftColDef *def, const SiftCChunk *c)
{
	return (c->enc == SIFT_COLENC_STRS) == (def->store == SIFT_COLS_VAR) && (c->enc != SIFT_COLENC_ISTR || leaf_is_str(def));
}

/*
 * avail: the bytes of the record present, len: its full length. chunks: also validate every
 * chunk (needs all of them present).
 */
static bool
unit_parse(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v, bool chunks)
{
	uint32		head[4];
	uint64		pos = UNIT_HEAD,
				dirend,
				pop = 0;
	uint32		np;

	if (avail < UNIT_HEAD || avail > len || ((uintptr_t) rec & 7) != 0)
		return false;
	memset(v, 0, sizeof(*v));
	memcpy(head, rec, sizeof(head));
	memcpy(v->pagemask, rec + sizeof(head), sizeof(v->pagemask));
	np = head[2];
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		pop += sift_pc64(v->pagemask[w]);
	if (head[0] != COLU_MAGIC || head[1] > SIFT_COL_UNIT_MAX_ROWS || np > SIFT_SB_PAGES || pop != np || (np == 0) != (head[1] == 0))
		return false;
	v->rec = rec;
	v->len = len;
	v->schema = schema;
	v->nrows = head[1];
	v->npages = np;
	for (uint32 w = 0, p = 0; w < SIFT_PAGEMASK_WORDS; w++)
		for (uint64 m = v->pagemask[w]; m; m &= m - 1)
			v->page[p++] = (uint8) (w * 64 + pg_rightmost_one_pos64(m));
	if (pos + (uint64) (np + 1) * 4 + (uint64) np * 2 > avail)
		return false;
	v->rowstart = (const uint32 *) (rec + pos);
	pos += (uint64) (np + 1) * 4;
	v->maxoff = (const uint16 *) (rec + pos);
	pos += (uint64) np * 2;
	v->sparse = rec + pos;
	if (v->rowstart[0] != 0 || v->rowstart[np] != v->nrows)
		return false;
	for (uint32 p = 0; p < np; p++)
	{
		uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
					mo = v->maxoff[p],
					sl;

		if (v->rowstart[p + 1] <= v->rowstart[p] || mo < cnt || mo > MaxHeapTuplesPerPage)
			return false;
		sl = page_sparse_len(v, p);
		if (pos + sl > avail || (sl > 0 && !sparse_valid(rec + pos, mo, cnt)))
			return false;
		pos += sl;
	}
	pos = (pos + 3) & ~UINT64CONST(3);
	if (pos + 8 * (uint64) schema->ncols > avail)
		return false;
	v->chunk_off = (const uint32 *) (rec + pos);
	v->chunk_len = v->chunk_off + schema->ncols;
	dirend = (pos + 8 * (uint64) schema->ncols + 7) & ~UINT64CONST(7);
	for (int col = 0; col < schema->ncols; col++)
	{
		SiftCChunk	c;

		if (v->chunk_len[col] == 0)
			continue;
		if (v->chunk_off[col] % 8 != 0 || v->chunk_off[col] < dirend || (uint64) v->chunk_off[col] + v->chunk_len[col] > len ||
			(chunks && (!sift_cchunk_parse(rec + v->chunk_off[col], v->chunk_len[col], &c) || (schema->cols[col].parent < 0 && c.n != v->nrows) ||
						!chunk_fits_leaf(&schema->cols[col], &c))))
			return false;
	}
	return true;
}

bool
sift_cunit_parse(const uint8 *rec, uint32 len, const SiftColSchema *schema, SiftCUnitView *v)
{
	return unit_parse(rec, len, len, schema, v, true);
}

bool
sift_cunit_parse_head(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v)
{
	return unit_parse(rec, avail, len, schema, v, false);
}

bool
sift_cunit_chunk(const SiftCUnitView *v, int col, SiftCChunk *c)
{
	if (v->chunk_len[col] == 0)
		return false;
	if (!sift_cchunk_parse(v->rec + v->chunk_off[col], v->chunk_len[col], c))
		elog(ERROR, "sift_col: chunk of a parsed unit is inconsistent");
	return true;
}

/* The p-th set bit of the page mask: the page of the superblock (the table is built when the unit is parsed) */
static uint32
unit_page(const SiftCUnitView *v, uint32 p)
{
	if (p >= v->npages)
		elog(ERROR, "sift_col: page index %u is outside the unit", p);
	return v->page[p];
}

BlockNumber
sift_cunit_page_block(const SiftCUnitView *v, uint32 sb, uint32 p)
{
	return sb * SIFT_SB_PAGES + unit_page(v, p);
}

uint32
sift_cunit_page_sparse_len(const SiftCUnitView *v, uint32 p)
{
	return page_sparse_len(v, p);
}

void
sift_cunit_page_offmask_at(const SiftCUnitView *v, uint32 p, uint32 soff, uint64 *offmask)
{
	uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
				sl = page_sparse_len(v, p);

	memset(offmask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
	if (sl > 0)
		memcpy(offmask, v->sparse + soff, sl);
	else
		for (uint32 w = 0; w * 64 < cnt; w++)
			offmask[w] = cnt - w * 64 >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (cnt - w * 64)) - 1;
}

void
sift_cunit_page_offmask(const SiftCUnitView *v, uint32 p, uint64 *offmask)
{
	sift_cunit_page_offmask_at(v, p, page_sparse_off(v, p), offmask);
}

uint32
sift_cunit_row_page(const SiftCUnitView *v, uint32 row)
{
	uint32		lo = 0,
				hi = v->npages;		/* rowstart[lo] <= row < rowstart[hi] */

	while (hi - lo > 1)
	{
		uint32		mid = (lo + hi) / 2;

		if (v->rowstart[mid] <= row)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

void
sift_cunit_keys(const SiftCUnitView *v, uint32 sb, SiftKey *keys)
{
	const uint8 *bm = v->sparse;
	uint16		offs[MaxHeapTuplesPerPage];

	for (uint32 p = 0; p < v->npages; p++)
	{
		uint32		sl = page_sparse_len(v, p);
		SiftKey		blk = sift_cunit_page_block(v, sb, p);

		page_offsets(v, p, sl > 0 ? bm : NULL, offs);
		for (uint32 r = v->rowstart[p]; r < v->rowstart[p + 1]; r++)
			keys[r] = (blk << 16) | offs[r - v->rowstart[p]];
		bm += sl;
	}
}

/* The keys of the rows ords (ascending) only: the pages that hold them are decoded, the others skipped */
void
sift_cunit_row_keys(const SiftCUnitView *v, uint32 sb, const uint32 *ords, uint32 n, SiftKey *out)
{
	const uint8 *bm = v->sparse;
	uint16		offs[MaxHeapTuplesPerPage];
	uint32		k = 0;

	for (uint32 p = 0; p < v->npages && k < n; p++)
	{
		uint32		sl = page_sparse_len(v, p);

		if (ords[k] < v->rowstart[p + 1])
		{
			SiftKey		blk = sift_cunit_page_block(v, sb, p);

			page_offsets(v, p, sl > 0 ? bm : NULL, offs);
			for (; k < n && ords[k] < v->rowstart[p + 1]; k++)
				out[k] = (blk << 16) | offs[ords[k] - v->rowstart[p]];
		}
		bm += sl;
	}
}

/* ---- decoding a unit into a batch ---- */

typedef struct UnitLeaf
{
	bool		present;
	SiftCChunk	c;
	int64	   *key;			/* decoded, NULL slots 0 */
	uint32	   *remap;			/* STR: code of the unit's dictionary -> code in the destination, PG_UINT32_MAX = not yet */
	uint32	   *eoff;			/* GROUP: element offset of each row, nrows + 1 */
	uint32	   *voff;			/* VAR and inline STR: start of each string, n + 1 */
	char	   *vb;
	uint8	   *tag;			/* the chunk's tags per slot (overflow types, kinds); NULL without SIFT_COLC_TAGS and SIFT_COLC_KINDS (minor 3: not known) */
	bool		inl;			/* STR stored inline: the strings are interned into the destination's dictionary */
} UnitLeaf;

/* Appends slot `slot` of leaf col of the unit to dst's leaf */
static void
unit_emit(SiftColBatch *dst, SiftCDict *const *dicts, UnitLeaf *lf, int col, uint32 slot)
{
	bool		isnull = !lf->present || sift_col_bit_test(lf->c.nulls, slot);
	bool		ovf = lf->present && sift_col_bit_test(lf->c.ovf, slot);
	uint8		tag = lf->present && lf->tag ? lf->tag[slot] : SIFT_COLTAG_UNKNOWN;
	int64		key = isnull ? 0 : lf->key[slot];

	if (lf->inl)
	{
		vals_put(dst->mcxt, &dst->col[col], isnull ? 0 : sift_cdict_intern(dst->dict[col], lf->vb + lf->voff[slot], (uint32) key), isnull, ovf, tag);
		return;
	}
	if (lf->voff != NULL)
	{
		vals_put_var(dst->mcxt, &dst->col[col], lf->vb + lf->voff[slot], (uint32) key, isnull, ovf, tag);
		return;
	}
	if (!isnull && lf->remap != NULL)
	{
		uint32	   *to;

		if (key < 0 || key >= dicts[col]->n)
			elog(ERROR, "sift_col: dictionary code %lld is outside the dictionary of %u values", (long long) key, dicts[col]->n);
		to = &lf->remap[key];
		if (*to == PG_UINT32_MAX)
			*to = sift_cdict_intern(dst->dict[col], dicts[col]->bytes + dicts[col]->off[key], cdict_len(dicts[col], (uint32) key));
		key = *to;
	}
	vals_put(dst->mcxt, &dst->col[col], key, isnull, ovf, tag);
}

void
sift_cunit_decode(const SiftCUnitView *v, uint32 sb, SiftCDict *const *dicts, const uint64 *live, SiftColBatch *dst)
{
	const SiftColSchema *s = v->schema;
	SiftKey    *keys = palloc(Max(v->nrows, 1) * sizeof(SiftKey));
	UnitLeaf   *lf = palloc0(s->ncols * sizeof(UnitLeaf));

	if (dst->schema->ncols != s->ncols || dst->sb != sb)
		elog(ERROR, "sift_col: a unit is decoded into a batch of its own schema and superblock");
	sift_cunit_keys(v, sb, keys);
	for (int col = 0; col < s->ncols; col++)
	{
		UnitLeaf   *l = &lf[col];

		l->present = sift_cunit_chunk(v, col, &l->c);
		if (!l->present)
			continue;
		l->key = palloc((l->c.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
		sift_cchunk_decode(&l->c, l->key);
		l->inl = l->c.enc == SIFT_COLENC_ISTR;
		l->tag = palloc(Max(l->c.n, 1));
		if (!sift_cchunk_tags(&l->c, l->tag))
			l->tag = NULL;
		if (s->cols[col].store == SIFT_COLS_VAR || l->inl)
			sift_cchunk_strs(&l->c, CurrentMemoryContext, &l->voff, &l->vb);
		if (leaf_is_str(&s->cols[col]) && !l->inl)
		{
			l->remap = palloc(Max(dicts[col]->n, 1) * sizeof(uint32));
			memset(l->remap, 0xFF, Max(dicts[col]->n, 1) * sizeof(uint32));
		}
		if (s->cols[col].kind == SIFT_COLK_GROUP)
		{
			l->eoff = palloc((v->nrows + 1) * sizeof(uint32));
			l->eoff[0] = 0;
			for (uint32 r = 0; r < v->nrows; r++)
				l->eoff[r + 1] = l->eoff[r] + (uint32) l->key[r];
		}
	}
	for (int col = 0; col < s->ncols; col++)
		if (s->cols[col].parent >= 0 && lf[col].present && lf[col].c.n != (lf[s->cols[col].parent].eoff ? lf[s->cols[col].parent].eoff[v->nrows] : 0))
			elog(ERROR, "sift_col: group leaf %d has %u values for %u elements", col, lf[col].c.n,
				 lf[s->cols[col].parent].eoff ? lf[s->cols[col].parent].eoff[v->nrows] : 0);

	for (uint32 r = 0; r < v->nrows; r++)
	{
		if (live != NULL && !sift_col_bit_test(live, r))
			continue;
		batch_row_begin(dst, keys[r]);
		for (int col = 0; col < s->ncols; col++)
		{
			if (s->cols[col].parent >= 0)
				continue;
			unit_emit(dst, dicts, &lf[col], col, r);
			if (s->cols[col].kind != SIFT_COLK_GROUP)
				continue;
			batch_group_end(dst, col, lf[col].present ? lf[col].eoff[r + 1] - lf[col].eoff[r] : 0);
			for (int ch = col + 1; ch < s->ncols; ch++)
				if (s->cols[ch].parent == col && lf[col].present)
					for (uint32 j = lf[col].eoff[r]; j < lf[col].eoff[r + 1]; j++)
						unit_emit(dst, dicts, &lf[ch], ch, j);
		}
		dst->nrows++;
	}
	for (int col = 0; col < s->ncols; col++)
	{
		if (lf[col].key)
			pfree(lf[col].key);
		if (lf[col].remap)
			pfree(lf[col].remap);
		if (lf[col].eoff)
			pfree(lf[col].eoff);
		if (lf[col].tag)
			pfree(lf[col].tag);
		if (lf[col].voff)
		{
			pfree(lf[col].voff);
			pfree(lf[col].vb);
		}
	}
	pfree(lf);
	pfree(keys);
}
