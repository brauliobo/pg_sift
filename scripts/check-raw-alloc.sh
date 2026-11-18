#!/usr/bin/env bash
# The C library's allocator is out of reach of the memory contexts and of the leak hooks (scripts/leakcheck.sh sees contexts, resources and shared tables, not a malloc that is never
# freed), so src/ allocates with palloc. Only the Vulkan backend (src/agg/vk/, a library of its own with handles) calls malloc, and LeakSanitizer (scripts/leaks-asan.sh) is the check for it.
# The fix for a new malloc, calloc, realloc or strdup is palloc in the context that owns the data.
#   scripts/check-raw-alloc.sh
cd "$(git rev-parse --show-toplevel)" || exit 1
out=$(git grep -nE '(^|[^_a-zA-Z])(malloc|calloc|realloc|strdup|strndup|asprintf)\(' -- 'src/*.c' ':!src/agg/vk/*' ':!src/*_test.c' ':!src/*/*_test.c' ':!src/*/test/*' ':!src/core/leak_test.c' |
	grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)' || true)
[[ -z $out ]] || { echo "$out"; echo "check-raw-alloc: raw C allocation in src/: palloc in the context that owns the data (only src/agg/vk/ keeps handles of a library of its own)"; exit 1; }
