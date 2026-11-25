/*
 * agg/vclass.h - the class of a column a scan decodes, shared by the operators of SiftAggV6 and the storage formats that answer for it (agg/batch_source.h, core/store/ops.h).
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

/*
 * The virtual attributes (an INDEX_VAR or scan varattno above every column of an index) of what a statement reads of a key of a jsonb document whose leaf is `leaf`: its JSON type
 * (jsonb_typeof(doc -> 'key')), its `->>` text, and the float8 cast of that text. An index holds at most SIFT_F2_MAX_COLS (1024) leaves, so each kind has a range of that many attributes.
 */
#define SIFT_V6_JVIRT_VATT		(3 << 13)
#define SIFT_V6_JVIRT_SPAN		1024
#define SIFT_V6_JTYPE_VATT		SIFT_V6_JVIRT_VATT
#define SIFT_V6_JTEXT_VATT		(SIFT_V6_JVIRT_VATT + SIFT_V6_JVIRT_SPAN)
#define SIFT_V6_JF8_VATT		(SIFT_V6_JVIRT_VATT + 2 * SIFT_V6_JVIRT_SPAN)

static inline bool
sift_v6_vatt_is_json(int attno)
{
	return attno >= SIFT_V6_JVIRT_VATT;
}

static inline int
sift_v6_vatt_leaf(int attno)
{
	return (attno - SIFT_V6_JVIRT_VATT) % SIFT_V6_JVIRT_SPAN;
}

static inline bool
sift_v6_vatt_is_type(int attno)
{
	return attno >= SIFT_V6_JTYPE_VATT && attno < SIFT_V6_JTEXT_VATT;
}

#endif							/* SIFT_AGG_VCLASS_H */
