#!/usr/bin/env bash
# Runs a lint check once per box and per input: a pass is remembered by the git content of the check's inputs, and a check that is running makes the same
# check on the same inputs in another run wait for it and take its result (several landings lint at once; each used to generate the same charts and run the same selftest).
#
#   scripts/lint-cache.sh NAME [--names] [--tags] PATHSPEC... -- COMMAND...
#
# The key is the staged content of PATHSPEC (`git ls-files -s`), the unstaged changes to it, with --names the name of every tracked file (a map of files),
# with --tags the release tags. LINT_CACHE_RECORD=1 stores the pass without running the command, for a caller that has just done what the command proves (land.sh after
# gen-docs left the tree unchanged: `gen-docs.py --check` is then true by construction). Only a pass is stored (SIFT_LINT_CACHE, default ~/.cache/pg_sift/lint); a failure always runs again. CI and
# SIFT_LINT_NOCACHE=1 run the command as it is.
NAME=${1:?name}; shift
names='' tags='' specs=()
while [[ ${1:-} != -- ]]; do
	case ${1:?missing --} in --names) names=1 ;; --tags) tags=1 ;; *) specs+=("$1") ;; esac
	shift
done
shift
[[ -z ${CI:-} && -z ${SIFT_LINT_NOCACHE:-} ]] || exec "$@"

DIR=${SIFT_LINT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/pg_sift/lint}
mkdir -p "$DIR" 2>/dev/null || exec "$@"
key=$({
	printf '%s\n' "$NAME" "$*"
	git ls-files -s -- "${specs[@]}"
	git diff --no-ext-diff -- "${specs[@]}"
	[[ -z $names ]] || git ls-files
	[[ -z $tags ]] || git tag --list 'v*'
	git ls-files -s -- scripts/lint-cache.sh
} | scripts/sha256.sh | cut -c1-64)
[[ -n $key ]] || exec "$@"
stamp=$DIR/$NAME-${key:0:24}.ok

[[ -z ${LINT_CACHE_RECORD:-} ]] || { echo "recorded by the caller" >"$stamp"; exit 0; }
hit() { [[ -f $stamp ]] && { echo "$(cat "$stamp") (cached)"; exit 0; }; }
hit
if command -v flock >/dev/null; then   # the same check on the same inputs in another run: wait for it (at most 20 minutes), then take its result
	exec 9>"$stamp.lock"
	flock -w 1200 9 && hit
fi
out=$(mktemp)
trap 'rm -f "$out"' EXIT
"$@" >"$out" 2>&1
rc=$?
cat "$out"
if (( rc == 0 )); then
	tail -n 1 "$out" >"$stamp.$$" && mv "$stamp.$$" "$stamp"
	find "$DIR" \( -name "$NAME-*.ok" -o -name "$NAME-*.lock" \) -mtime +7 -delete 2>/dev/null
fi
exit $rc
