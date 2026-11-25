/*
 * core/maint/drain.h - the maintenance workers of a database leave before DROP DATABASE and CREATE DATABASE ... TEMPLATE look for the sessions that use it, and before DROP EXTENSION pg_sift
 * drops what they read. The schema of the extension is not renamed.
 */
#ifndef SIFT_MAINT_DRAIN_H
#define SIFT_MAINT_DRAIN_H

#include "postgres.h"

extern void sift_maint_drain_install(void);

#endif							/* SIFT_MAINT_DRAIN_H */
