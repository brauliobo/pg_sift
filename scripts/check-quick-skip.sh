#!/usr/bin/env bash
# The quick tier must not hide a test. SIFT_QUICK reaches a test through knob and ams (test/t/lib.bash, test/t/SiftTest.pm), which shrink a size and run the body all the
# same. A test that reads SIFT_QUICK itself can leave its body out of the quick tier (and out of the pull requests that run only that tier) without a line that says so;
# it is listed in scripts/quick-skip.allow, path TAB reason, and an entry whose file no longer reads it is removed.
#   scripts/check-quick-skip.sh [--selftest]
cd "$(git rev-parse --show-toplevel)" || exit 1
allow=scripts/quick-skip.allow

check() { # ROOT: a line for each breach under ROOT
	local root=$1 f
	git -C "$root" grep -l 'SIFT_QUICK' -- test ':!test/t/lib.bash' ':!test/t/SiftTest.pm' | while read -r f; do
		grep -q "^$f	." "$root/$allow" || echo "$f: reads SIFT_QUICK and is not in $allow (use knob for a size; a test left out of the quick tier is listed there with the reason)"
	done
	grep -v '^#' "$root/$allow" | cut -f1 | while read -r f; do
		git -C "$root" grep -q 'SIFT_QUICK' -- "$f" 2>/dev/null || echo "$allow: $f does not read SIFT_QUICK any more"
	done
}

if [[ ${1:-} == --selftest ]]; then
	t=$(mktemp -d)
	trap 'rm -rf "$t"' EXIT
	git -C "$t" init -q && mkdir -p "$t/scripts" "$t/test/t"
	echo '[[ -z $SIFT_QUICK ]] || exit 0' >"$t/test/t/a.sh"
	echo '[[ -z $SIFT_QUICK ]] || exit 0' >"$t/test/t/b.sh"
	echo 'knob() { echo $SIFT_QUICK; }' >"$t/test/t/lib.bash"
	printf '# header\ntest/t/b.sh\tthe body runs in the full tier only: it needs 2 GB\ntest/t/gone.sh\tstale\n' >"$t/$allow"
	git -C "$t" add -A
	out=$(check "$t")
	[[ $out == *"test/t/a.sh: reads SIFT_QUICK"* ]] || { echo "selftest: a test that reads SIFT_QUICK and is not listed passed: $out"; exit 1; }
	[[ $out == *"gone.sh does not read"* ]] || { echo "selftest: a stale entry passed: $out"; exit 1; }
	[[ $(wc -l <<<"$out") == 2 ]] || { echo "selftest: a listed test or the library was refused: $out"; exit 1; }
	echo "check-quick-skip selftest ok"
	exit 0
fi
out=$(check .)
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "quick-skip: no test leaves the quick tier by itself"
