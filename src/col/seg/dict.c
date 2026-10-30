/*
 * col/seg/dict.c - the dictionaries of a segment: the lookups by value, the lazy views,
 * the counts and places of a value, the text signatures
 * and the checks of their hashes and counts
 */
#include "core/host.h"
#include "col/compress.h"
#include "col/seg_int.h"

/* Where the dictionary of leaf col lies in the stream: offset and length (up to the next dictionary or the end of the section) */
static void
dict_region(SiftColSegment *seg, int col, uint64 *off, uint64 *len)
{
	const SiftSectionRef *s = seg_sec(seg, SIFT_SEC_DICT);
	uint32		hdr[2];
	uint64		next,
				tab[SIFT_COL_MAX_COLS];

	seg_points_read(seg);
	next = PRIV(seg)->dict_end;

	seg_seg_read(seg, s->off, sizeof(hdr), hdr);
	if (hdr[0] != DICTS_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
		corrupt(seg, "DICTS section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
	seg_seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
	*off = tab[col];
	for (int c = 0; c < seg->schema->ncols; c++)
		if (tab[c] > *off && tab[c] < next)
			next = tab[c];
	if (*off == 0 || *off >= s->len)
		corrupt(seg, "Leaf %d has no dictionary.", col);
	*len = next - *off;
}

uint64
sift_cseg_dict_stored(SiftColSegment *seg, int col)
{
	uint64		off,
				len;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return 0;
	dict_region(seg, col, &off, &len);
	return len;
}

/* The dictionary of leaf col read from the segment into mcxt (the loader of the backend's cache, col/dictcache.c) */
static SiftCDict *
dict_load(SiftColSegment *seg, int col, MemoryContext mcxt)
{
	uint64		off,
				len;
	char	   *data;
	Size		used;
	SiftCDict  *d;

	dict_region(seg, col, &off, &len);
	/* the plain form is read into the arrays of the dictionary, not into a buffer that is copied (a scan of a leaf reads it whole once for each backend) */
	if (len >= DICT_HEAD)
	{
		uint32		head[2];
		uint64		at = seg_sec(seg, SIFT_SEC_DICT)->off + off;

		seg_seg_read(seg, at, sizeof(head), head);
		if (head[0] == DICT_PLAIN && (len - DICT_HEAD) / sizeof(uint32) >= (uint64) head[1] + 1)
		{
			d = sift_cdict_plain_begin(mcxt, head[1]);
			seg_seg_read(seg, at + DICT_HEAD, ((Size) head[1] + 1) * sizeof(uint32), d->off);
			if (!sift_cdict_plain_bytes(d, mcxt, len - DICT_HEAD - ((uint64) head[1] + 1) * sizeof(uint32)))
				corrupt(seg, "The dictionary of leaf %d is malformed.", col);
			seg_seg_read(seg, at + DICT_HEAD + ((uint64) head[1] + 1) * sizeof(uint32), d->nbytes, d->bytes);
			if (!sift_cdict_plain_valid(d))
				corrupt(seg, "The dictionary of leaf %d is malformed.", col);
			return d;
		}
	}
	data = palloc(len);
	seg_seg_read(seg, seg_sec(seg, SIFT_SEC_DICT)->off + off, len, data);
	d = sift_cdict_parse(mcxt, data, len, &used);
	if (d == NULL)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	pfree(data);
	return d;
}

SiftCDict *
sift_cseg_dict(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (pv->dict[col] == NULL)
		pv->dict[col] = sift_cseg_dict_cached(seg, col, dict_load);
	return pv->dict[col];
}

/* Block b of a dictionary of the searched form, decompressed into l->blkraw with the offsets of its strings in l->blkoff */
static void
look_load_block(DictLook *l, uint32 b)
{
	uint32		o[2],
				cnt = Min(DICT_BLOCK, l->n - b * DICT_BLOCK),
				rawlen,
				pos = 0;

	seg_seg_read(l->seg, l->boff_at + (uint64) b * sizeof(uint32), sizeof(o), o);
	if (o[1] < o[0] + DICT_BLOCK_HEAD || o[1] - o[0] > sizeof(l->blkz) || o[1] > l->blk_len)
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	seg_seg_read(l->seg, l->blk_at + o[0], o[1] - o[0], l->blkz);
	rawlen = l->blkz[2] | (uint32) l->blkz[3] << 8;
	if (rawlen > DICT_BLOCK_RAW_MAX)
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	if (l->blkz[0] == 0)
	{
		if (o[1] - o[0] - DICT_BLOCK_HEAD != rawlen)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		memcpy(l->blkraw, l->blkz + DICT_BLOCK_HEAD, rawlen);
	}
	else if ((l->blkz[0] != SIFT_CC_ALG_ZSTD && l->blkz[0] != SIFT_CC_ALG_LZ4) || !sift_colz_available(l->blkz[0]))
		corrupt(l->seg, "The dictionary of leaf %d is compressed with an algorithm this build lacks.", l->col);
	else if (!sift_colz_decompress(l->blkz[0], l->blkz + DICT_BLOCK_HEAD, o[1] - o[0] - DICT_BLOCK_HEAD, l->blkraw, rawlen))
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	if (l->fixed)
	{
		if (rawlen != cnt * l->fixed)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		for (uint32 i = 0; i <= cnt; i++)
			l->blkoff[i] = i * l->fixed;
	}
	else
	{
		if (rawlen < cnt)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		for (uint32 i = 0; i < cnt; i++)
		{
			l->blkoff[i] = cnt + pos;
			pos += l->blkraw[i];
		}
		l->blkoff[cnt] = cnt + pos;
		if (cnt + pos != rawlen)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	}
	l->blkno = b;
	l->loads++;
	if (l->bc != NULL)
		l->bc->spent += l->blkz[0] == 0 ? DICT_READ_COST : l->blkz[0] == SIFT_CC_ALG_LZ4 ? DICT_LZ4_COST : DICT_ZSTD_COST;
}

/* The string of a code of a dictionary in the stream, into buf (SIFT_COL_DICT_MAX_ENTRY bytes) */
static const char *
look_code_str(DictLook *l, uint32 code, uint32 *len, char *buf)
{
	uint64		at;

	if (code >= l->n)
		corrupt(l->seg, "Code %u is beyond the %u strings of the dictionary of leaf %d.", code, l->n, l->col);
	if (l->blocked)
	{
		uint32		i = code % DICT_BLOCK;

		if (l->blkno != code / DICT_BLOCK)
			look_load_block(l, code / DICT_BLOCK);
		*len = l->blkoff[i + 1] - l->blkoff[i];
		memcpy(buf, l->blkraw + l->blkoff[i], *len);
		return buf;
	}
	if (l->fixed)
	{
		*len = l->fixed;
		at = l->bytes_at + (uint64) code * l->fixed;
	}
	else
	{
		uint32		o[2];

		seg_seg_read(l->seg, l->offs_at + (uint64) code * sizeof(uint32), sizeof(o), o);
		if (o[1] < o[0] || o[1] > l->nbytes || o[1] - o[0] > SIFT_COL_DICT_MAX_ENTRY)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		*len = o[1] - o[0];
		at = l->bytes_at + o[0];
	}
	seg_seg_read(l->seg, at, *len, buf);
	return buf;
}

static inline BisectSlot *
look_slot(const DictLook *l, uint32 pos)
{
	return l->bc ? &l->bc->slot[(pos * 0x9E3779B1u) >> 8 & l->bc->mask] : NULL;
}

static uint32
look_perm(const DictLook *l, uint32 pos)
{
	const BisectSlot *s = look_slot(l, pos);
	uint32		code;

	if (s != NULL && s->pos1 == pos + 1)
		return s->code;
	if (l->bc != NULL)
		l->bc->spent += DICT_READ_COST;
	seg_seg_read(l->seg, l->perm_at + (uint64) pos * sizeof(uint32), sizeof(code), &code);
	if (code >= l->n)
		corrupt(l->seg, "The order of the dictionary of leaf %d names code %u of %u.", l->col, code, l->n);
	return code;
}

/* A string of the order by position; the buffer is the one of the search (a bisection holds one string at a time) */
static const char *
look_sorted_str(const void *ctx, uint32 pos, uint32 *len)
{
	DictLook   *l = (DictLook *) ctx;
	BisectSlot *s = look_slot(l, pos);
	uint32		code;
	const char *str;

	if (s != NULL && s->pos1 == pos + 1)
	{
		if (s->hits < UINT8_MAX)
			s->hits++;
		if (s->len != BISECT_NOSTR)
		{
			*len = s->len;
			return s->str;
		}
		return look_code_str(l, s->code, len, l->buf);
	}
	code = look_perm(l, pos);
	str = look_code_str(l, code, len, l->buf);
	/* a miss takes over an empty slot or one nothing has used since the last miss on it, and otherwise ages it: what every search shares (the top of the bisection) stays */
	if (s != NULL && (s->pos1 == 0 || s->hits == 0))
	{
		s->pos1 = pos + 1;
		s->code = code;
		s->hits = 0;
		s->len = *len <= BISECT_STR ? (uint16) *len : BISECT_NOSTR;
		if (s->len != BISECT_NOSTR)
			memcpy(s->str, str, *len);
	}
	else if (s != NULL)
		s->hits--;
	return str;
}

/* The cache of the positions of a dictionary of many strings, once per search */
static void
look_cache(DictLook *l)
{
	if (!l->bc_tried)
	{
		l->bc_tried = true;
		if (l->n >= BISECT_MIN && HEAD_CACHED())
			l->bc = sift_cseg_bisect(l->seg, l->col, l->n);
	}
}

#define HASH_WINDOW	32				/* slots read at a time */

/*
 * The code of a string by the hash of the dictionary (DICT_HASHED): the slots from its position are read a window at a time up to an empty one; those with its fingerprint name a
 * code, whose string is read (a block) and compared. A string the dictionary lacks costs the read of a window and no block, almost always.
 */
static int64
look_find_hash(DictLook *l, const char *s, uint32 len)
{
	uint64		h = sift_cdict_hash(s, len);
	uint32		ns = SIFT_CDICT_HASH_SLOTS(l->n),
				cb = sift_cdict_hash_cb(l->n),
				fp = (uint32) h & (UINT32_MAX >> cb),
				i = sift_cdict_hash_pos(h, ns),
				win[HASH_WINDOW];

	for (uint32 probed = 0; probed < ns;)
	{
		uint32		k = Min(HASH_WINDOW, ns - i);

		seg_seg_read(l->seg, l->hash_at + (uint64) i * sizeof(uint32), k * sizeof(uint32), win);
		for (uint32 j = 0; j < k; j++, probed++)
		{
			uint32		code,
						slen,
						loads;
			const char *str;

			if (win[j] == 0)
				return -1;
			if (win[j] >> cb != fp)
				continue;
			code = (win[j] & (UINT32_MAX >> (32 - cb))) - 1;
			if (code >= l->n)
				corrupt(l->seg, "The hash of the dictionary of leaf %d names code %u of %u.", l->col, code, l->n);
			loads = l->loads;
			str = look_code_str(l, code, &slen, l->buf);
			sift_cseg_dict_spent(l->seg, l->col, l->loads != loads ? DICT_ZSTD_COST : DICT_READ_COST);
			if (slen == len && memcmp(str, s, len) == 0)
				return code;
		}
		i = i + k == ns ? 0 : i + k;
	}
	return -1;
}

static int64
look_find(const SiftCDictLook *look, const char *s, uint32 len)
{
	const DictLook *l = (const DictLook *) look;
	SiftCSorted so = {l->n, look_sorted_str, l};
	SiftColValue c = {.v.s = s,.len = len};
	uint32		pos;

	if (l->hash_at != 0)
		return look_find_hash((DictLook *) l, s, len);
	look_cache((DictLook *) l);
	pos = sift_csorted_bound(&so, &c, false);

	return pos < l->n && sift_csorted_cmp(&so, pos, &c) == 0 ? (int64) look_perm(l, pos) : -1;
}

/*
 * The strings that hold c, block by block in code order: a block of DICT_BLOCK strings is a word of lut, and its bytes and offsets are what str_contains takes.
 * With the trigram signatures of the blocks (column format minor 29) a literal of three bytes or more reads, decompresses and scans only the blocks whose
 * signature has the bit of each of its trigrams; *passed is how many that were.
 */
#define SIG_CHUNK	(64 * 1024)

static uint32
look_contains(DictLook *l, SiftColOp op, const SiftColValue *c, int nvals, uint64 *lut, uint32 *passed)
{
	uint32		count = 0,
				nbits = l->sig_at != 0 && op == SIFT_COLOP_CONTAINS && c->len >= 3 ? c->len - 2 : 0,
				sigbytes = l->sig_at != 0 ? (1u << l->sig_lg) / 8 : 0,
				per = nbits > 0 ? Max(SIG_CHUNK / sigbytes, 1) : 1,
				got = 0,
				first = 0;
	uint32	   *bits = NULL;
	uint8	   *sig = NULL;

	if (nbits > 0)
	{
		bits = palloc(nbits * sizeof(uint32));
		for (uint32 j = 0; j < nbits; j++)
			bits[j] = sift_tsig_bit((const uint8 *) c->v.s + j, l->sig_lg);
		sig = palloc((Size) per * sigbytes);
	}
	*passed = 0;
	for (uint32 b = 0; b * DICT_BLOCK < l->n; b++)
	{
		uint32		cnt = Min(DICT_BLOCK, l->n - b * DICT_BLOCK),
					off[DICT_BLOCK + 1];

		if (nbits > 0)
		{
			const uint8 *s;
			bool		may = true;

			if (b >= first + got)
			{
				first = b;
				got = Min(per, (l->n + DICT_BLOCK - 1) / DICT_BLOCK - b);
				seg_seg_read(l->seg, l->sig_at + (uint64) b * sigbytes, (Size) got * sigbytes, sig);
			}
			s = sig + (Size) (b - first) * sigbytes;
			for (uint32 j = 0; j < nbits && may; j++)
				may = s[bits[j] >> 3] >> (bits[j] & 7) & 1;
			if (!may)
				continue;
		}
		(*passed)++;
		if (l->blkno != b)
			look_load_block(l, b);
		for (uint32 i = 0; i <= cnt; i++)
			off[i] = l->blkoff[i];
		if (op == SIFT_COLOP_SUFFIX)
		{
			lut[b] = 0;
			for (uint32 i = 0; i < cnt; i++)
				if (sift_cstr_matches(op, (const char *) l->blkraw + off[i], off[i + 1] - off[i], c))
					lut[b] |= UINT64CONST(1) << i;
		}
		else
			sift_cstrs_contains(lut + b, (const char *) l->blkraw, off, cnt, op, c, nvals);
		count += pg_popcount64(lut[b]);
	}
	if (bits)
	{
		pfree(bits);
		pfree(sig);
	}
	/* a signature that leaves over half the blocks (a short part, a frequent one) saves little: the next search parses the dictionary and keeps it if the cache holds it, as for one without signatures */
	if (l->sig_at != 0 && *passed * 2 > (l->n + DICT_BLOCK - 1) / DICT_BLOCK)
		sift_cseg_dict_heavy(l->seg, l->col, (uint64) l->n * DICT_PARSE_COST);
	return count;
}

static uint32
look_match(const SiftCDictLook *look, SiftColOp op, const SiftColValue *c, int nvals, uint64 *lut)
{
	const DictLook *l = (const DictLook *) look;
	SiftCSorted so = {l->n, look_sorted_str, l};
	uint32		from,
				to;

	if (SIFT_COLOP_SCANS(op))
		return look_contains((DictLook *) l, op, c, nvals, lut, &(uint32) {0});
	look_cache((DictLook *) l);
	sift_csorted_range(&so, op, c, &from, &to);
	for (uint32 pos = from; pos < to;)
	{
		uint32		codes[1024],
					k = Min(to - pos, lengthof(codes));

		seg_seg_read(l->seg, l->perm_at + (uint64) pos * sizeof(uint32), k * sizeof(uint32), codes);
		for (uint32 i = 0; i < k; i++)
		{
			if (codes[i] >= l->n)
				corrupt(l->seg, "The order of the dictionary of leaf %d names code %u of %u.", l->col, codes[i], l->n);
			lut[codes[i] >> 6] |= UINT64CONST(1) << (codes[i] & 63);
		}
		pos += k;
	}
	return to - from;
}

/* A dictionary of blocks and a hash without an order: CONTAINS reads the blocks, every other test needs the strings in order, so the dictionary is parsed (and kept, as for a dictionary read whole) */
static uint32
look_match_blocks(const SiftCDictLook *look, SiftColOp op, const SiftColValue *c, int nvals, uint64 *lut)
{
	const DictLook *l = (const DictLook *) look;

	if (op == SIFT_COLOP_CONTAINS || op == SIFT_COLOP_ICONTAINS)
		return look_contains((DictLook *) l, op, c, nvals, lut, &(uint32) {0});
	return sift_cdict_match(sift_cseg_dict(l->seg, l->col), op, c, nvals, lut);
}

/* The blocks and the string buffer of a DictLook are written before they are read: only what comes before them is cleared (clearing them costs 34,000 instructions a query and segment) */
static DictLook *
look_alloc(SiftColSegment *seg)
{
	DictLook   *l = MemoryContextAlloc(seg->mcxt, sizeof(DictLook));

	memset(l, 0, offsetof(DictLook, blkoff));
	return l;
}

/* Where the trigram signatures of the blocks lie, when the dictionary has them: after the form, 8-aligned (the dictionary starts 8-aligned), end being where the form ends */
static void
look_sig(DictLook *l, uint64 off, uint64 len, uint64 end)
{
	uint64		at = off + (end - off + 7) / 8 * 8;
	uint32		hdr[2];
	uint64		nblk = ((uint64) l->n + DICT_BLOCK - 1) / DICT_BLOCK;

	if (at + DICT_TSIG_HEAD > off + len)
		return;
	seg_seg_read(l->seg, at, sizeof(hdr), hdr);
	if (hdr[0] != DICT_TSIG_MAGIC)
		return;
	if (hdr[1] < DICT_TSIG_LO_LG || hdr[1] > DICT_TSIG_HI_LG || at + DICT_TSIG_HEAD + nblk * ((1u << hdr[1]) / 8) > off + len)
		corrupt(l->seg, "The signatures of the dictionary of leaf %d are malformed.", l->col);
	l->sig_at = at + DICT_TSIG_HEAD;
	l->sig_lg = hdr[1];
}

/* A searchable dictionary (form 2) from where its parts lie, which the backend kept from the query that read them */
static DictLook *
look_from_head(SiftColSegment *seg, int col, SiftCBisect *bc)
{
	DictLook   *l = look_alloc(seg);

	l->seg = seg;
	l->col = col;
	l->n = bc->head.n;
	l->fixed = bc->head.fixed;
	l->nbytes = bc->head.nbytes;
	l->boff_at = bc->head.boff_at;
	l->blk_at = bc->head.blk_at;
	l->blk_len = bc->head.blk_len;
	l->perm_at = bc->head.perm_at;
	l->cum_at = bc->head.cum_at;
	l->hash_at = bc->head.hash_at;
	l->blocked = true;
	l->blkno = UINT32_MAX;
	l->sig_at = bc->head.sig_at;
	l->sig_lg = bc->head.sig_lg;
	l->look = (SiftCDictLook) {look_find, look_match};
	l->stub.n = l->n;
	l->stub.look = &l->look;
	l->searchable = true;
	l->bc = bc;
	l->bc_tried = true;
	return l;
}

/* Where the dictionary of leaf col lies when it is stored plain or in blocks (forms 0, 2 and 3); NULL for the compact form, which is read whole */
DictLook *
seg_dict_look(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	uint64		off,
				len;
	uint32		head[4];
	DictLook   *l;
	uint64		nblk;
	uint32		blen;
	uint64		end = 0;
	SiftCBisect *bc;
	bool		hashed;

	if (pv->look_checked[col])
		return pv->look[col];
	pv->look_checked[col] = true;
	if (HEAD_CACHED() && (bc = sift_cseg_bisect_find(seg, col)) != NULL && bc->have_head)
		return pv->look[col] = look_from_head(seg, col, bc);
	dict_region(seg, col, &off, &len);
	off += seg_sec(seg, SIFT_SEC_DICT)->off;
	if (len < DICT_HEAD)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	seg_seg_read(seg, off, DICT_HEAD, head);
	hashed = (head[0] & DICT_HASHED) != 0;
	head[0] &= ~DICT_HASHED;
	if (head[0] != DICT_LOOKUP && head[0] != DICT_LOOKUP_COUNT && head[0] != DICT_BLOCKS && head[0] != DICT_PLAIN)
		return NULL;
	l = look_alloc(seg);
	l->seg = seg;
	l->col = col;
	l->n = head[1];
	if (head[0] == DICT_PLAIN)
	{
		if (len - DICT_HEAD < ((uint64) l->n + 1) * sizeof(uint32))
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		l->offs_at = off + DICT_HEAD;
		seg_seg_read(seg, l->offs_at + (uint64) l->n * sizeof(uint32), sizeof(l->nbytes), &l->nbytes);
		l->bytes_at = l->offs_at + ((uint64) l->n + 1) * sizeof(uint32);
		if (len - DICT_HEAD - ((uint64) l->n + 1) * sizeof(uint32) < l->nbytes)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	}
	else
	{
		if (len < DICT_HEAD_LOOKUP)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		seg_seg_read(seg, off, DICT_HEAD_LOOKUP, head);
		head[0] &= ~DICT_HASHED;
		l->fixed = head[2];
		l->nbytes = head[3];
		l->blocked = true;
		l->blkno = UINT32_MAX;
		nblk = ((uint64) l->n + DICT_BLOCK - 1) / DICT_BLOCK;
		l->boff_at = off + DICT_HEAD_LOOKUP;
		l->blk_at = l->boff_at + (nblk + 1) * sizeof(uint32);
		if (l->n == 0 || l->fixed > SIFT_COL_DICT_MAX_ENTRY || (l->fixed != 0 && (uint64) l->n * l->fixed != l->nbytes) ||
			l->blk_at > off + len)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		seg_seg_read(seg, l->boff_at + nblk * sizeof(uint32), sizeof(uint32), &blen);
		l->blk_len = blen;
		end = l->blk_at + blen;
		if (head[0] == DICT_LOOKUP || head[0] == DICT_LOOKUP_COUNT)
		{
			l->perm_at = (l->blk_at + blen + 3) / 4 * 4;
			if (l->perm_at + (uint64) l->n * sizeof(uint32) > off + len)
				corrupt(seg, "The dictionary of leaf %d is malformed.", col);
			end = l->perm_at + (uint64) l->n * sizeof(uint32);
			if (head[0] == DICT_LOOKUP_COUNT)
			{
				uint32		last;

				l->cum_at = l->perm_at + (uint64) l->n * sizeof(uint32);
				if (l->cum_at + DICT_COUNT_HEAD(l->n) > off + len)
					corrupt(seg, "The dictionary of leaf %d is malformed.", col);
				seg_seg_read(seg, l->cum_at + (uint64) 8 * DICT_COUNT_NB(l->n) + sizeof(uint32), sizeof(last), &last);
				if (l->cum_at + DICT_COUNT_HEAD(l->n) + last > off + len)
					corrupt(seg, "The dictionary of leaf %d is malformed.", col);
				end = l->cum_at + DICT_COUNT_HEAD(l->n) + last;
			}
			if (hashed)
			{
				l->hash_at = head[0] == DICT_LOOKUP_COUNT ? l->cum_at + DICT_COUNT_HEAD(l->n) + (uint64) 0 : l->perm_at + (uint64) l->n * sizeof(uint32);
				if (head[0] == DICT_LOOKUP_COUNT)
				{
					uint32		last;

					seg_seg_read(seg, l->cum_at + (uint64) 8 * DICT_COUNT_NB(l->n) + sizeof(uint32), sizeof(last), &last);
					l->hash_at += last;
				}
				if (l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32) > off + len)
					corrupt(seg, "The dictionary of leaf %d is malformed.", col);
				end = l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32);
			}
			look_sig(l, off, len, end);
			l->look = (SiftCDictLook) {look_find, look_match};
			l->stub.n = l->n;
			l->stub.look = &l->look;
			l->searchable = true;
			if (l->n >= BISECT_MIN && HEAD_CACHED() && (bc = sift_cseg_bisect(seg, col, l->n)) != NULL)
			{
				bc->head = (DictLookHead) {l->n, l->fixed, l->nbytes, l->offs_at, l->bytes_at, l->boff_at, l->blk_at, l->blk_len, l->perm_at, l->cum_at, l->hash_at, l->sig_at, l->sig_lg};
				bc->have_head = true;
				l->bc = bc;
				l->bc_tried = true;
			}
		}
		else if (l->blk_at + blen > off + len)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		else
		{
			if (hashed)
			{
				/* blocks, then the hash, no order: an equality finds its code in place, any other test parses the dictionary (look_match_blocks) */
				l->hash_at = (l->blk_at + blen + 3) / 4 * 4;
				if (l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32) > off + len)
					corrupt(seg, "The dictionary of leaf %d is malformed.", col);
				end = l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32);
			}
			look_sig(l, off, len, end);
			/* without the hash, form 3 is searched only by a substring test (sift_cseg_dict_search): that stub has no find */
			l->look = (SiftCDictLook) {hashed ? look_find : NULL, hashed ? look_match_blocks : look_match};
			l->stub.n = l->n;
			l->stub.look = &l->look;
		}
	}
	pv->look[col] = l;
	return l;
}

/* Whether the dictionary of leaf col holds row counts (sift_cseg_dict_count) */
bool
sift_cseg_dict_counted(SiftColSegment *seg, int col)
{
	DictLook   *l;

	return seg->schema->cols[col].store == SIFT_COLS_STR && (l = seg_dict_look(seg, col)) != NULL && l->cum_at != 0;
}

bool
sift_cseg_dict_hashed(SiftColSegment *seg, int col)
{
	DictLook   *l;

	return seg->schema->cols[col].store == SIFT_COLS_STR && (l = seg_dict_look(seg, col)) != NULL && l->hash_at != 0;
}

/* Every string of the dictionary is reachable from its position in the hash, and the hash holds nothing else */
void
sift_cseg_dict_verify_hash(SiftColSegment *seg, int col, SiftCDict *d)
{
	DictLook   *l = seg_dict_look(seg, col);
	uint32		ns = SIFT_CDICT_HASH_SLOTS(d->n),
				cb = sift_cdict_hash_cb(d->n),
				used = 0;
	uint32	   *slot = palloc(ns * sizeof(uint32));

	if (l == NULL || l->hash_at == 0)
		corrupt(seg, "The dictionary of leaf %d has no hash.", col);
	seg_seg_read(seg, l->hash_at, (Size) ns * sizeof(uint32), slot);
	for (uint32 i = 0; i < ns; i++)
		used += slot[i] != 0;
	if (used != d->n)
		corrupt(seg, "The hash of the dictionary of leaf %d holds %u strings, the dictionary %u.", col, used, d->n);
	for (uint32 c = 0; c < d->n; c++)
	{
		uint32		len;
		const char *str = sift_cdict_str(d, c, &len);
		uint64		h = sift_cdict_hash(str, len);
		uint32		i = sift_cdict_hash_pos(h, ns);

		while (slot[i] != 0 && slot[i] != sift_cdict_hash_slot(h, cb, c))
			i = i + 1 == ns ? 0 : i + 1;
		if (slot[i] == 0)
			corrupt(seg, "The hash of the dictionary of leaf %d does not hold code %u.", col, c);
	}
	pfree(slot);
}

/* The values of one block of the row counts of a dictionary: the rows of the codes at the places of block b, DICT_BLOCK of them (the places past n are 0) */
#define COUNT_PACKED_MAX	(DICT_BLOCK * 32 / 8)

static void
count_block(DictLook *l, uint32 b, uint32 *base, uint32 *vals)
{
	SiftColSegment *seg = l->seg;
	uint32		rec[4],
				bytes;
	uint8		cmin,
				buf[COUNT_PACKED_MAX + 8] = {0};

	seg_seg_read(seg, l->cum_at + (uint64) b * 8, sizeof(rec), rec);
	seg_seg_read(seg, l->cum_at + (uint64) 8 * (DICT_COUNT_NB(l->n) + 1) + b, 1, &cmin);
	bytes = rec[3] - rec[1];
	if (rec[3] < rec[1] || bytes % 8 || bytes > COUNT_PACKED_MAX || rec[2] < rec[0])
		corrupt(seg, "The row counts of the dictionary of leaf %d are malformed at block %u.", l->col, b);
	if (bytes > 0)
		seg_seg_read(seg, l->cum_at + DICT_COUNT_HEAD(l->n) + rec[1], bytes, buf);
	*base = rec[0];
	for (uint32 i = 0; i < DICT_BLOCK; i++)
		vals[i] = cmin;
	for (uint32 i = 0, w = bytes / 8; w > 0 && i < DICT_BLOCK; i++)
	{
		uint64		x = 0;
		uint32		bit = i * w;

		memcpy(&x, buf + bit / 8, sizeof(x));
		vals[i] += (uint32) (x >> (bit % 8)) & (uint32) ((UINT64CONST(1) << w) - 1);
	}
}

/* The rows of the codes at the places below pos (0 to n) of the order of a dictionary that counts them */
static uint32
look_cum(DictLook *l, uint32 pos)
{
	uint32		base,
				vals[DICT_BLOCK],
				b = pos / DICT_BLOCK;

	if (pos % DICT_BLOCK == 0)
	{
		seg_seg_read(l->seg, l->cum_at + (uint64) b * 8, sizeof(base), &base);
		return base;
	}
	count_block(l, b, &base, vals);
	for (uint32 i = 0; i < pos % DICT_BLOCK; i++)
		base += vals[i];
	return base;
}

/* sift.col_verify: the counts of the dictionary of leaf col are, at every place of its order, the rows of the code there that the units hold (rows[code]), from zero */
void
sift_cseg_dict_count_check(SiftColSegment *seg, int col, const uint32 *rows)
{
	DictLook   *l = seg_dict_look(seg, col);
	uint32		nb,
				cum = 0,
				last[2];

	Assert(l != NULL && l->cum_at != 0);
	nb = DICT_COUNT_NB(l->n);
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		base,
					vals[DICT_BLOCK],
					codes[DICT_BLOCK],
					lo = b * DICT_BLOCK,
					k = Min(l->n - lo, DICT_BLOCK);

		count_block(l, b, &base, vals);
		if (base != cum)
			corrupt(seg, "The row counts of the dictionary of leaf %d start block %u at %u rows, not the %u before it.", col, b, base, cum);
		seg_seg_read(seg, l->perm_at + (uint64) lo * sizeof(uint32), k * sizeof(uint32), codes);
		for (uint32 i = 0; i < DICT_BLOCK; i++)
		{
			if (i >= k)
				break;
			if (codes[i] >= l->n || vals[i] != rows[codes[i]])
				corrupt(seg, "The row count of the string at place %u of the dictionary of leaf %d is not the rows of its code.", lo + i, col);
			cum += vals[i];
		}
		CHECK_FOR_INTERRUPTS();
	}
	seg_seg_read(seg, l->cum_at + (uint64) nb * 8, sizeof(last), last);
	if (last[0] != cum)
		corrupt(seg, "The row counts of the dictionary of leaf %d end at %u rows, not the %u they add up to.", col, last[0], cum);
}

/* The places [from, to) of the byte order of a dictionary read in place whose strings pass op (EQ, LT, LE, GT, GE or PREFIX, bytewise) against c: two bisections */
static void
dict_places(DictLook *l, SiftColOp op, const SiftColValue *c, uint32 *from, uint32 *to)
{
	SiftCSorted so = {l->n, look_sorted_str, l};

	look_cache(l);
	if (op == SIFT_COLOP_EQ)
	{
		*from = sift_csorted_bound(&so, c, false);
		*to = *from < l->n && sift_csorted_cmp(&so, *from, c) == 0 ? *from + 1 : *from;
	}
	else
		sift_csorted_range(&so, op, c, from, to);
}

/* dict_places for a leaf whose dictionary is searchable in place (false: it is not) */
bool
sift_cseg_dict_places(SiftColSegment *seg, int col, SiftColOp op, const SiftColValue *c, uint32 *from, uint32 *to)
{
	DictLook   *l;

	if (seg->schema->cols[col].store != SIFT_COLS_STR || (l = seg_dict_look(seg, col)) == NULL || !l->searchable)
		return false;
	dict_places(l, op, c, from, to);
	return true;
}

/*
 * The rows of the segment whose string of leaf col passes op (EQ, LT, LE, GT, GE or PREFIX, bytewise) against c, from the cumulative counts that a dictionary
 * of the option count_columns stores with its order: two bisections and two reads, whatever the rows. false: the leaf has no counts in this segment.
 */
bool
sift_cseg_dict_count(SiftColSegment *seg, int col, SiftColOp op, const SiftColValue *c, uint64 *rows)
{
	DictLook   *l;
	uint32		from,
				to,
				cum[2];

	if (seg->schema->cols[col].store != SIFT_COLS_STR || (l = seg_dict_look(seg, col)) == NULL || l->cum_at == 0)
		return false;
	dict_places(l, op, c, &from, &to);
	cum[0] = look_cum(l, from);
	cum[1] = look_cum(l, to);
	if (cum[1] < cum[0] || cum[1] > seg->entry.ndocs)
		corrupt(seg, "The row counts of the dictionary of leaf %d are not ascending.", col);
	*rows = cum[1] - cum[0];
	return true;
}

SiftCDict *
sift_cseg_dict_search(SiftColSegment *seg, int col, SiftColOp op)
{
	SegPriv    *pv = PRIV(seg);
	DictLook   *l;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (pv->dict[col] == NULL)
		pv->dict[col] = sift_cseg_dict_peek(seg, col);
	if (pv->dict[col] != NULL)
		return pv->dict[col];
	l = seg_dict_look(seg, col);
	/* a dictionary the backend tests again and again is worth its parse, and then its hash table and sorted order, unless the cache cannot hold them: then every parse is lost; a substring test of a blocked one reads its blocks (those the signatures leave) until then, and for good where the parse is lost */
	if (l && (l->searchable || l->hash_at != 0 || (SIFT_COLOP_SCANS(op) && l->blocked && (l->sig_at != 0 || sift_cseg_dict_nokeep(seg, col)))) &&
		((l->hash_at != 0 ? sift_cseg_dict_spent(seg, col, 0) < (uint64) l->n * DICT_PARSE_COST :
		  l->bc != NULL ? l->bc->spent < (uint64) l->n * DICT_PARSE_COST : sift_cseg_dict_asked(seg, col) <= DICT_PARSE_AFTER) || sift_cseg_dict_nokeep(seg, col)))
		return &l->stub;
	return sift_cseg_dict(seg, col);
}

void
sift_cseg_dict_tsig_check(SiftColSegment *seg, int col, const SiftCDict *d)
{
	DictLook   *l = seg_dict_look(seg, col);
	Size		bytes;
	uint8	   *want,
			   *have;

	if (l == NULL || l->sig_at == 0)
		return;
	if (seg->entry.format < SIFT_COL_FORMAT_TSIG)
		corrupt(seg, "The dictionary of leaf %d has trigram signatures in a segment of column format 1.%d.", col, seg->entry.format);
	bytes = (Size) ((l->n + DICT_BLOCK - 1) / DICT_BLOCK) * ((1u << l->sig_lg) / 8);
	want = palloc(bytes);
	have = palloc(bytes);
	sift_cdict_tsig_fill(d, l->sig_lg, want);
	seg_seg_read(seg, l->sig_at, bytes, have);
	if (memcmp(want, have, bytes) != 0)
		corrupt(seg, "The trigram signatures of the dictionary of leaf %d are not those of its strings.", col);
	pfree(want);
	pfree(have);
}

bool
sift_cseg_dict_tsig(SiftColSegment *seg, int col, const char *needle, uint32 len, uint32 *passed, uint32 *total)
{
	DictLook   *l = seg->schema->cols[col].store == SIFT_COLS_STR ? seg_dict_look(seg, col) : NULL;
	SiftColValue c = {.v.s = needle,.len = len};
	uint64	   *lut;

	if (l == NULL || l->sig_at == 0)
		return false;
	lut = palloc0(SIFT_COL_NWORDS(l->n) * sizeof(uint64));
	look_contains(l, SIFT_COLOP_CONTAINS, &c, 1, lut, passed);
	*total = (l->n + DICT_BLOCK - 1) / DICT_BLOCK;
	pfree(lut);
	return true;
}

bool
sift_cseg_dict_lazy(SiftColSegment *seg, int col, uint32 want)
{
	SegPriv    *pv = PRIV(seg);
	DictLook   *l;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return false;
	if (pv->dict[col] == NULL)
		pv->dict[col] = sift_cseg_dict_peek(seg, col);
	if (pv->dict[col] != NULL || (l = seg_dict_look(seg, col)) == NULL)
		return false;
	/* the strings of a few codes cost less than the parse of all of them; the reads of a segment add up over its units */
	if (((uint64) l->reads + want) * DICT_LAZY_RATIO >= l->n)
		return false;
	l->reads += want;
	return true;
}

/* The loads that pay for the whole dictionary: its blocks decompressed one by one cost the same as the parse when they are a DICT_LAZY_LOADS-th of them */
static bool
look_lazy_left(const DictLook *l)
{
	return l->blocked && (uint64) l->loads * DICT_LAZY_LOADS < ((uint64) l->n + DICT_BLOCK - 1) / DICT_BLOCK;
}

static const char *
lazy_get(const void *gctx, uint32 code, uint32 *len, char *buf)
{
	DictLook   *l = (DictLook *) gctx;

	if (!look_lazy_left(l))
		return sift_cdict_str(sift_cseg_dict(l->seg, l->col), code, len);
	return look_code_str(l, code, len, buf);
}

void
sift_col_dict_full(const void *gctx, SiftColDict *self)
{
	const DictLook *l = gctx;
	uint64		id = self->id;

	sift_cdict_view(sift_cseg_dict(l->seg, l->col), id, self);
}

bool
sift_cseg_dict_view_lazy(SiftColSegment *seg, int col, SiftColDict *view)
{
	SegPriv    *pv = PRIV(seg);
	DictLook   *l;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return false;
	if (pv->dict[col] == NULL)
		pv->dict[col] = sift_cseg_dict_peek(seg, col);
	if (pv->dict[col] != NULL || (l = seg_dict_look(seg, col)) == NULL || !look_lazy_left(l))
		return false;
	view->n = l->n;
	view->off = NULL;
	view->bytes = NULL;
	view->order = NULL;
	view->octx = NULL;
	view->get = lazy_get;
	view->gctx = l;
	return true;
}

const char *
sift_cseg_dict_get(SiftColSegment *seg, int col, uint32 code, uint32 *len, char *buf)
{
	DictLook   *l = seg_dict_look(seg, col);

	if (l == NULL || PRIV(seg)->dict[col] != NULL)
		return sift_cdict_str(sift_cseg_dict(seg, col), code, len);
	return look_code_str(l, code, len, buf);
}
