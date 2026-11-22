#!/usr/bin/env bash
# Every C symbol that the frozen script of 5.1 names must exist in the 6.0 library, or CREATE EXTENSION ... VERSION '5.1' and every call before the update fail with 58P01.
#   scripts/check-retired.sh FROZEN_SCRIPT [SRC_DIR]
# 1. each symbol of src/core/retired.c is named by FROZEN_SCRIPT (a typo here protects nothing);
# 2. with SRC_DIR, each symbol that FROZEN_SCRIPT names is defined there (PG_FUNCTION_INFO_V1) or in src/core/retired.c.
set -euo pipefail
cd "$(dirname "$0")/.."
frozen=${1:?usage: check-retired.sh FROZEN_SCRIPT [SRC_DIR]}
src=${2:-}
registry=$(grep -oE '^SIFT_(RETIRED|REMOVED)_FN\([a-z0-9_]+' src/core/retired.c | sed 's/.*(//' | sort -u)
named=$(grep -oE "MODULE_PATHNAME'[ ]*,[ ]*'[A-Za-z0-9_]+'" "$frozen" | sed "s/.*,[ ]*'//;s/'//" | sort -u)
echo "registry: $(wc -l <<<"$registry") symbols; frozen script names $(wc -l <<<"$named")"
stray=$(comm -23 <(echo "$registry") <(echo "$named"))
[[ -z $stray ]] || { echo "check-retired: not named by $frozen: $stray"; exit 1; }
if [[ -n $src ]]; then
	defined=$(grep -rhoE 'PG_FUNCTION_INFO_V1\([A-Za-z0-9_]+\)' "$src" | sed 's/.*(//;s/)//' | sort -u)
	missing=$(comm -23 <(echo "$named") <(sort -u <(echo "$defined") <(echo "$registry")))
	[[ -z $missing ]] || { echo "check-retired: named by $frozen, defined nowhere: $missing"; exit 1; }
	echo "check-retired: every symbol of $frozen is defined in $src or retired"
else
	echo "check-retired: every retired symbol is named by $frozen"
fi
