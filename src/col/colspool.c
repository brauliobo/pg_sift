/*
 * col/colspool.c - rows spooled to a temporary file as columnar chunks (colspool.h)
 * The rows of a spool are added to a keyed batch (arrival order: the addresses ascend, so the row directory of the unit is the cheap one); when it holds chunk_rows rows, or its memory
 * passes the share of the spool, it is encoded as a unit record by the shipped codecs and appended to the file as a length and the record. The strings of a leaf go through the dictionary of
 * the spool (interned once, written after the last chunk), unless it would pass its cap or hold a string too long for a dictionary: the leaf is then stored inline in the chunks that need it.
 * Reading takes the chunks in order and decodes each straight onto a batch: no row is made on either side.
 */
#include "postgres.h"
#include "storage/buffile.h"
#include "col/api/format.h"
#include "col/codec.h"
#include "col/compress.h"
#include "col/colspool.h"
#include "col/enc.h"
#include "col/kern.h"

#define CHUNK_ROWS_MIN	512				/* below it the chunks lose what the columns gain from many rows: the rows go to the row file */
#define CHUNK_ROWS_MAX	16384
#define PROBE_ROWS		4096			/* the rows whose memory the plan measures */
#define PICK_ROWS		2048			/* the rows whose chunk the plan encodes with each layer, to pick one */
#define PRICE_ROWS		4096			/* the rows whose chunks, with the layer picked, price the files */
#define DICT_CAP_MIN	((Size) 4 << 10)
#define DICT_CAP_MAX	((Size) 256 << 10)
#define DICT_MEM_FACTOR	3				/* what a dictionary holds in memory for each byte of its strings: the strings, their offsets and the table */
#define LAYER_GAIN_PCT	20				/* a layer is used when the sample's chunks are at least this much smaller with it than without */

#ifdef SIFT_TESTING
int			sift_test_col_spool_layer = 0;
int			sift_test_col_spool_chunk_rows = 0;
int			sift_test_col_spool_share_kb = 0;
int			sift_test_col_spool_unbounded = 0;
int			sift_test_col_spool_used_rows = 0;
int			sift_test_col_spool_est_bytes = 0;
#endif

typedef struct SpoolLayer
{
	const char *name;
	const char *spec;
	int			alg;
} SpoolLayer;

enum {LAYER_CASCADE, LAYER_LZ4, LAYER_ZSTD, LAYER_NONE};
static const SpoolLayer layers[] = {{"cascade", "auto", SIFT_CC_ALG_NONE}, {"lz4", "lz4", SIFT_CC_ALG_LZ4}, {"zstd", "zstd:1", SIFT_CC_ALG_ZSTD}, {"none", "none", SIFT_CC_ALG_NONE}};

struct SiftChunkPlan
{
	const SiftColSchema *sc;
	SiftCcPolicy *pol;			/* [ncols] */
	bool	   *str;			/* [ncols]: the leaves with a dictionary */
	int			nstr;
	uint32		chunk_rows;
	Size		mem_cap;		/* a spool whose memory reaches this writes its chunk */
	Size		dict_cap;		/* the bytes of strings of the dictionary of a leaf of a spool */
	int			layer;
	double		row_bytes;
	StringInfoData scratch;		/* a chunk going out or coming in: the spools of a build are written and read one at a time */
};

struct SiftChunkSpool
{
	SiftChunkPlan *plan;
	MemoryContext cxt;			/* the batch, the dictionaries; reading: the dictionaries parsed */
	MemoryContext tmp;			/* reading: what a chunk takes */
	BufFile    *file;			/* NULL: the bytes are counted and nothing is written */
	uint64		fbytes;
	uint64		chunks_end;		/* the bytes of the chunks: the dictionaries follow them */
	uint64		rread;
	int			dict_fileno;
	off_t		dict_off;
	bool		reading;
	SiftColBatch *b;			/* writing, from the first row */
	SiftCDict **rd;				/* [ncols]: the dictionary of the spool for each leaf that has one */
};

static void
policy_set(SiftChunkPlan *p, int layer)
{
	p->layer = layer;
	for (int c = 0; c < p->sc->ncols; c++)
		SIFT_INVARIANT(sift_cc_policy_parse(layers[layer].spec, &p->pol[c]) == NULL, "sift_col spool: the codec policy does not parse");
}

static bool
layer_available(int layer)
{
	return layers[layer].alg == SIFT_CC_ALG_NONE || sift_colz_available(layers[layer].alg);
}

/* ---------------------------------------------------------------------------
 * Writing
 * ------------------------------------------------------------------------- */

static SiftChunkSpool *
spool_create(MemoryContext parent, SiftChunkPlan *p, bool counted)
{
	SiftChunkSpool *s = MemoryContextAllocZero(parent, sizeof(SiftChunkSpool));
	MemoryContext old;

	s->plan = p;
	s->cxt = AllocSetContextCreate(parent, "sift_col spool chunks", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(parent);
	s->file = counted ? NULL : BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	return s;
}

SiftChunkSpool *
sift_chunkspool_create(MemoryContext parent, SiftChunkPlan *p)
{
	return spool_create(parent, p, false);
}

static void
open_batch(SiftChunkSpool *s, uint32 rows)
{
	SiftChunkPlan *p = s->plan;
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	s->b = sift_cbatch_create(s->cxt, p->sc);
	s->b->keyed = true;
	sift_cbatch_reserve(s->b, rows);
	s->rd = palloc0(p->sc->ncols * sizeof(SiftCDict *));
	for (int c = 0; c < p->sc->ncols; c++)
		if (p->str[c])
			s->rd[c] = sift_cdict_create(s->cxt);
	MemoryContextSwitchTo(old);
}

static void
put_chunk(SiftChunkSpool *s, const char *data, uint32 len)
{
	if (s->file != NULL)
	{
		BufFileWrite(s->file, &len, sizeof(len));
		BufFileWrite(s->file, data, len);
	}
	s->fbytes += sizeof(len) + len;
}

/* The rows of the batch are written as a chunk, and the batch starts again */
static void
flush_chunk(SiftChunkSpool *s)
{
	SiftChunkPlan *p = s->plan;
	SiftColBatch *b = s->b;
	MemoryContext old;
	bool	   *inl;
	uint32	  **map;
	Size		start;

	if (b == NULL || b->nrows == 0)
		return;
	old = MemoryContextSwitchTo(s->cxt);
	inl = palloc0(p->sc->ncols * sizeof(bool));
	map = palloc0(p->sc->ncols * sizeof(uint32 *));
	for (int c = 0; c < p->sc->ncols; c++)
	{
		const SiftCDict *d = b->dict[c];

		if (d == NULL || (inl[c] = sift_cbatch_str_inline(b, c, s->rd[c], p->dict_cap)))
			continue;
		map[c] = palloc(Max(d->n, 1) * sizeof(uint32));
		for (uint32 i = 0; i < d->n; i++)
			map[c][i] = sift_cdict_intern(s->rd[c], d->bytes + d->off[i], d->off[i + 1] - d->off[i]);
	}
	MemoryContextSwitchTo(old);
	resetStringInfo(&p->scratch);
	sift_cunit_encode(b, map, inl, p->pol, true, false, NULL, &p->scratch, &start, NULL);
	put_chunk(s, p->scratch.data + start, (uint32) (p->scratch.len - start));
	for (int c = 0; c < p->sc->ncols; c++)
		if (map[c] != NULL)
			pfree(map[c]);
	pfree(map);
	pfree(inl);
	sift_cbatch_reset(b, 0);
	b->keyed = true;
}

static bool
over_share(const SiftChunkSpool *s)
{
#ifdef SIFT_TESTING
	if (sift_test_col_spool_unbounded)
		return false;
#endif
	return MemoryContextMemAllocated(s->cxt, false) >= s->plan->mem_cap;
}

void
sift_chunkspool_add_rows(SiftChunkSpool *s, const SiftColRowRef *ref, uint32 n)
{
	SiftChunkPlan *p = s->plan;

	if (s->b == NULL)
		open_batch(s, p->chunk_rows);
	while (n > 0)
	{
		uint32		take = Min(n, p->chunk_rows - s->b->nrows);

		sift_cbatch_add_scoped_rows(s->b, ref, sizeof(SiftColRowRef), take, -1, 0, NULL);
		ref += take;
		n -= take;
		if (s->b->nrows >= p->chunk_rows)
			flush_chunk(s);
		else if (over_share(s))
		{
			flush_chunk(s);
			sift_cbatch_release(s->b);
			s->b->keyed = true;
		}
	}
}

void
sift_chunkspool_seal(SiftChunkSpool *s)
{
	SiftChunkPlan *p = s->plan;

	flush_chunk(s);
	s->chunks_end = s->fbytes;
	if (s->file != NULL)
		BufFileTell(s->file, &s->dict_fileno, &s->dict_off);
	for (int c = 0; c < p->sc->ncols; c++)
		if (s->rd != NULL && s->rd[c] != NULL)
		{
			StringInfoData one;

			initStringInfo(&one);
			sift_cdict_serialize(s->rd[c], &p->pol[c], 0, &one);
			put_chunk(s, one.data, (uint32) one.len);
			pfree(one.data);
		}
	MemoryContextReset(s->cxt);
	s->b = NULL;
	s->rd = NULL;
}

uint64
sift_chunkspool_bytes(const SiftChunkSpool *s)
{
	return s->fbytes;
}

/* ---------------------------------------------------------------------------
 * Reading
 * ------------------------------------------------------------------------- */

static void
read_setup(SiftChunkSpool *s)
{
	SiftChunkPlan *p = s->plan;
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	s->rd = palloc0(p->sc->ncols * sizeof(SiftCDict *));
	if (s->fbytes > s->chunks_end)
	{
		SIFT_INVARIANT(BufFileSeek(s->file, s->dict_fileno, s->dict_off, SEEK_SET) == 0, "sift_col spool: cannot seek to the dictionaries");
		for (int c = 0; c < p->sc->ncols; c++)
			if (p->str[c])
			{
				uint32		len;
				Size		used;

				BufFileReadExact(s->file, &len, sizeof(len));
				enlargeStringInfo(&p->scratch, len + SIFT_COL_PAD);
				BufFileReadExact(s->file, p->scratch.data, len);
				s->rd[c] = sift_cdict_parse(s->cxt, p->scratch.data, len, &used);
				SIFT_INVARIANT(s->rd[c] != NULL, "sift_col spool: a dictionary does not parse");
			}
	}
	SIFT_INVARIANT(BufFileSeek(s->file, 0, 0, SEEK_SET) == 0, "sift_col spool: cannot rewind");
	s->tmp = AllocSetContextCreate(s->cxt, "sift_col spool chunk", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(old);
	s->reading = true;
}

bool
sift_chunkspool_read_into(SiftChunkSpool *s, SiftColBatch *dst)
{
	SiftChunkPlan *p = s->plan;
	StringInfo	buf = &p->scratch;
	SiftCUnitView v;
	SiftCDict **dicts;
	uint32		len;

	if (s->chunks_end == 0)
		return false;
	if (!s->reading)
		read_setup(s);
	if (s->rread >= s->chunks_end)
		return false;
	MemoryContextReset(s->tmp);
	BufFileReadExact(s->file, &len, sizeof(len));
	enlargeStringInfo(buf, len + SIFT_COL_PAD);
	BufFileReadExact(s->file, buf->data, len);
	memset(buf->data + len, 0, SIFT_COL_PAD);
	s->rread += sizeof(len) + len;
	SIFT_INVARIANT(sift_cunit_parse((const uint8 *) buf->data, len, p->sc, &v), "sift_col spool: a chunk does not parse");
	dicts = MemoryContextAllocZero(s->tmp, p->sc->ncols * sizeof(SiftCDict *));
	for (int c = 0; c < p->sc->ncols; c++)
		if (p->str[c])
			dicts[c] = s->rd[c] != NULL ? s->rd[c] : sift_cdict_create(s->tmp);
	sift_cunit_decode(&v, 0, dicts, NULL, NULL, dst);
	return true;
}

void
sift_chunkspool_close(SiftChunkSpool *s)
{
	if (s->file != NULL)
		BufFileClose(s->file);
	MemoryContextDelete(s->cxt);
	pfree(s);
}

/* ---------------------------------------------------------------------------
 * The plan
 * ------------------------------------------------------------------------- */

/* The memory of a spool before its first row, and what each row of the batch adds to it, as the rows measure them */
static void
measure_memory(SiftChunkPlan *p, const SiftColRowRef *ref, uint32 n, Size *fixed, double *per_row)
{
	SiftChunkSpool *s = spool_create(CurrentMemoryContext, p, true);
	SiftChunkPlan probe = *p;
	Size		base,
				full;

	s->plan = &probe;
	open_batch(s, 0);
	base = MemoryContextMemAllocated(s->cxt, false);
	sift_cbatch_reserve(s->b, n);
	sift_cbatch_add_scoped_rows(s->b, ref, sizeof(SiftColRowRef), n, -1, 0, NULL);
	full = MemoryContextMemAllocated(s->cxt, false);
	*fixed = base;
	*per_row = (double) (full - base) / n;
	sift_chunkspool_close(s);
}

/* The bytes the rows take as chunks of the plan's size and the layer in hand, counted */
static uint64
price_rows(SiftChunkPlan *p, const SiftColRowRef *ref, uint32 n)
{
	SiftChunkSpool *s = spool_create(CurrentMemoryContext, p, true);
	uint64		bytes;

	sift_chunkspool_add_rows(s, ref, n);
	sift_chunkspool_seal(s);
	bytes = s->fbytes;
	sift_chunkspool_close(s);
	return bytes;
}

/* The bytes of strings the dictionary of a leaf of a spool may hold: a quarter of the memory of the spool for all the leaves that have one */
static Size
dict_cap_of(const SiftChunkPlan *p, Size spool_share)
{
	return Min(Max(spool_share / (4 * DICT_MEM_FACTOR * Max(p->nstr, 1)), DICT_CAP_MIN), DICT_CAP_MAX);
}

/*
 * The layer over the cascade codecs: the heaviest the build's libraries have (zstd, else lz4) when the sample's chunk is at least LAYER_GAIN_PCT smaller with it than with the cascade alone.
 * (zstd is never worse than lz4 where lz4 helps, so lz4 is a candidate only when the build has no zstd.)
 */
static void
pick_layer(SiftChunkPlan *p, const SiftColRowRef *ref, uint32 n)
{
	int			heavy = layer_available(LAYER_ZSTD) ? LAYER_ZSTD : layer_available(LAYER_LZ4) ? LAYER_LZ4 : LAYER_CASCADE;
	uint64		plain;

#ifdef SIFT_TESTING
	if (sift_test_col_spool_layer > 0)
	{
		policy_set(p, sift_test_col_spool_layer == 4 ? LAYER_NONE : sift_test_col_spool_layer - 1);
		return;
	}
#endif
	policy_set(p, LAYER_CASCADE);
	if (heavy == LAYER_CASCADE)
		return;
	plain = price_rows(p, ref, n);
	policy_set(p, heavy);
	if (price_rows(p, ref, n) * 100 > plain * (100 - LAYER_GAIN_PCT))
		policy_set(p, LAYER_CASCADE);
}

SiftChunkPlan *
sift_chunkplan_make(const SiftColSchema *sc, const SiftColRowRef *ref, uint32 n, int nspools, Size share)
{
	SiftChunkPlan *p = palloc0(sizeof(SiftChunkPlan));
	MemoryContext caller = CurrentMemoryContext;
	MemoryContext tcx = AllocSetContextCreate(caller, "sift_col spool plan", ALLOCSET_DEFAULT_SIZES);
	SiftColBatch *shape = sift_cbatch_create(tcx, sc);
	Size		fixed,
				spool_share,
				dict_mem,
				room;
	double		per_row;
	uint32		chunk = CHUNK_ROWS_MAX;
	uint32		priced;
	bool		fits;

	p->sc = sc;
	p->pol = palloc(sc->ncols * sizeof(SiftCcPolicy));
	p->str = palloc0(sc->ncols * sizeof(bool));
	policy_set(p, 0);
	initStringInfo(&p->scratch);
	for (int c = 0; c < sc->ncols; c++)
		if (shape->dict[c] != NULL)
		{
			p->str[c] = true;
			p->nstr++;
		}
#ifdef SIFT_TESTING
	if (sift_test_col_spool_share_kb > 0)
		share = (Size) sift_test_col_spool_share_kb * 1024;
#endif
	spool_share = share / Max(nspools, 1);
	p->mem_cap = spool_share;
	p->dict_cap = dict_cap_of(p, spool_share);
	dict_mem = p->nstr * p->dict_cap * DICT_MEM_FACTOR;

	MemoryContextSwitchTo(tcx);
	measure_memory(p, ref, Min(n, PROBE_ROWS), &fixed, &per_row);
	room = spool_share > fixed + dict_mem ? spool_share - fixed - dict_mem : 0;
	while (chunk > CHUNK_ROWS_MIN && (double) chunk * per_row > (double) room)
		chunk /= 2;
	fits = (double) chunk * per_row <= (double) room;
#ifdef SIFT_TESTING
	if (sift_test_col_spool_unbounded)
		chunk = CHUNK_ROWS_MAX;
	if (sift_test_col_spool_chunk_rows > 0)
		chunk = sift_test_col_spool_chunk_rows;
	fits |= sift_test_col_spool_unbounded || sift_test_col_spool_chunk_rows > 0;
	sift_test_col_spool_used_rows = fits ? (int) chunk : 0;
#endif
	if (fits)
	{
		p->chunk_rows = chunk;
		pick_layer(p, ref, Min(n, PICK_ROWS));
		priced = Min(n, PRICE_ROWS);
		p->row_bytes = (double) price_rows(p, ref, priced) / priced;
	}
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(tcx);
	if (!fits)
	{
		pfree(p->scratch.data);
		pfree(p->str);
		pfree(p->pol);
		pfree(p);
		return NULL;
	}
	return p;
}

uint32
sift_chunkplan_rows(const SiftChunkPlan *p)
{
	return p->chunk_rows;
}

double
sift_chunkplan_row_bytes(const SiftChunkPlan *p)
{
	return p->row_bytes;
}

const char *
sift_chunkplan_layer(const SiftChunkPlan *p)
{
	return layers[p->layer].name;
}
