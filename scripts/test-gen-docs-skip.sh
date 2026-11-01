#!/usr/bin/env bash
# scripts/gen-docs.py skips the benchmark renderers when the files they read and write are as a finished run left them, runs them when one of those files changed, and the files
# are the same as a run without the skip. Works on a copy of the tree (a few seconds of CPU per run of gen-docs.py).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
bad=0
fail() { echo "FAIL: $*"; bad=1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - 2>/dev/null | tar -xf - -C "$T"
git -C "$T" init -q
# the public README has no benchmark block (their results are not in the tree): the copy gets one, so that the renderers have a block to write
printf '\n<!-- bench:sets:begin -->\n<!-- bench:sets:end -->\n' >>"$T/README.md"
unset CI SIFT_LINT_NOCACHE
export SIFT_GEN_DOCS_CACHE=$T/.cache
# gen NAME [env]: a run of the copy; its log is $T/NAME.log
gen() { local n=$1; shift; env "$@" python3 -B -I "$T/scripts/gen-docs.py" >"$T/$n.log" 2>&1 || { fail "gen-docs.py exits with an error ($n): $(tail -n 3 "$T/$n.log")"; }; }
skipped() { grep -q 'renderers are skipped' "$T/$1.log"; }
# the files of the renderers, as one digest
state() { (cd "$T" && find README.md docs bench -type f ! -name '*.pyc' -print0 | sort -z | xargs -0 cat | scripts/sha256.sh); }

gen first
skipped first && fail "the first run, with nothing recorded, skipped the renderers"
good=$(state)
gen again
skipped again || fail "a second run on the files a run left did not skip the renderers"
[[ $(state) == "$good" ]] || fail "the skipped run changed a file"
# a change of a file they read or write (a stale generated block): they run, and the block is as the first run made it
n=$(awk '/<!-- bench:[a-z-]+:begin/ { f = 1; next } /<!-- bench:[a-z-]+:end/ { f = 0 } f && NF && !d { print NR; d = 1 }' "$T/README.md")
[[ -n $n ]] || fail "README.md has no generated benchmark block to change"
sed -i.bak "${n}s/\$/ stale/" "$T/README.md" && rm -f "$T/README.md.bak"
[[ $(state) != "$good" ]] || fail "the change of the block did not change the files"
gen changed
skipped changed && fail "a changed input did not run the renderers"
[[ $(state) == "$good" ]] || fail "the renderers did not restore the block"
# a change of a file they do not read (src/), and one of prose: skipped, and what a run without the skip gives is the same
echo '/* a comment */' >>"$T/src/core/ops.c"
gen src
skipped src || fail "a change of src/ ran the renderers"
# a change of the prose of a page with benchmark blocks (outside the blocks): the renderers only replace the blocks, so they are skipped
sed -i.bak '1s/$/ (edited)/' "$T/README.md" && rm -f "$T/README.md.bak"
gen prose
skipped prose || fail "a change of the prose of README.md ran the renderers"
good_src=$(state)
rm -rf "$T/.cache"
gen full SIFT_LINT_NOCACHE=1
skipped full && fail "a run with SIFT_LINT_NOCACHE skipped the renderers"
[[ $(state) == "$good_src" ]] || fail "the files after a skipped run differ from those of a full run"
(( bad )) || echo "gen-docs-skip: the renderers are skipped on files a run left, run on a changed one, and give the files of a full run"
exit $bad
