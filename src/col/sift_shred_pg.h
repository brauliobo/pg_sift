/*
 * sift_shred_pg.h - the PostgreSQL driver of the shredder: walks a jsonb with the iterator and feeds sift_shred.h its events.
 */
#ifndef SIFT_SHRED_PG_H
#define SIFT_SHRED_PG_H

#include "utils/jsonb.h"

#include "sift_shred.h"

/* One jsonb as the next value of the shredder (a container, or the scalar of a raw scalar jsonb): for a caller that sends the
 * events around it itself, e.g. the object {attname: value} of the attributes of a tuple */
extern void sift_shred_container(SiftShred *sh, JsonbContainer *c);

/* A whole document: sift_shred_begin, the value, sift_shred_finish */
extern void sift_shred_jsonb(SiftShred *sh, Jsonb *jb);

#endif							/* SIFT_SHRED_PG_H */
