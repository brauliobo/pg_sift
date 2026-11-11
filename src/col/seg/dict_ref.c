/*
 * col/seg/dict_ref.c - the dictionaries of a segment that refer to a shared value store (column format minor 46, docs/COL-VSTORE.md):
 * the load, the string of a code, the match through the store, the codes a leaf names and the epoch they name
 */
#include "col/compress.h"
#include "col/seg_int.h"

/* A dictionary of column format minor 46 that is a reference to the shared value store of the leaf's column (col/vstore/vstore_ref.c) */
SiftCDict *
seg_dict_ref_load(SiftColSegment *seg, int col, uint64 at, uint64 len, MemoryContext mcxt)
{
	char	   *data = palloc(len);
	SiftCDict  *d;

	seg_read(seg, at, len, data);
	d = sift_cref_host == NULL || len < DICT_REF_WORDS * sizeof(uint32) ? NULL : sift_cref_host->parse(mcxt, ((uint32 *) data)[3], data, len);
	if (d == NULL)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	pfree(data);
	return d;
}

/* The dictionary of leaf col read from the segment into mcxt (the loader of the backend's cache, col/dictcache.c) */

/* The string of a code of a dictionary that refers to a shared value store: the code of the store that the segment holds for it, then the string from the runs of the store (col/vstore/) */
const char *
seg_ref_code_str(DictLook *l, uint32 code, uint32 *len, char *buf)
{
	uint64		bit = (uint64) code * l->ref_bits;
	uint8		raw[sizeof(uint64) + 1] = {0};
	uint32		at;

	if (l->ref == NULL)
		l->ref = sift_cref_host->open(l->seg->index, l->ref_store, l->ref_epoch, l->seg->mcxt);
	seg_read(l->seg, l->refs_at + (bit >> 3), Min(sizeof(raw), (l->hash_at - l->refs_at) - (bit >> 3)), raw);
	at = sift_ckeyed_get(raw, sizeof(raw), bit & 7, l->ref_bits);
	l->loads++;
	return sift_cref_host->str(l->ref, at, len, buf);
}

/* The string of a code of a dictionary in the stream, into buf (SIFT_COL_DICT_MAX_ENTRY bytes) */

/*
 * A dictionary that refers to a shared value store: the store finds the codes of its strings that pass the test (each run is searched in place, in byte order or block
 * by block), and the segment's own codes are those whose code of the store is one of them
 */
static uint32
look_match_ref(const SiftCDictLook *look, SiftColOp op, const SiftColValue *c, int nvals, uint64 *lut)
{
	DictLook   *l = (DictLook *) look;
	uint64		total,
				nbytes = sift_cdict_perm_bytes(l->n, l->ref_bits);
	uint64	   *hit;
	uint8	   *packed = palloc(nbytes + sizeof(uint64));
	uint32		count = 0;

	if (l->ref == NULL)
		l->ref = sift_cref_host->open(l->seg->index, l->ref_store, l->ref_epoch, l->seg->mcxt);
	total = sift_cref_host->ncodes(l->ref);
	/* check-memory: allow one bit a code of the store: an eighth of a byte a string */
	hit = palloc0((total / 64 + 1) * sizeof(uint64));
	sift_cref_host->match(l->ref, op, c, nvals, hit);
	memset(packed + nbytes, 0, sizeof(uint64));
	seg_read(l->seg, l->refs_at, nbytes, packed);
	for (uint32 i = 0; i < l->n; i++)
	{
		uint32		g = sift_ckeyed_get(packed, nbytes + sizeof(uint64), (uint64) i * l->ref_bits, l->ref_bits);

		if (g < total && (hit[g >> 6] >> (g & 63) & 1))
		{
			lut[i >> 6] |= UINT64CONST(1) << (i & 63);
			count++;
		}
	}
	pfree(packed);
	pfree(hit);
	return count;
}

/* The dictionary of form DICT_REF (col/enc.h): five words, the form, n, the bits of a code of the store, the store and its epoch, the codes of the store packed, then the hash of the strings */
DictLook *
seg_look_ref(SiftColSegment *seg, int col, uint64 off, uint64 len)
{
	uint32		head[DICT_REF_WORDS];
	DictLook   *l = seg_look_alloc(seg);

	if (len < sizeof(head))
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	seg_read(seg, off, sizeof(head), head);
	l->seg = seg;
	l->col = col;
	l->n = head[1];
	l->ref_bits = head[2];
	l->ref_store = head[3];
	l->ref_epoch = head[4];
	l->refs_at = off + sizeof(head);
	l->hash_at = l->refs_at + sift_cdict_perm_bytes(l->n, l->ref_bits);
	if (l->n == 0 || l->ref_bits < 1 || l->ref_bits > 32 || l->ref_store == 0 ||
		l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32) > off + len)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	l->look = (SiftCDictLook) {seg_look_find, look_match_ref};
	l->stub.n = l->n;
	l->stub.look = &l->look;
	return l;
}

/* The codes of the store that the dictionary of leaf col names, in the order of the codes of the segment (NULL when it is not a reference); *n of them */
uint32 *
sift_cseg_ref_codes(SiftColSegment *seg, int col, uint32 *store, uint32 *epoch, uint32 *n)
{
	uint64		off,
				len,
				at,
				nbytes;
	uint32		head[DICT_REF_WORDS];
	uint8	   *packed;
	uint32	   *codes;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	seg_dict_region(seg, col, &off, &len);
	at = seg_sec(seg, SIFT_SEC_DICT)->off + off;
	if (len < sizeof(head))
		return NULL;
	seg_read(seg, at, sizeof(head), head);
	if (head[0] != DICT_REF)
		return NULL;
	*n = head[1];
	*store = head[3];
	*epoch = head[4];
	nbytes = sift_cdict_perm_bytes(head[1], head[2]);
	if (head[1] == 0 || head[2] < 1 || head[2] > 32 || sizeof(head) + nbytes > len)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	packed = palloc0(nbytes + sizeof(uint64));
	seg_read(seg, at + sizeof(head), nbytes, packed);
	codes = palloc(head[1] * sizeof(uint32));
	for (uint32 i = 0; i < head[1]; i++)
		codes[i] = sift_ckeyed_get(packed, nbytes + sizeof(uint64), (uint64) i * head[2], head[2]);
	pfree(packed);
	return codes;
}

/* Whether a leaf of the segment refers to an epoch of its shared value store that is not the store's current one, or to the store at all when the leaf is left[c] (NULL: none is) */
bool
sift_cseg_names_old_epoch(SiftColSegment *seg, const bool *left)
{
	for (int c = 0; sift_cref_host != NULL && c < seg->schema->ncols; c++)
	{
		uint64		off,
					len;
		uint32		head[DICT_REF_WORDS];

		if (seg->schema->cols[c].store != SIFT_COLS_STR)
			continue;
		seg_dict_region(seg, c, &off, &len);
		if (len < sizeof(head))
			continue;
		seg_read(seg, seg_sec(seg, SIFT_SEC_DICT)->off + off, sizeof(head), head);
		if (head[0] == DICT_REF && ((left != NULL && left[c]) || sift_cref_host->epoch(head[3]) != head[4]))
			return true;
	}
	return false;
}

/*
 * The strings of cnt consecutive codes from code, appended to all with their offsets in off[0..cnt): a dictionary in blocks gives those of one block with one
 * copy (cnt strings of one block at most), the other forms one string at a time
 */
void
seg_look_strs(DictLook *l, uint32 code, uint32 cnt, StringInfo all, uint32 *off)
{
	uint32		i = code % DICT_BLOCK;

	if (l->blocked && i + cnt <= DICT_BLOCK && code + cnt <= l->n)
	{
		if (l->blkno != code / DICT_BLOCK)
			sift_cseg_look_block(l, code / DICT_BLOCK);
		for (uint32 j = 0; j < cnt; j++)
			off[j] = (uint32) all->len + l->blkoff[i + j] - l->blkoff[i];
		appendBinaryStringInfo(all, (const char *) l->blkraw + l->blkoff[i], l->blkoff[i + cnt] - l->blkoff[i]);
		return;
	}
	for (uint32 j = 0; j < cnt; j++)
	{
		uint32		len;
		const char *s = sift_cseg_look_str(l, code + j, &len, l->buf);

		off[j] = (uint32) all->len;
		appendBinaryStringInfo(all, s, len);
	}
}
