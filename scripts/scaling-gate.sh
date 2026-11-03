#!/usr/bin/env bash
# The scaling gate's library: this tree built as a SIFT_TESTING=1 flavour with 8-page units (SIFT_SB_SHIFT=3, src/core/sift.h), so that a few MB of heap hold
# thousands of column units. It is a build of its own (the geometry differs from the shipped one: never install it) kept in a cache, keyed by the tree.
#
#   scripts/scaling-gate.sh build      build or find the flavour; prints its root (PostgreSQL 18 or later: 77 when there is no extension_control_path to hold it)
#
# SIFT_SB_DIR (default ~/.cache/sift/sb-flavour/<key>, a symlink to tmpfs on a development host) says where it is kept. The test is test/t/scaling_slope.sh.
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIFT=3
PG_MAJOR=$(pg_config --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
(( PG_MAJOR >= 18 )) || { echo "SKIP the 8-page flavour needs PostgreSQL 18 (extension_control_path)" >&2; exit 77; }

# a compiler cache, when the host has one in the usual place: the flavour is the whole library again (58 s cold, 3 s when the sources were compiled before with these flags)
for d in /usr/lib/ccache /usr/lib64/ccache /opt/homebrew/opt/ccache/libexec; do [[ -d $d ]] && { PATH=$d:$PATH; break; }; done

paths=(Makefile 'pg_sift*.control' sql src tools scripts/harden.mk)
key=$({ git -C "$REPO" ls-files -s -- "${paths[@]}"; git -C "$REPO" diff HEAD -- "${paths[@]}"; echo "shift=$SHIFT $(pg_config --version)"; } | "$REPO/scripts/sha256.sh" | cut -c1-16)
DIR=${SIFT_SB_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/sift/sb-flavour/$key}

# Two jobs of a CI run build it at the same time (scaling_slope, sb_mask_small): the one that starts second removes the directory of the first, and both fail. One builds under a
# lock directory (mkdir: macOS has no flock) and the others wait for its .flavor; a lock whose owner is gone is taken over.
build() {
	local owner
	[[ -f $DIR/root/.flavor ]] && { echo "$DIR/root"; return; }
	mkdir -p "$(dirname "$DIR")" || return 1
	until mkdir "$DIR.lock" 2>/dev/null; do
		[[ -f $DIR/root/.flavor ]] && { echo "$DIR/root"; return; }
		owner=$(cat "$DIR.lock/pid" 2>/dev/null)
		if [[ -n $owner ]] && ! kill -0 "$owner" 2>/dev/null; then rm -rf "$DIR.lock"; fi
		sleep 1
	done
	echo $$ >"$DIR.lock/pid"
	[[ -f $DIR/root/.flavor ]] && { rm -rf "$DIR.lock"; echo "$DIR/root"; return; }
	rm -rf "$DIR"; mkdir -p "$DIR/src" || { rm -rf "$DIR.lock"; return 1; }
	git -C "$REPO" ls-files -z -c -o --exclude-standard -- "${paths[@]}" | tar -C "$REPO" --null -T - -cf - | tar -C "$DIR/src" -xf -
	local flavor=(USE_PGXS=1 SIFT_TESTING=1 SIFT_SB_SHIFT=$SHIFT)
	{ nice make -C "$DIR/src" -j4 "${flavor[@]}" && make -C "$DIR/src" "${flavor[@]}" DESTDIR="$DIR/root" install; } >"$DIR/build.log" 2>&1 \
		|| { tail -20 "$DIR/build.log" >&2; rm -rf "$DIR" "$DIR.lock"; return 1; }
	echo "testing sb-shift-$SHIFT" >"$DIR/root/.flavor"
	rm -rf "$DIR/src" "$DIR.lock"
	echo "$DIR/root"
}

case ${1:-} in
	build) build ;;
	*) echo "usage: $0 build" >&2; exit 2 ;;
esac
