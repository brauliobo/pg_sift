/*
 * core/quarantine/flags.h - what a backend remembers of the flag SIFT_META_QUARANTINED of the metapages it has read (core/quarantine/flags.c).
 */
#ifndef SIFT_QUARANTINE_FLAGS_H
#define SIFT_QUARANTINE_FLAGS_H

#include "core/host.h"

/* Whether the metapage of the index has the flag, read once until the relcache entry of the index is invalidated */
extern bool sift_quar_meta_flagged(Relation index);

#endif							/* SIFT_QUARANTINE_FLAGS_H */
