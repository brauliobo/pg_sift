# The queue of the background maintenance (core/maint/sched.c, db.c, sched_reap.c) on a fake host: a clock, the workers it starts and the pids it sees are the test's; the hold of drain.c on a fake queue.
MAINT_TESTS = core/maint/sched core/maint/drain

TESTS    += $(MAINT_TESTS)
AUTOSTUB += $(MAINT_TESTS)
$(foreach t,$(MAINT_TESTS),$(eval XFLAGS_$(t) = -D_GNU_SOURCE -Wno-unused-function))
