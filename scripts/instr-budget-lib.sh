# The harness of the scripts/instr-budget*.sh budgets: a scratch cluster with one table, then callgrind instructions of a statement run by a single-user backend.
# ib_begin NAME PORT (the script's NAME for messages, its default SIFT_PORT) starts the cluster and reads the SQL that builds the table from stdin; ib_ir SQLFILE FUNCTION... prints
# the instructions collected inside the functions (PortalRun: the whole statement); ib_end removes the data. Needs valgrind and an installed build: scripts/pgtest.sh install.
REPO=$(cd "$(dirname "$0")/.." && pwd)

ib_begin() {
	command -v valgrind >/dev/null || { echo "$1: valgrind is not installed, skipped"; exit 0; }
	export SIFT_TMP=${SIFT_TMP:-/tmp/sift-instr} SIFT_PORT=${SIFT_PORT:-$2}
	cd "$REPO"
	scripts/pgtest.sh stop >/dev/null 2>&1 || true
	rm -rf "$SIFT_TMP/data"
	scripts/pgtest.sh start >/dev/null
	eval "$(scripts/pgtest.sh env | grep '^export')"
	psql -X -q -At -d postgres -c "CREATE EXTENSION pg_sift" -c "ALTER DATABASE postgres SET sift.col_auto_merge = off" >/dev/null
	psql -X -q -At -d postgres >/dev/null
	scripts/pgtest.sh stop >/dev/null
}

ib_ir() {
	local file=$1 toggles=() f
	shift
	for f in "$@"; do toggles+=(--toggle-collect="$f"); done
	valgrind --tool=callgrind --collect-atstart=no "${toggles[@]}" --callgrind-out-file="${file%.sql}.cg" \
		postgres --single -D "$SIFT_TMP/data" -c sift.async_maintenance=off -c jit=off -c autovacuum=off postgres < "$file" 2>&1 >/dev/null | sed -n 's/.*Collected : *\([0-9]*\).*/\1/p' | tail -n 1
}

ib_end() { rm -rf "$SIFT_TMP/data"; }
