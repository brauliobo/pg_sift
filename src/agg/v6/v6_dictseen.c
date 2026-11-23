/*
 * agg/v6/v6_dictseen.c - the distinct strings of dictionary-coded rows (v6_dictseen.h).
 */
#include "agg/kern6.h"
#include "agg/v6/v6_dictseen.h"

#define DICTSEEN_MIN_CAP		(256 * 1024)

/* A run of strings: n of them, string i is bytes[off[i] .. off[i + 1]) */
typedef struct DictRun
{
	uint32		n;
	uint32	   *off;
	char	   *bytes;
	uint32		off_cap;
	Size		bytes_cap;
} DictRun;

/* A string the parts have named: its hash (never 0), and where its bytes are in the arena */
typedef struct DictSlot
{
	uint64		hash;
	uint32		off;
	uint32		len;
} DictSlot;

struct SiftDictSeen
{
	MemoryContext cx;
	uint64		cap;
	bool		have;			/* a part is being read: its dictionary is in part, its seen codes in bits */
	bool		emitted;		/* a string has gone to emit: the strings of the set are not all there are */
	uint64		gen;
	uint32		n;
	DictRun		part;
	uint64	   *bits;
	uint32		bits_cap;
	DictSlot   *ht;				/* the strings of the parts folded so far, each once: an open-addressed table of the hashes, the bytes in arena */
	uint32		ht_cap;			/* a power of 2, or 0 before the first string */
	uint32		ht_used;
	char	   *arena;
	Size		arena_len;
	Size		arena_cap;
};

/* the bits of word w of a bitmap of n rows that are rows */
static inline uint64
word_mask(uint16 n, uint32 w)
{
	return (w + 1) * 64 <= n ? ~UINT64CONST(0) : (UINT64CONST(1) << (n & 63)) - 1;
}

static inline uint32
run_len(const DictRun *r, uint32 i)
{
	return r->off[i + 1] - r->off[i];
}

static inline uint64
str_hash(const char *p, uint32 len)
{
	return sift_aggk6_hash_bytes(p, len) | 1;
}

/* the home slot of a hash in a table of cap slots: the high bits, as the lowest of a hash that is never 0 is always set */
static inline uint32
slot_of(uint64 hash, uint32 cap)
{
	return (uint32) (hash >> 32) & (cap - 1);
}

static void
run_room(SiftDictSeen *d, DictRun *r, uint32 n, Size bytes)
{
	if (n + 1 > r->off_cap)
	{
		if (r->off != NULL)
			pfree(r->off);
		r->off_cap = Max(n + 1, 2 * r->off_cap);
		r->off = MemoryContextAlloc(d->cx, (Size) r->off_cap * sizeof(uint32));
	}
	if (bytes > r->bytes_cap || r->bytes == NULL)
	{
		if (r->bytes != NULL)
			pfree(r->bytes);
		r->bytes_cap = Max(Max(bytes, 2 * r->bytes_cap), 64);
		r->bytes = MemoryContextAlloc(d->cx, r->bytes_cap);
	}
}

static void
run_free(DictRun *r)
{
	if (r->off != NULL)
		pfree(r->off);
	if (r->bytes != NULL)
		pfree(r->bytes);
	*r = (DictRun) {0};
}

SiftDictSeen *
sift_dictseen_create(MemoryContext cx, uint64 cap_bytes)
{
	SiftDictSeen *d = MemoryContextAllocZero(cx, sizeof(SiftDictSeen));

	d->cx = cx;
	d->cap = Max(cap_bytes, DICTSEEN_MIN_CAP);
	return d;
}

/* The part a batch is of: its dictionary kept (the caller's lasts only for the call) and no code seen */
static void
part_start(SiftDictSeen *d, const SiftDictSrc *src)
{
	const uint32 words = (src->n + 63) / 64;

	run_room(d, &d->part, src->n, src->off[src->n]);
	memcpy(d->part.off, src->off, ((Size) src->n + 1) * sizeof(uint32));
	memcpy(d->part.bytes, src->bytes, src->off[src->n]);
	d->part.n = src->n;
	if (words > d->bits_cap)
	{
		if (d->bits != NULL)
			pfree(d->bits);
		d->bits_cap = Max(words, 2 * d->bits_cap);
		d->bits = MemoryContextAlloc(d->cx, (Size) d->bits_cap * sizeof(uint64));
	}
	if (words > 0)
		memset(d->bits, 0, (Size) words * sizeof(uint64));
	d->have = true;
	d->gen = src->gen;
	d->n = src->n;
}

static void
ht_grow(SiftDictSeen *d)
{
	DictSlot   *old = d->ht;
	const uint32 ocap = d->ht_cap;

	d->ht_cap = ocap == 0 ? 1024 : ocap * 2;
	d->ht = MemoryContextAllocZero(d->cx, (Size) d->ht_cap * sizeof(DictSlot));
	for (uint32 i = 0; i < ocap; i++)
		if (old[i].hash != 0)
		{
			uint32		at = slot_of(old[i].hash, d->ht_cap);

			while (d->ht[at].hash != 0)
				at = (at + 1) & (d->ht_cap - 1);
			d->ht[at] = old[i];
		}
	if (old != NULL)
		pfree(old);
}

/* a string joins the set once: a hash probe, and the bytes appended to the arena; the table moves 16 bytes a string when it grows, never the bytes */
static void
seen_add(SiftDictSeen *d, const char *p, uint32 len)
{
	const uint64 h = str_hash(p, len);
	uint32		at;

	if ((d->ht_used + 1) * 10 > d->ht_cap * 7)
		ht_grow(d);
	for (at = slot_of(h, d->ht_cap); d->ht[at].hash != 0; at = (at + 1) & (d->ht_cap - 1))
		if (d->ht[at].hash == h && d->ht[at].len == len && memcmp(d->arena + d->ht[at].off, p, len) == 0)
			return;
	if (d->arena_len + len > d->arena_cap)
	{
		d->arena_cap = Max(Max(2 * d->arena_cap, d->arena_len + len), 4096);
		d->arena = d->arena == NULL ? MemoryContextAlloc(d->cx, d->arena_cap) : repalloc(d->arena, d->arena_cap);
	}
	memcpy(d->arena + d->arena_len, p, len);
	d->ht[at] = (DictSlot) {h, (uint32) d->arena_len, len};
	d->arena_len += len;
	d->ht_used++;
}

/* every string of the set goes to emit and the set starts again, with no table and no arena */
static void
seen_emit(SiftDictSeen *d, SiftDictSeenEmit emit, void *arg)
{
	d->emitted = true;
	for (uint32 i = 0; i < d->ht_cap; i++)
		if (d->ht[i].hash != 0)
			emit(arg, d->arena + d->ht[i].off, d->ht[i].len);
	if (d->ht != NULL)
		pfree(d->ht);
	if (d->arena != NULL)
		pfree(d->arena);
	d->ht = NULL;
	d->arena = NULL;
	d->ht_cap = d->ht_used = 0;
	d->arena_len = d->arena_cap = 0;
}

/* The part's seen strings join the set (a dictionary in no order is as good as one in byte order) */
static void
fold_part(SiftDictSeen *d, SiftDictSeenEmit emit, void *arg)
{
	const uint32 words = (d->n + 63) / 64;

	if (!d->have)
		return;
	d->have = false;
	for (uint32 w = 0; w < words; w++)
		for (uint64 m = d->bits[w]; m != 0; m &= m - 1)
		{
			const uint32 c = w * 64 + (uint32) __builtin_ctzll(m);

			seen_add(d, d->part.bytes + d->part.off[c], run_len(&d->part, c));
		}
	if (d->arena_len + (uint64) d->ht_cap * sizeof(DictSlot) > d->cap)
		seen_emit(d, emit, arg);
}

bool
sift_dictseen_fits(const SiftDictSrc *src)
{
	return src->n <= SIFT_DICTSEEN_MAX_CODES && src->off[src->n] <= SIFT_DICTSEEN_MAX_BYTES;
}

void
sift_dictseen_part(SiftDictSeen *d, const SiftDictSrc *src, SiftDictSeenEmit emit, void *arg)
{
	fold_part(d, emit, arg);
	for (uint32 c = 0; c < src->n; c++)
		seen_add(d, src->bytes + src->off[c], src->off[c + 1] - src->off[c]);
	if (d->arena_len + (uint64) d->ht_cap * sizeof(DictSlot) > d->cap)
		seen_emit(d, emit, arg);
}

bool
sift_dictseen_exact(SiftDictSeen *d, SiftDictSeenEmit emit, void *arg, uint64 *count)
{
	fold_part(d, emit, arg);
	*count = d->ht_used;
	return !d->emitted;
}

bool
sift_dictseen_batch(SiftDictSeen *d, const SiftDictSrc *src, const uint32 *code, const uint64 *take, const uint64 *valid, uint16 n_raw, SiftDictSeenEmit emit, void *arg)
{
	if (src->n > SIFT_DICTSEEN_MAX_CODES || src->off[src->n] > SIFT_DICTSEEN_MAX_BYTES)
		return false;
	if (!d->have || d->gen != src->gen)
	{
		fold_part(d, emit, arg);
		part_start(d, src);
	}
	for (uint32 w = 0; w < (uint32) (n_raw + 63) / 64; w++)
		for (uint64 m = (valid != NULL ? take[w] & valid[w] : take[w]) & word_mask(n_raw, w); m != 0; m &= m - 1)
		{
			const uint32 c = code[w * 64 + (uint32) __builtin_ctzll(m)];

			Assert(c < d->n);
			d->bits[c >> 6] |= UINT64CONST(1) << (c & 63);
		}
	return true;
}

void
sift_dictseen_drain(SiftDictSeen *d, SiftDictSeenEmit emit, void *arg)
{
	fold_part(d, emit, arg);
	seen_emit(d, emit, arg);
}

uint64
sift_dictseen_count(const SiftDictSeen *d)
{
	return d->ht_used;
}

void
sift_dictseen_free(SiftDictSeen *d)
{
	run_free(&d->part);
	if (d->bits != NULL)
		pfree(d->bits);
	if (d->ht != NULL)
		pfree(d->ht);
	if (d->arena != NULL)
		pfree(d->arena);
	pfree(d);
}
