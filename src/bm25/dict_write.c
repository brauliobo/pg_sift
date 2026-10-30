/*
 * bm25/dict_write.c - term dictionary writer
 * (layout in bm25/dict.h).
 */
#include "dict_int.h"

SiftDictWriter *
sift_dict_writer_begin(SiftStreamWriter *sw, bool packed, bool pos2)
{
	SiftDictWriter *w = palloc0(sizeof(SiftDictWriter));

	w->sw = sw;
	w->packed = packed;
	w->pos2 = pos2;
	w->spool = BufFileCreateTemp(false);
	w->post_end = sift_sw_tell(sw);
	w->prevlen = -1;
	w->idx = BufFileCreateTemp(false);
	w->maxroot = 64;
	w->root = palloc(w->maxroot * sizeof(uint64));
	initStringInfo(&w->blk);
	return w;
}

static void
flush_block(SiftDictWriter *w)
{
	uint8		hdr[BLOCK_HDR_MAX];
	uint8	   *p = sift_put_varint(hdr + 3, w->blk_post);
	uint32		len = (p - hdr) + w->blk.len;

	if (w->nblk == 0)
		return;
	hdr[0] = len & 0xff;
	hdr[1] = len >> 8;
	hdr[2] = w->nblk | (w->packed ? SIFT_DICT_PACKED : 0) | (w->pos2 ? SIFT_DICT_POS2 : 0);
	BufFileWrite(w->spool, hdr, p - hdr);
	BufFileWrite(w->spool, w->blk.data, w->blk.len);
	w->dict_len += len;
	resetStringInfo(&w->blk);
	w->nblk = 0;
}

/*
 * One entry of a block, appended to b. Format 2 and before: u8 shared, u8 suffix length, suffix, varint df, u8 flags, u8 max_bound.
 * Packed (segment format 3): u8 shared | (suffix length - 1) << 4 (a nibble of 15 says a byte follows with the value), suffix,
 * u8 flags | min(df, 15) << 4 (15: varint df - 15 follows), u8 max_bound; the inline keys as packed_key
 */
static void
put_entry(StringInfo b, const char *term, int len, int shared, const SiftTermInfo *info, bool packed)
{
	uint8		e[4 + SIFT_MAX_TERM_BYTES + 64];
	uint8	   *p = e;
	bool		inl = (info->flags & SIFT_TERM_INLINE) != 0;
	int			suffix = len - shared;

	if (packed)
	{
		*p++ = Min(shared, 15) | Min(suffix - 1, 15) << 4;
		if (shared >= 15)
			*p++ = shared;
		if (suffix - 1 >= 15)
			*p++ = suffix - 1;
		memcpy(p, term + shared, suffix);
		p += suffix;
		*p++ = (info->flags & ~SIFT_TERM_POS2) | Min(info->df, 15) << 4;
		if (info->df >= 15)
			p = sift_put_varint(p, info->df - 15);
		*p++ = info->max_bound;
	}
	else
	{
		*p++ = shared;
		*p++ = suffix;
		memcpy(p, term + shared, suffix);
		p += suffix;
		p = sift_put_varint(p, info->df);
		*p++ = info->flags;
		*p++ = info->max_bound;
	}
	if (inl)
	{
		SiftKey		k0 = info->inline_keys[0],
					k1 = info->inline_keys[1];

		if (packed)
		{
			p = sift_put_varint(p, sift_dict_pack_key(k0));
			if (info->df > 1)
				p = sift_put_varint(p, sift_dict_pack_key(k1) - sift_dict_pack_key(k0));
		}
		else
		{
			p = sift_put_varint(p, k0);
			if (info->df > 1)
				p = sift_put_varint(p, k1 - k0);
		}
		if (!(info->flags & SIFT_TERM_ALL_TF1))
			for (uint32 i = 0; i < info->df; i++)
				*p++ = info->inline_tf[i];
		if (info->flags & SIFT_TERM_POS)
		{
			p = sift_put_varint(p, info->pos_len);
			appendBinaryStringInfo(b, (const char *) e, p - e);
			p = e;
			appendBinaryStringInfo(b, (const char *) info->inline_pos, info->pos_len);
		}
	}
	else
	{
		p = sift_put_varint(p, info->post_len);
		p = sift_put_varint(p, info->post_len - info->codec_len);
		if (!(info->flags & SIFT_TERM_ALL_TF1))
			p = sift_put_varint(p, info->nnib);
		if (info->flags & SIFT_TERM_POS)
			p = sift_put_varint(p, info->pos_len);
	}
	appendBinaryStringInfo(b, (const char *) e, p - e);
}

void
sift_dict_add(SiftDictWriter *w, const char *term, int len, const SiftTermInfo *info)
{
	int			shared = 0;
	bool		inl = (info->flags & SIFT_TERM_INLINE) != 0;

	SIFT_INVARIANT(len >= 1 && len <= SIFT_MAX_TERM_BYTES &&
				   (w->prevlen < 0 || sift_term_cmp(w->prev, w->prevlen, term, len) < 0),
				   "sift: dictionary terms must be non-empty, at most %d bytes and strictly ascending", SIFT_MAX_TERM_BYTES);
	SIFT_INVARIANT(inl || info->post_off == ALIGN4(w->post_end), "sift: term blob at %llu, expected %llu",
				   (unsigned long long) info->post_off, (unsigned long long) ALIGN4(w->post_end));
	if (w->nblk == 0)
	{
		SiftDictIdx e = {prefix8(term, len), (uint32) w->dict_len, w->nslots};

		if (w->dict_len > SIFT_DICT_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift: segment dictionary exceeds 4 GB"),
					 errhint("Partition the table so that each index holds fewer distinct terms.")));
		if (w->nidx % SIFT_DICT_ROOT_FANOUT == 0)
		{
			if (w->nidx / SIFT_DICT_ROOT_FANOUT == w->maxroot)
			{
				w->maxroot *= 2;
				w->root = repalloc_huge(w->root, w->maxroot * sizeof(uint64));
			}
			w->root[w->nidx / SIFT_DICT_ROOT_FANOUT] = e.prefix;
		}
		BufFileWrite(w->idx, &e, sizeof(e));
		w->nidx++;
		w->blk_post = ALIGN4(w->post_end);
	}
	else
		while (shared < Min(len, w->prevlen) && term[shared] == w->prev[shared])
			shared++;
	put_entry(&w->blk, term, len, shared, info, w->packed);
	if (!inl)
		w->post_end = info->post_off + info->post_len;
	memcpy(w->prev, term, len);
	w->prevlen = len;
	w->nterms++;
	w->nslots += !inl;
	if (++w->nblk == SIFT_DICT_BLOCK)
		flush_block(w);
}

uint32
sift_dict_writer_nslots(const SiftDictWriter *w)
{
	return w->nslots;
}

void
sift_dict_writer_finish(SiftDictWriter *w, SiftSegHeaderData *hdr)
{
	flush_block(w);
	hdr->nterms = w->nterms;
	hdr->sections[SIFT_SEC_DICT].off = sift_sw_tell(w->sw);
	hdr->sections[SIFT_SEC_DICT].len = w->dict_len;
	sift_sw_write_file(w->sw, w->spool, w->dict_len);
	BufFileClose(w->spool);
	hdr->sections[SIFT_SEC_DICTIDX].off = sift_sw_tell(w->sw);
	hdr->sections[SIFT_SEC_DICTIDX].len = w->nidx * sizeof(SiftDictIdx);
	sift_sw_write_file(w->sw, w->idx, w->nidx * sizeof(SiftDictIdx));
	BufFileClose(w->idx);
	hdr->dictroot_off = sift_sw_tell(w->sw);
	hdr->dictroot_n = (w->nidx + SIFT_DICT_ROOT_FANOUT - 1) / SIFT_DICT_ROOT_FANOUT;
	sift_sw_write(w->sw, w->root, hdr->dictroot_n * sizeof(uint64));
	pfree(w->root);
	pfree(w->blk.data);
}
