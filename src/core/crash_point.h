/*
 * core/crash_point.h - the named crash points of the test build: sift.test_crash_point = '<tag>' kills the process with SIGKILL, its WAL flushed, the first time it passes the point of
 * that tag (core/crash_point_test.c). A release build compiles every point to nothing and has no setting.
 *
 * A tag names the boundary of a durable write: pN is the Nth phase of a commit (p1 the pages written, p2 the manifest record that publishes them), pre_ before it, post_ after it.
 * The tags are the ones bench/torture/030_v6_crash_standby_torture.pl arms; a point is written SIFT_CRASH_POINT(tag), with the bare tag, so a misspelt tag is a compile error.
 */
#ifndef SIFT_CORE_CRASH_POINT_H
#define SIFT_CORE_CRASH_POINT_H

#define SIFT_CRASH_POINT_LIST(X) \
	X(f2_col_delta_post_append_pre_commit) \
	X(f2_col_delta_fold_pre_p2) \
	X(f2_col_delta_fold_post_p2) \
	X(f2_proj_build_pre_commit) \
	X(f2_proj_refresh_post_p1_pre_p2) \
	X(f2_proj_drop_post_p2) \
	X(f2_tier_upload_post_remote_pre_commit) \
	X(f2_tier_evict_post_p2) \
	X(f2_ttl_drop_pre_p2) \
	X(f2_ttl_drop_post_p2)

#ifdef SIFT_TESTING

typedef enum SiftCrashPoint
{
#define X(tag) SIFT_CP_##tag,
	SIFT_CRASH_POINT_LIST(X)
#undef X
	SIFT_CP_COUNT
} SiftCrashPoint;

extern void sift_crash_point(SiftCrashPoint cp);
extern void sift_crash_point_gucs(void);

#define SIFT_CRASH_POINT(tag)			sift_crash_point(SIFT_CP_##tag)
#define SIFT_CRASH_POINT_IF(cond, tag)	do { if (cond) sift_crash_point(SIFT_CP_##tag); } while (0)

#else

#define SIFT_CRASH_POINT(tag)			((void) 0)
#define SIFT_CRASH_POINT_IF(cond, tag)	((void) 0)

#endif

#endif
