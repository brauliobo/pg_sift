# Releases and checkouts of sift built once each for the benchmarks of steps/extras.sh and steps/sets.sh (sourced; REPO is the checkout of the harness).
# A label is a tag or HEAD with its git worktree under $EXTRAS_SRC and its production install (pgtest.sh install, SIFT_TESTING= unset) under $EXTRAS_CACHE/LABEL/root:
# v302 (v3.0.2), v310 (v3.1.0), v310-vk (v3.1.0 with VULKAN=1) and main (HEAD of this checkout); any other tag vX.Y.Z is labelled vXYZ.
EXTRAS_SRC=${EXTRAS_SRC:-${BENCH_WORK:-/tmp/sift-bench}/builds}
EXTRAS_CACHE=${EXTRAS_CACHE:-$HOME/.cache/sift-bench-builds}

build_label() { [[ $1 == HEAD ]] && echo main || echo "${1//./}"; }   # a ref -> its label
build_ref() { case $1 in main) echo HEAD ;; *-vk) build_ref "${1%-vk}" ;; v[0-9]*) sed -E 's/^v([0-9])([0-9])([0-9]+)$/v\1.\2.\3/' <<<"$1" ;; *) echo "$1" ;; esac; }
build_dir() { echo "$EXTRAS_SRC/$1"; }
build_prepare() { # LABEL: the checkout and its production install, made when missing
	local l=$1 dir ref; dir=$(build_dir "$l"); ref=$(build_ref "$l")
	if [[ ! -d $dir ]]; then
		mkdir -p "$EXTRAS_SRC"
		git -C "$REPO" worktree add --detach "$dir" "$(git -C "$REPO" rev-parse "$ref^{commit}")" >&2
	fi
	local vk=(); [[ $l != *-vk ]] || vk=(VULKAN=1)
	[[ -f $EXTRAS_CACHE/$l/root/.flavor ]] || ( cd "$dir" && env SIFT_TESTING= SIFT_TMP="$EXTRAS_CACHE/$l" "${vk[@]}" nice scripts/pgtest.sh install >&2 )
}
