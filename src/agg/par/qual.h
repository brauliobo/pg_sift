/*
 * agg/par/qual.h - a qual of the statement that no scan key says (SiftV6Spec.vqual) as the compute threads run it: a column of a sift_table against a constant, an integer by any comparison, a text by
 * = and <> or by LIKE and NOT LIKE of a literal (agg/text_qual.h). A thread clears the rows of a granule that fail it from the columns par_load decoded, in pure C.
 */
#ifndef SIFT_AGG_V6_PAR_QUAL_H
#define SIFT_AGG_V6_PAR_QUAL_H

#include "postgres.h"

#include "agg/text_qual.h"

#define PAR_QUALS_MAX	8

typedef enum ParQualKind
{
	PQK_INT,
	PQK_TEXT
} ParQualKind;

typedef enum ParQualOp
{
	PQO_EQ,
	PQO_NE,
	PQO_LT,
	PQO_LE,
	PQO_GT,
	PQO_GE
} ParQualOp;

typedef struct ParQual
{
	ParQualKind kind;
	int			at;				/* the leaf of the shape the qual reads */
	ParQualOp	op;
	int64		ival;
	SiftTextQual text;
} ParQual;

#endif							/* SIFT_AGG_V6_PAR_QUAL_H */
