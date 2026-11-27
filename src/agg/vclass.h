/*
 * agg/vclass.h - the class of a column a scan decodes, shared by the operators of SiftAggV6 and the storage formats that answer for it (agg/batch_source.h, core/store/ops.h).
 */
#ifndef SIFT_AGG_VCLASS_H
#define SIFT_AGG_VCLASS_H

#include "catalog/pg_type_d.h"

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

/* SiftV6InCol.attno of the found flag of connection c of a connected index (SiftConnOps, core/store/ops.h): 1 where the parent row of the key was found, NULL where it was not */
#define SIFT_V6_FOUND_ATTNO(c)	(-2 - (c))

/* the connection whose found flag the column of attno is, -1 for any other column */
static inline int
sift_v6_attno_found(int attno)
{
	return attno <= SIFT_V6_FOUND_ATTNO(0) ? SIFT_V6_FOUND_ATTNO(0) - attno : -1;
}

/* SiftV6InCol.attno of a column that is a leaf of the layout of the index, not an attribute: SIFT_V6_JLEAF_VATT + the leaf (a typed path of a document, `doc ->> 'k'`) */
#define SIFT_V6_JLEAF_VATT		(1 << 14)

/*
 * The virtual attributes (an INDEX_VAR or scan varattno above every column of an index) of what a statement reads of a key of a jsonb document whose leaf is `leaf`: its JSON type
 * (jsonb_typeof(doc -> 'key')), its `->>` text, the float8 and the bool cast of that text, whether a document has the key (`doc ? 'key'`, false for a SQL NULL document, which only an
 * aggregate FILTER reads as it reads NULL), the integer an integer leaf holds (NULL for a value it could not hold: a containment test, which no cast reads), and the integer cast of the
 * `->>` text of an integer leaf (the leaf holds the numbers whose text is an integer). An index holds at most SIFT_F2_MAX_COLS (1024) leaves, so each kind has a range of that many attributes.
 */
#define SIFT_V6_JVIRT_VATT		(3 << 13)
#define SIFT_V6_JVIRT_SPAN		1024
#define SIFT_V6_JTYPE_VATT		SIFT_V6_JVIRT_VATT
#define SIFT_V6_JTEXT_VATT		(SIFT_V6_JVIRT_VATT + SIFT_V6_JVIRT_SPAN)
#define SIFT_V6_JF8_VATT		(SIFT_V6_JVIRT_VATT + 2 * SIFT_V6_JVIRT_SPAN)
#define SIFT_V6_JB_VATT			(SIFT_V6_JVIRT_VATT + 3 * SIFT_V6_JVIRT_SPAN)
#define SIFT_V6_JPRES_VATT		(SIFT_V6_JVIRT_VATT + 4 * SIFT_V6_JVIRT_SPAN)
#define SIFT_V6_JHELD_VATT		(SIFT_V6_JVIRT_VATT + 5 * SIFT_V6_JVIRT_SPAN)
#define SIFT_V6_JINT_VATT		(SIFT_V6_JVIRT_VATT + 6 * SIFT_V6_JVIRT_SPAN)

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

/* the presence of the key: the scan reads it from the tags of the chunk of its leaf, as it reads the type */
static inline bool
sift_v6_vatt_is_presence(int attno)
{
	return attno >= SIFT_V6_JPRES_VATT && attno < SIFT_V6_JPRES_VATT + SIFT_V6_JVIRT_SPAN;
}

static inline bool
sift_v6_vatt_is_type(int attno)
{
	return (attno >= SIFT_V6_JTYPE_VATT && attno < SIFT_V6_JTEXT_VATT) || sift_v6_vatt_is_presence(attno);
}

/* the conversion of a value of an exception that raises for a text that is no number, boolean or integer (the float8, the bool and the integer cast of the `->>` text) */
static inline bool
sift_v6_vatt_raises(int attno)
{
	return (attno >= SIFT_V6_JF8_VATT && attno < SIFT_V6_JPRES_VATT) || (attno >= SIFT_V6_JINT_VATT && attno < SIFT_V6_JINT_VATT + SIFT_V6_JVIRT_SPAN);
}

static inline bool
sift_v6_vatt_is_int_cast(int attno)
{
	return attno >= SIFT_V6_JINT_VATT && attno < SIFT_V6_JINT_VATT + SIFT_V6_JVIRT_SPAN;
}

/* the first virtual attribute of what a column of a key reads: jtype with the type bool is the presence of the key, jint the integer cast of its text */
static inline int
sift_v6_jvatt_base(bool jtype, bool jint, Oid typid)
{
	if (jtype)
		return typid == BOOLOID ? SIFT_V6_JPRES_VATT : SIFT_V6_JTYPE_VATT;
	if (jint)
		return SIFT_V6_JINT_VATT;
	return typid == FLOAT8OID ? SIFT_V6_JF8_VATT : typid == BOOLOID ? SIFT_V6_JB_VATT : typid == INT8OID ? SIFT_V6_JHELD_VATT : SIFT_V6_JTEXT_VATT;
}

#endif							/* SIFT_AGG_VCLASS_H */
