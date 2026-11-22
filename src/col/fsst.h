/*
 * col/fsst.h - a static symbol table for strings (FSST): up to 255 symbols of 1 to 8 bytes, a string coded one byte per symbol and the escape code (255) before a byte no symbol covers.
 * Each string is coded apart, so a string is decoded alone and the codes of a run of strings are decoded in one pass over their bytes.
 */
#ifndef SIFT_FSST_H
#define SIFT_FSST_H

#include "core/host.h"

#define SIFT_FSST_MAX_SYMBOLS	255
#define SIFT_FSST_ESCAPE		255
#define SIFT_FSST_SYMBOL_BYTES	8
#define SIFT_FSST_SLACK			8		/* what a decode may write past the last byte it returns, and an input is read one byte past its end */
#define SIFT_FSST_SAMPLE_BYTES	16384

typedef struct SiftFsst
{
	uint32		nsym;
	uint8		len[256];				/* of each symbol; the escape copies one byte */
	uint8		sym[256][SIFT_FSST_SYMBOL_BYTES];	/* zero padded; the escape's is all zero */
	uint64		word[256];				/* sym[c] as a word, and its mask: what the encoder compares */
	uint64		mask[256];
	uint16		first[257];				/* the symbols that start with byte b are order[first[b] .. first[b + 1]), the longest first */
	uint8		order[SIFT_FSST_MAX_SYMBOLS];
} SiftFsst;

/* The table the strings p[i] (len[i] bytes, n of them) train: a sample of SIFT_FSST_SAMPLE_BYTES of them, five generations of the greedy choice by gain. Returns the sample's coded size in per mille of its raw size */
extern uint32 sift_fsst_train(SiftFsst *t, const char *const *p, const uint32 *len, uint32 n);

/* One string coded into out (room for 2 * len bytes); returns the coded length */
extern uint32 sift_fsst_encode(const SiftFsst *t, const char *s, uint32 len, uint8 *out);

/*
 * The coded bytes every string that starts with prefix begins with: the codes of the prefix up to the first byte at which a symbol longer than what is left of the prefix could start, which the
 * bytes after the prefix might complete. out has room for 2 * plen bytes; returns the coded length, and *raw_len the bytes of the prefix they stand for (the rest, at most 7, is not coded here).
 */
extern uint32 sift_fsst_encode_safe_prefix(const SiftFsst *t, const char *prefix, uint32 plen, uint8 *out, uint32 *raw_len);

/* The table as stored (the lengths of the symbols, then their bytes, padded to 8) appended to s; and read back from p: the bytes it took, 0 when p is no table */
extern void sift_fsst_put_table(StringInfo s, const SiftFsst *t);
extern uint32 sift_fsst_read_table(SiftFsst *t, const uint8 *p, uint32 avail);

/*
 * The n strings coded at enc, string i in [off[i], off[i + 1]) of it (enc is readable one byte past off[n]): written one after the other to out (cap bytes, which holds the
 * SIFT_FSST_SLACK past their end) with their ends in outoff (n + 1 entries). false when a string is cut short in its escape or the output does not fit cap.
 */
extern bool sift_fsst_decode_strs(const SiftFsst *t, const uint8 *enc, const uint32 *off, uint32 n, uint8 *out, uint64 cap, uint32 *outoff);

/* One string decoded: out has room for 8 * inlen + SIFT_FSST_SLACK bytes; returns its length, or UINT32_MAX when it ends inside an escape */
extern uint32 sift_fsst_decode(const SiftFsst *t, const uint8 *in, uint32 inlen, uint8 *out);

#endif							/* SIFT_FSST_H */
