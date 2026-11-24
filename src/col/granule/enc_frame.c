/*
 * col/granule/enc_frame.c - the framed chunks (SIFT_ENC_FRAMED, col/granule/granule.h): the frames of 256 values a writer cuts a STR_BLOCK or FLOAT_BSS chunk of a sift_table part into, and the
 * readers of a range of values or of one value that inflate only the frames they touch.
 */
#include "postgres.h"

#include "col/codec.h"
#include "col/compress.h"
#include "col/granule/granule.h"
#include "col/granule/granule_int.h"
#include "core/par/sbuf.h"
#include "core/damage.h"

#define FRAME_HDR_BYTES		8

void
sift_frames_put(StringInfo s, int nn, SiftFramePut put, void *arg)
{
	SiftChunkFrameHdr hdr = {(uint32) ((nn + SIFT_FRAME_ROWS - 1) / SIFT_FRAME_ROWS), SIFT_FRAME_ROWS};
	uint32	   *off = sift_alloc0((hdr.nframes + 1) * sizeof(uint32));
	StringInfoData frames;
	uint32		table = MAXALIGN((hdr.nframes + 1) * sizeof(uint32));

	sift_sb_init(&frames);
	for (uint32 f = 0; f < hdr.nframes; f++)
	{
		put(&frames, (int) (f * SIFT_FRAME_ROWS), Min(SIFT_FRAME_ROWS, nn - (int) (f * SIFT_FRAME_ROWS)), arg);
		pad8(&frames);
		off[f + 1] = frames.len;
	}
	sift_sb_append(s, (char *) &hdr, sizeof(hdr));
	sift_sb_append(s, (char *) off, (hdr.nframes + 1) * sizeof(uint32));
	sift_sb_spaces(s, table - (hdr.nframes + 1) * sizeof(uint32));
	sift_sb_append(s, frames.data, frames.len);
	sift_free(off);
	sift_free(frames.data);
}

bool
sift_chunk_framed(const SiftChunk *c)
{
	return (c->ref->enc & SIFT_ENC_FRAMED) != 0;
}

/* Frame f of the chunk: its bytes and their length, and how many values it holds */
static const char *
frame_at(const SiftChunk *c, uint32 f, uint32 *len, int *cnt)
{
	SiftChunkFrameHdr hdr;
	uint32		lo,
				hi,
				table;

	if (c->body_len < FRAME_HDR_BYTES)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a framed chunk is shorter than its header"));
	memcpy(&hdr, c->body, sizeof(hdr));
	if (hdr.frame_rows != SIFT_FRAME_ROWS || hdr.nframes != (uint32) ((c->nnonnull + SIFT_FRAME_ROWS - 1) / SIFT_FRAME_ROWS) || f >= hdr.nframes)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a framed chunk of %d values has %u frames of %u", c->nnonnull, hdr.nframes, hdr.frame_rows));
	table = FRAME_HDR_BYTES + MAXALIGN((hdr.nframes + 1) * sizeof(uint32));
	if (c->body_len < table)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a framed chunk is shorter than its table of frames"));
	memcpy(&lo, c->body + FRAME_HDR_BYTES + f * sizeof(uint32), 4);
	memcpy(&hi, c->body + FRAME_HDR_BYTES + (f + 1) * sizeof(uint32), 4);
	if (lo > hi || hi > c->body_len - table)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a frame of a framed chunk is past its end"));
	*len = hi - lo;
	*cnt = Min(SIFT_FRAME_ROWS, c->nnonnull - (int) (f * SIFT_FRAME_ROWS));
	return c->body + table + lo;
}

void
sift_frame_strs(const SiftChunk *c, uint32 f, uint32 **off, char **bytes)
{
	uint32		avail,
				len;
	int			cnt;
	const char *p = frame_at(c, f, &avail, &cnt);

	if (avail < 8)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a frame of strings is shorter than its length"));
	memcpy(&len, p, 4);
	if (len > avail - 8)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a frame of strings of %u bytes does not fit its chunk of %u", len, avail));
	sift_cc_decode_strs_dict(CurrentMemoryContext, (const uint8 *) p + 8, len, cnt, cnt, 0, off, bytes, NULL);
}

void
sift_frames_strs(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len, void **bytes_out)
{
	const uint32 f0 = k0 / SIFT_FRAME_ROWS,
				f1 = (k1 - 1) / SIFT_FRAME_ROWS;
	uint32	  **offs = palloc((f1 - f0 + 1) * sizeof(uint32 *));
	char	  **bytes = palloc((f1 - f0 + 1) * sizeof(char *));
	uint64		total = 0;
	char	   *all;

	if (k1 <= k0)
		return;
	for (uint32 f = f0; f <= f1; f++)
	{
		sift_frame_strs(c, f, &offs[f - f0], &bytes[f - f0]);
		total += offs[f - f0][Min(SIFT_FRAME_ROWS, c->nnonnull - (int) (f * SIFT_FRAME_ROWS))];
	}
	all = palloc(Max(total, 1));
	for (uint32 f = f0, at = 0; f <= f1; f++)
	{
		const int	base = (int) (f * SIFT_FRAME_ROWS),
					cnt = Min(SIFT_FRAME_ROWS, c->nnonnull - base);
		const uint32 *o = offs[f - f0];

		memcpy(all + at, bytes[f - f0], o[cnt]);
		for (int k = Max(k0 - base, 0); k < Min(k1 - base, cnt); k++)
		{
			p[base + k - k0] = all + at + o[k];
			len[base + k - k0] = o[k + 1] - o[k];
		}
		at += o[cnt];
		pfree(offs[f - f0]);
		pfree(bytes[f - f0]);
	}
	pfree(offs);
	pfree(bytes);
	if (bytes_out != NULL)
		*bytes_out = all;
}

void
sift_frames_block_strs(const SiftChunk *c, uint32 **offs, char **bytes)
{
	const int	nn = c->nnonnull;
	const char **p = palloc(Max(nn, 1) * sizeof(char *));
	uint32	   *len = palloc(Max(nn, 1) * sizeof(uint32));
	uint32	   *o = palloc((nn + 1) * sizeof(uint32));
	uint64		total = 0;
	void	   *all = NULL;

	if (nn > 0)
		sift_frames_strs(c, 0, nn, p, len, &all);
	for (int k = 0; k < nn; k++)
		total += len[k];
	*bytes = palloc(Max(total, 1));
	for (int k = 0, at = 0; k < nn; k++)
	{
		o[k] = at;
		memcpy(*bytes + at, p[k], len[k]);
		at += len[k];
	}
	o[nn] = (uint32) total;
	*offs = o;
	if (all != NULL)
		pfree(all);
	pfree(p);
	pfree(len);
}

void
sift_frame_f8_put(StringInfo s, const double *vals, int cnt, int alg)
{
	uint8	   *planes = sift_alloc((Size) cnt * 8);
	uint32		hdr[3] = {0, (uint32) cnt * 8, (uint32) cnt * 8};
	Size		cap = alg > 0 ? sift_colz_bound(alg, (Size) cnt * 8) : 0,
				clen = 0;
	char	   *buf = cap > 0 ? sift_alloc(cap) : NULL;

	for (int i = 0; i < cnt; i++)
		for (int b = 0; b < 8; b++)
			planes[b * cnt + i] = ((const uint8 *) &vals[i])[b];
	if (buf != NULL)
		clen = sift_colz_compress(alg, 0, planes, (Size) cnt * 8, buf, cap);
	if (clen != 0 && clen < (Size) cnt * 8)
	{
		hdr[0] = alg;
		hdr[2] = (uint32) clen;
		sift_sb_append(s, (char *) hdr, sizeof(hdr));
		sift_sb_append(s, buf, clen);
	}
	else
	{
		sift_sb_append(s, (char *) hdr, sizeof(hdr));
		sift_sb_append(s, (char *) planes, (Size) cnt * 8);
	}
	sift_free(planes);
	if (buf != NULL)
		sift_free(buf);
}

void
sift_frames_f8(const SiftChunk *c, int k0, int k1, double *out)
{
	if (k1 <= k0)
		return;
	for (uint32 f = k0 / SIFT_FRAME_ROWS; f <= (uint32) ((k1 - 1) / SIFT_FRAME_ROWS); f++)
	{
		uint32		len,
					hdr[3];
		int			cnt;
		const char *p = frame_at(c, f, &len, &cnt);
		const int	base = (int) (f * SIFT_FRAME_ROWS);
		uint8	   *planes;

		if (len < sizeof(hdr))
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a frame of doubles is shorter than its header"));
		memcpy(hdr, p, sizeof(hdr));
		if (hdr[1] != (uint32) cnt * 8 || hdr[2] > len - sizeof(hdr) || (hdr[0] == 0 && hdr[2] != hdr[1]))
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a frame of %d doubles claims %u bytes in %u", cnt, hdr[1], hdr[2]));
		planes = palloc(hdr[1] + 8);
		if (hdr[0] == 0)
			memcpy(planes, p + sizeof(hdr), hdr[1]);
		else if (!sift_colz_available(hdr[0]) || !sift_colz_decompress(hdr[0], p + sizeof(hdr), hdr[2], planes, hdr[1]))
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a frame of doubles does not inflate to its length"));
		for (int i = Max(k0 - base, 0); i < Min(k1 - base, cnt); i++)
			for (int b = 0; b < 8; b++)
				((uint8 *) &out[base + i - k0])[b] = planes[b * cnt + i];
		pfree(planes);
	}
}
