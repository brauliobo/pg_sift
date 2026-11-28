/*
 * agg/join/shape.h - the join of a fact table to its dimensions as the planner of the joins (agg/join/plan.c) and the one of an EXISTS over the elements of an array
 * (agg/exists_plan.c) read and extend it: the relations by the part they have in the join, each dimension with the keys its rows are probed by.
 */
#ifndef SIFT_AGG_V6_JOIN_SHAPE_H
#define SIFT_AGG_V6_JOIN_SHAPE_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/ext.h"
#include "agg/unnest/jsonb.h"

#define ROLE_FACT	(-2)
#define ROLE_ELEM	(-1)
#define ROLE_NONE	(-3)

typedef struct SiftV6JoinDim
{
	Relids		rels;
	RelOptInfo *rel;			/* its plan: the relation, or the join PostgreSQL made of the relations */
	Path	   *path;			/* the plan the node reads it with */
	List	   *on;				/* the RestrictInfo of each condition of its ON clause that no key says */
	bool		closing;		/* a RIGHT or FULL join of the join of all the other relations: the last join of the statement */
	Index		ojrelid;		/* a RIGHT or FULL join: the relation of that outer join, which the conditions above it name */
	SiftV6JoinMode mode;
	int			nkeys;
	const Node *probe[SIFT_STAR_MAX_KEYS];	/* the column of the relations before the dimension, or the expression of them, that its key is probed with */
	const Var  *key[SIFT_STAR_MAX_KEYS];
	int			kpos[SIFT_STAR_MAX_KEYS];	/* the position of a computed key in the tuples of path */
	const Node *kexpr[SIFT_STAR_MAX_KEYS];	/* the expression of the columns of the dimension that its key is, which its plan computes (NULL: the column key names); key is then a Var of its type only */
	bool		lateral;		/* a lateral sub-select cut to one row: its keys are the equalities it ties itself to the relations before it with (v6_lateral_plan.h) */
} SiftV6JoinDim;

typedef struct SiftV6JoinShape
{
	RelOptInfo *fact;
	RelOptInfo *elem;
	SiftV6ElemRte rte;			/* the function of elem */
	bool		elem_left;
	Relids		elem_nest;		/* the lookups the LEFT JOIN of elem joins inside its nullable side, to the element (LEFT JOIN LATERAL (SELECT .. FROM elem LEFT JOIN LATERAL lookup ..) ON true) */
	bool		semi;			/* elem is the array of an EXISTS: a row of the fact table counts once when some element passes */
	int			nd;
	int			nmain;			/* the dimensions of the statement's own join; those after them are the EXISTS' */
	SiftV6JoinDim dim[SIFT_STAR_MAX_DIMS];
	List	   *resid;			/* the RestrictInfo of each condition no key says */
	const RestrictInfo *sub_qual;	/* the EXISTS of the fact table's own conditions that semi stands for */
} SiftV6JoinShape;

/* the part a relation has in the join: the fact table, the element, the index of its dimension, or none */
extern int	sift_v6_join_role(const SiftV6JoinShape *sh, const Var *v);
/* the part of the latest relation the columns of e are of; ROLE_NONE when it reads a column the join does not know, or none */
extern int	sift_v6_join_expr_role(const SiftV6JoinShape *sh, const Node *e);
extern bool sift_v6_join_add_key(SiftV6JoinDim *d, const Node *probe, const Var *key, Oid inputcollid);
/* the same with a key that an expression of the dimension computes */
extern bool sift_v6_join_add_expr_key(SiftV6JoinDim *d, const Node *probe, const Var *key, const Node *kexpr, Oid inputcollid);
extern bool sift_v6_join_add_dim(SiftV6JoinShape *sh, Relids rels, SiftV6JoinMode mode);
/* the dimensions in the order the pipe probes them: a join of inner relations, then the others in the order the statement joins them, each probing columns of the relations before it */
extern bool sift_v6_join_make_dims(PlannerInfo *root, Relids base, SiftV6JoinShape *sh);

/* How many times more keys than PostgreSQL counts the inner and semi dimensions of one relation on one column of the fact pass to it (sift_v6_lookups_guess) */
extern double sift_v6_join_guessed_keys(PlannerInfo *root, const SiftV6JoinShape *sh, double input_rows);

/* The lookups inside the nullable side of the LEFT JOIN of the element match no row for a pair without one, and no condition but their keys joins them there */
extern bool sift_v6_join_elem_nest_ok(const SiftV6JoinShape *sh);

/*
 * An inner dimension of one relation whose keys are all keys of another one (a list of ids joined to the table of their rows, both to the same column of the fact table) is joined to it in the
 * plan of that one, when PostgreSQL's plan of the join of the two (the ids probing an index of the table) reads less than the two read whole: no fact row the join of the two drops would have
 * met both, and a fact row meets the rows of the join as it met the rows of each
 */
extern void sift_v6_join_chain_dims(PlannerInfo *root, SiftV6JoinShape *sh);

#endif							/* SIFT_AGG_V6_JOIN_SHAPE_H */
