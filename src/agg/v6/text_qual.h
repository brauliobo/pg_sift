/*
 * agg/v6/text_qual.h - a qual of a text column against a literal that is decided on the bytes of the string: = and <>, LIKE and NOT LIKE of '%lit%', 'lit%', '%lit' and 'lit', under a collation
 * whose equal strings are equal bytes. The serial batch quals (v6_bqual.c) and the compute threads (par/qual.c) run it the same way, on the strings they decoded.
 */
#ifndef SIFT_AGG_V6_TEXT_QUAL_H
#define SIFT_AGG_V6_TEXT_QUAL_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "col/like_simd.h"

typedef struct SiftTextQual
{
	bool		like;			/* LIKE, NOT LIKE; else = and <> */
	bool		negate;			/* <> and NOT LIKE */
	SiftLikeShape shape;
	const uint8 *bytes;			/* the text constant, or the literal of the pattern */
	uint32		len;
	SiftLikeNeedle needle;		/* the literal of a contains pattern, over bytes */
} SiftTextQual;

/* clause is `column op 'literal'` over a text or varchar column of relid (the literal is not NULL): its qual, in the current memory context, and the column */
extern bool sift_text_qual_of(const Node *clause, Index relid, SiftTextQual *q, const Var **column);

/* the string p[0 .. len - 1] passes (a NULL string passes no qual: the caller drops it) */
extern bool sift_text_qual_holds(const SiftTextQual *q, const uint8 *p, uint32 len);

#endif							/* SIFT_AGG_V6_TEXT_QUAL_H */
