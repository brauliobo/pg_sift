/*
 * col/shred_int.h - shared between col/shred.c (the shredder) and col/shred_spec.c (the spec parser): the node tree the parser builds and the
 * shredder walks.
 */
#ifndef SIFT_SHRED_INT_H
#define SIFT_SHRED_INT_H

#include "col/shred.h"

typedef enum NodeKind
{
	N_OBJ,
	N_LEAF,
	N_LIST,
	N_MAP
} NodeKind;

typedef struct Member
{
	char	   *key;
	int			len;
	SiftShredNode *node;
} Member;

struct SiftShredNode
{
	NodeKind	kind;
	int			col;			/* LEAF */
	SiftShredType type;			/* LEAF */
	uint32		maxlen;			/* LEAF text */
	int			group;			/* OBJ, LEAF: the group whose element they sit in; LIST, MAP: their own group */
	int			pgroup;			/* LIST, MAP: the group of the element the list hangs from */
	SiftShredNode *elem;	/* LIST, MAP: what each element is */
	int			nmembers;		/* OBJ, sorted by (len, bytes) like the keys of a jsonb */
	int			capmembers;
	Member	   *members;
};

extern Member *find_member(const SiftShredNode *n, const char *key, int len, int *pos);

#endif							/* SIFT_SHRED_INT_H */
