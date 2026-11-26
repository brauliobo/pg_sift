# The developer targets beyond the build (the goals that build nothing, the suites run against the scripts/pgtest.sh cluster, isa-check, limits, gpu-shaders); included by the Makefile.

ifneq ($(filter $(SIFT_DEV_GOALS),$(MAKECMDGOALS)),)
.PHONY: $(SIFT_DEV_GOALS)
check:
	scripts/ci.sh quick
lint:
	scripts/lint.sh
dev-check:
	scripts/lint.sh complexity memory locks capacity file-size neutral format-isolation
check-scaling-ops:
	scripts/check-scaling-ops.sh
pg-assert:
	nice scripts/pg-assert.sh $(if $(PG_ASSERT_MAJOR),--major $(PG_ASSERT_MAJOR))
pg-valgrind:
	nice scripts/pg-assert.sh --valgrind $(if $(PG_ASSERT_MAJOR),--major $(PG_ASSERT_MAJOR))
endif

# Suites beyond installcheck, run against the scripts/pgtest.sh cluster
# (install and start it first). scripts/ci.sh runs all of them.
.PHONY: isolationcheck tapcheck difftest fuzz soak gpu-shaders limits
isolationcheck:
	scripts/isolation.sh
tapcheck:
	scripts/tap.sh
difftest fuzz soak:
	scripts/$@.sh
# the x86-64 baseline of the objects and of pg_sift.so, by symbol (scripts/isa_check.sh)
.PHONY: isa-check
isa-check: all
	scripts/isa_check.sh
# docs/LIMITS.md from the registry in src/core/limits.h (scripts/gen-limits.sh --check proves it)
limits:
	scripts/gen-limits.sh

# Rebuilds src/agg/vk/spv.h from the shaders of src/agg/vk/shaders (needs glslc) and the Vulkan declarations from /usr/include/vulkan; commit the result.
gpu-shaders:
	scripts/gen-aggvk-spv.sh
	scripts/gen-aggvk-min.py
