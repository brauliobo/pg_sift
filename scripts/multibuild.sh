# Sourced by the scripts that compare builds of sift (merge-bytes.sh, build-bench.sh, perf-gate-maint.sh): one throwaway cluster of
# scripts/pgtest.sh for every BUILDS entry (LABEL=DIR), on the ports SIFT_PORT (default 56150) and up, removed at the end.
#   mb_begin PREFIX      the scratch directory $BASE (/tmp/PREFIX.XXXXXX) and the clean-up of the clusters and of it on exit
#   mb_start             installs and starts the cluster of every build; LABELS lists them in order
#   pg LABEL ARGS...     scripts/pgtest.sh of that build's cluster (SIFT_TESTING of the build: $MB_TESTING, default the production flavor)
#   q LABEL SQL...       one psql session, one -c per statement
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
PORT0=${SIFT_PORT:-56150}
declare -A DIR TMP PORT
LABELS=()

pg() {
	local l=$1; shift
	( cd "${DIR[$l]}" && SIFT_TMP=${TMP[$l]} SIFT_PORT=${PORT[$l]} SIFT_TESTING=${MB_TESTING-} scripts/pgtest.sh "$@" )
}
q() {
	local l=$1 a=() s; shift
	for s in "$@"; do a+=(-c "$s"); done
	pg "$l" psql -q -At -v ON_ERROR_STOP=1 "${a[@]}"
}
mb_cleanup() {
	local l
	for l in "${!DIR[@]}"; do pg "$l" destroy >/dev/null 2>&1 || true; done
	rm -rf "$BASE"
}
mb_begin() {
	BASE=$(mktemp -d "/tmp/$1.XXXXXX")
	trap mb_cleanup EXIT
}
mb_start() {
	local b l i=0
	for b in "${BUILDS[@]}"; do
		l=${b%%=*}
		LABELS+=("$l")
		DIR[$l]=$(cd "${b#*=}" && pwd) TMP[$l]=$BASE/$l PORT[$l]=$((PORT0 + i++))
		mkdir -p "${TMP[$l]}"
		pg "$l" install >/dev/null
		pg "$l" start >/dev/null
	done
}
