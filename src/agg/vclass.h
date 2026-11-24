/*
 * agg/vclass.h - the class of a column a scan decodes, shared by the operators of SiftAggV6 and the storage formats that answer for it (agg/batch_source.h, core/store_ops.h).
 */
#ifndef SIFT_AGG_VCLASS_H
#define SIFT_AGG_VCLASS_H

/* The physical class of a column the scan reads: how it decodes (SiftVecType) and what the group makes of it */
typedef enum SiftV6Class
{
	SV6C_NONE = 0,
	SV6C_I32,					/* int2, int4, date */
	SV6C_I64,					/* int8, timestamp, timestamptz */
	SV6C_F64,					/* float4, float8 */
	SV6C_BOOL,
	SV6C_TEXT,					/* text, varchar */
	SV6C_DATUM					/* arbitrary PostgreSQL type decoded into SVEC_DATUM */
} SiftV6Class;

/* SiftV6InCol.attno and SiftV6Col.attno of sift.score(ctid), the score of the row's hit: no column of the index; a source given a text bridge fills it as an F64 vector */
#define SIFT_V6_SCORE_ATTNO		(-1)

/* SiftV6InCol.attno of a column that is a leaf of the layout of the index, not an attribute: SIFT_V6_JLEAF_VATT + the leaf (a typed path of a document, `doc ->> 'k'`) */
#define SIFT_V6_JLEAF_VATT		(1 << 14)

#endif							/* SIFT_AGG_VCLASS_H */
