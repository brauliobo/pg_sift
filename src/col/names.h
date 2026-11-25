/*
 * col/names.h - a name that changes, and the text of the options of a sift_col index that has to say it (col/names.c finds what to change, col/names_text.c writes the text)
 */
#ifndef SIFT_COL_NAMES_H
#define SIFT_COL_NAMES_H

#include "utils/rel.h"

typedef enum SiftNameKind
{
	SIFT_NAME_COLUMN,			/* a column of the table relid was renamed from -> to */
	SIFT_NAME_TABLE,			/* the table relid was renamed or moved: from_schema.from -> to_schema.to */
	SIFT_NAME_INDEX				/* an index of the table relid was renamed from -> to (a scope names the btree it uses) */
} SiftNameKind;

typedef struct SiftNameChange
{
	SiftNameKind kind;
	Oid			relid;
	const char *from_schema;
	const char *from;
	const char *to_schema;
	const char *to;
} SiftNameChange;

/* The options of the index whose text names what changed, with the text written again (a list of DefElem); NIL when none does */
extern List *sift_names_renamed_options(Relation index, const SiftNameChange *c);

/* The text of the connect or scope option of the table host with what changed written again; NULL when it names none of it (and for any other option) */
extern char *sift_names_renamed_option(const char *name, const char *text, Oid host, const SiftNameChange *c);

/* The text of the option of an index (or of a definition) with the column from named to: NULL when the option does not name it as a column of the table */
extern char *sift_names_list_option(const char *option, const char *text, const char *from, const char *to);

#endif
