#!/usr/bin/env bash
# A sift.test_* setting is defined only in the testing flavour (needs no PostgreSQL and no build): a Define*Variable("sift.test_..") of src/ outside #ifdef SIFT_TESTING
# puts a setting that only tests use in the library users get. The settings in scripts/test-settings-shipped.allow are the exceptions, each with its reason.
#
#   scripts/check-test-settings.sh
#   scripts/check-test-settings.sh --selftest
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail
cd "$(dirname "$0")/.."

# outside ALLOW FILE...: file:line name of each setting defined where SIFT_TESTING is not defined; a conditional is a testing one from `#ifdef SIFT_TESTING` or
# `#if defined(SIFT_TESTING)` to its #else, and from the #else of `#ifndef SIFT_TESTING`
outside() {
	local allow=$1; shift
	awk -v allow="$allow" '
		BEGIN { n = split(allow, a, " "); for (i = 1; i <= n; i++) ok[a[i]] = 1 }
		FNR == 1 { d = 0 }
		/^[ \t]*#[ \t]*(ifdef|ifndef|if)[ \t]/ {
			d++; neg[d] = ($0 ~ /ifndef[ \t]+SIFT_TESTING/); on[d] = ($0 ~ /ifdef[ \t]+SIFT_TESTING/ || $0 ~ /if[ \t]+defined[ \t]*\(SIFT_TESTING\)/); next
		}
		/^[ \t]*#[ \t]*(else|elif)/ { on[d] = neg[d] && $0 ~ /else/; neg[d] = 0; next }
		/^[ \t]*#[ \t]*endif/ { d--; next }
		match($0, /Define[A-Za-z]*Variable\("sift\.test_[a-z0-9_]+"/) {
			t = 0; for (i = 1; i <= d; i++) t += on[i]
			name = substr($0, RSTART, RLENGTH); sub(/.*\("/, "", name); sub(/"$/, "", name)
			if (!t && !(name in ok)) print FILENAME ":" FNR " " name
		}' "$@"
}

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d)
	trap 'rm -rf "$d"' EXIT
	printf '%s\n' '#ifdef SIFT_TESTING' 'DefineCustomBoolVariable("sift.test_a", 1);' '#endif' >"$d/in.c"
	printf '%s\n' '#ifdef SIFT_TESTING' '#ifdef X' '#endif' 'DefineCustomBoolVariable("sift.test_a", 1);' '#else' 'DefineCustomBoolVariable("sift.test_b", 1);' '#endif' >"$d/else.c"
	printf '%s\n' '#ifndef SIFT_TESTING' '#else' 'DefineCustomBoolVariable("sift.test_a", 1);' '#endif' 'DefineCustomIntVariable("sift.test_c",' >"$d/out.c"
	printf '%s\n' 'DefineCustomIntVariable("sift.test_shipped", 1);' 'DefineCustomIntVariable("sift.plain", 1);' >"$d/allowed.c"
	[[ -z $(outside "" "$d/in.c") ]] || { echo "check-test-settings: selftest: a setting inside #ifdef SIFT_TESTING was reported"; exit 1; }
	(( $(outside "" "$d/else.c" | wc -l) == 1 )) || { echo "check-test-settings: selftest: the #else of #ifdef SIFT_TESTING is not testing"; exit 1; }
	[[ -z $(outside "" "$d/out.c" | grep -v sift.test_c) ]] || { echo "check-test-settings: selftest: the #else of #ifndef SIFT_TESTING was reported"; exit 1; }
	[[ -n $(outside "" "$d/allowed.c") && -z $(outside "sift.test_shipped" "$d/allowed.c") ]] || { echo "check-test-settings: selftest: the allow list does not work"; exit 1; }
	echo "check-test-settings: selftest ok"
	exit 0
fi

allow=$(sed -n 's/^\(sift\.test_[a-z0-9_]*\)\t.*/\1/p' scripts/test-settings-shipped.allow | tr '\n' ' ')
mapfile -t files < <(git ls-files 'src/*.c')
bad=$(outside "$allow" "${files[@]}")
[[ -z $bad ]] || { echo "$bad"; echo "check-test-settings: a sift.test_ setting defined outside #ifdef SIFT_TESTING: move it into the testing block, or name it in scripts/test-settings-shipped.allow with its reason"; exit 1; }
echo "check-test-settings: every sift.test_ setting of src/ is defined under SIFT_TESTING or named in scripts/test-settings-shipped.allow"
