/*
 * col/shred_pg.h - the PostgreSQL driver of the shredder: walks a jsonb with the iterator and feeds col/shred.h its events.
 */
#ifndef SIFT_SHRED_PG_H
#define SIFT_SHRED_PG_H

#include "utils/jsonb.h"

#include "col/shred.h"

/* One jsonb as the next value of the shredder (a container, or the scalar of a raw scalar jsonb): for a caller that sends the
 * events around it itself, e.g. the object {attname: value} of the attributes of a tuple */
extern void sift_shred_container(SiftShred *sh, JsonbContainer *c);

/* A whole document: sift_shred_begin, the value, sift_shred_finish */
extern void sift_shred_jsonb(SiftShred *sh, Jsonb *jb);

/*
 * The recheck of a row: a row whose document holds a value a typed leaf cannot store is shredded again as the SQL
 * of the query reads it. How a column or a group is read comes from the query (the planner); the walk reads the value the way that
 * expression does and gives the shredder the result, or records the error SQL raises for it (sift_shred_recheck_error).
 */
typedef enum SiftRecheckCol
{
	SRC_OFF,					/* as the shredder does: a value the leaf cannot store is an overflow slot */
	SRC_JSONCAST,				/* the value of a jsonb that the query casts to the leaf's number type (a jsonb_each member): a number as the cast gives it, any other type is the error the cast raises */
	SRC_MEMBER,					/* the value column of a map of which the query casts the member of some keys (SiftRecheckMember) as SRC_JSONCAST does: the other members are read as the shredder does, and no error */
	SRC_TEXTIO					/* the text of the scalar (a string as it is, a number as numeric_out prints it, true or false, a container as its JSON) through the input function of the leaf's type, as a record column of jsonb_to_recordset and (x->>'k')::type do */
} SiftRecheckCol;

typedef enum SiftRecheckGroup
{
	SRG_OFF,
	SRG_RECORDSET				/* jsonb_to_recordset over the list: its elements are objects and its columns are read in one call, so a value that raises poisons the whole list of the row */
} SiftRecheckGroup;

typedef struct SiftRecheckMember
{
	int			col;			/* the value column of the map */
	char	   *key;
	int			keylen;
	Oid			cast;			/* the type the cast gives (float8, int4 or int8): the error names it */
} SiftRecheckMember;

typedef struct SiftShredRecheck
{
	const SiftShredSpec *spec;
	uint8	   *col;			/* [spec->ncols] SiftRecheckCol */
	uint8	   *grp;			/* [spec->ngroups] SiftRecheckGroup */
	FmgrInfo   *in;				/* [spec->ncols] the input function of a bool, integer or float column */
	MemoryContext mcxt;			/* the error is copied here */
	ErrorData  *err;			/* the first error SQL raises for the document being walked (NULL: none) */
	int			poison_grp;		/* the value just read raised in a group read as a whole (SRG_RECORDSET): its list is OVERFLOW; -1: none */
	SiftRecheckMember *mem;		/* SRC_MEMBER columns: the keys read */
	int			nmem;
	const char *key;			/* the key of the member being walked, set by the walk (not NUL terminated) */
	int			keylen;
} SiftShredRecheck;

extern SiftShredRecheck *sift_shred_recheck_create(MemoryContext mcxt, const SiftShredSpec *spec);
/* sift_shred_container with the recheck: rc->err is reset by the caller (sift_shred_recheck_begin) */
extern void sift_shred_container_rc(SiftShred *sh, JsonbContainer *c, SiftShredRecheck *rc);
extern void sift_shred_recheck_begin(SiftShredRecheck *rc);
/* column col is read as the cast of the member of key to the type cast */
extern void sift_shred_recheck_member(SiftShredRecheck *rc, int col, const char *key, Oid cast);

/* The walk's hooks (col/shred_recheck.c): the value is consumed (true) or left to the plain walk (false); settle runs after either */
extern bool sift_shred_recheck_value(SiftShred *sh, const JsonbValue *v, SiftShredRecheck *rc);
extern void sift_shred_recheck_settle(SiftShred *sh, SiftShredRecheck *rc);

#endif							/* SIFT_SHRED_PG_H */
