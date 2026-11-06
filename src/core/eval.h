/* Developer experiments: sift.eval_methods names the ones that are on. A method is off unless named, except those of the default set (skip_trigger_lookup), which a name with a minus sign turns off. */
#ifndef SIFT_CORE_EVAL_H
#define SIFT_CORE_EVAL_H

typedef enum SiftEvalMethod
{
	SIFT_EVAL_SKIP_TRIGGER_LOOKUP,
	SIFT_EVAL_PENALTY_MAINTENANCE,
	SIFT_EVAL_SNAPSHOT_MASKS,
	SIFT_EVAL_UNIT_REWRITE,
	SIFT_EVAL_LATEST_HINT
} SiftEvalMethod;

extern void
sift_eval_init(void);
extern bool
sift_eval_on(SiftEvalMethod method);
#endif
