#!/usr/bin/env bash
# Sourced by the benchmarks that run on a scratch PostgreSQL cluster of their own (steps/sets.sh, extras.sh): never an existing cluster or database.
#
#   scratch_scope "$@"     re-run the script in a systemd scope (MemoryMax SCRATCH_MEM, 12G; CPUQuota SCRATCH_CPU, 300%; no swap); returns in the scope
#   scratch_up NAME        stage SIFT_BUILD's sift, make the cluster $SCRATCH_ROOT/NAME on a free port of 56150-56250 and start it (autovacuum off, fsync off,
#                          jit off, SCRATCH_SHARED_BUFFERS 2GB); exports PGHOST, PGPORT, PGUSER and defines `sq` (psql -qAt)
#   scratch_down           stop it (KEEP=1 keeps the data directory, otherwise it is removed)
#   host_load              the load averages and the CPU pressure ("load 5.9 5.7 5.4 cpu_pressure 2.1%"): printed next to every number
#
# SIFT_BUILD is the checkout whose scripts/pgtest.sh builds and loads the extension (default: this one); SIFT_ROOT, an install that already exists
# (pgtest.sh install, SIFT_TESTING= for the release library), is used as it is. Both let one script measure a release tag next to main.
SCRATCH_ROOT=${SCRATCH_ROOT:-$HOME/.cache/sift-bench-scratch}
SIFT_BUILD=${SIFT_BUILD:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}

scratch_scope() {
	if [[ -z ${SCRATCH_SCOPED:-} && -z ${NO_CAPS:-} ]] && command -v systemd-run >/dev/null; then
		exec systemd-run --user --scope -q -p "MemoryMax=${SCRATCH_MEM:-12G}" -p MemorySwapMax=0 -p OOMPolicy=continue -p "CPUQuota=${SCRATCH_CPU:-300%}" env SCRATCH_SCOPED=1 "$0" "$@"
	fi
}

host_load() { # the load averages and the CPU pressure of the host
	printf 'load %s cpu_pressure %s%%' "$(cut -d' ' -f1-3 /proc/loadavg)" "$(awk -F'[= ]' '/^some/ {print $3}' /proc/pressure/cpu 2>/dev/null || echo NA)"
}

scratch_up() {
	local name=$1 port=
	export SCRATCH_NAME=$name SIFT_TMP=$SCRATCH_ROOT/$name
	PG=$SIFT_BUILD/scripts/pgtest.sh
	[[ -n ${SIFT_ROOT:-} ]] || export SIFT_ROOT=$SIFT_TMP/root
	[[ -f $SIFT_ROOT/.flavor ]] || SIFT_TESTING= SIFT_TMP=$SIFT_TMP "$PG" install >&2 || return 1
	for _ in $(seq 1 60); do
		port=$((56150 + RANDOM % 101))
		ss -ltn 2>/dev/null | grep -q ":$port " || break
	done
	export SIFT_PORT=$port
	if [[ ! -f $SIFT_TMP/data/PG_VERSION ]]; then
		mkdir -p "$SIFT_TMP/sock"
		"$PG" new-data "$SIFT_TMP/data" wal || return 1
		cat >>"$SIFT_TMP/data/postgresql.conf" <<-EOF
			port = $SIFT_PORT
			listen_addresses = ''
			unix_socket_directories = '$SIFT_TMP/sock'
			$("$PG" conf)
			fsync = off
			synchronous_commit = off
			shared_buffers = ${SCRATCH_SHARED_BUFFERS:-2GB}
			max_wal_size = 16GB
			checkpoint_timeout = 30min
			max_worker_processes = 16
			jit = off
			autovacuum = off
		EOF
	else # a kept cluster: the port of this run
		sed -i "s/^port = .*/port = $SIFT_PORT/" "$SIFT_TMP/data/postgresql.conf"
	fi
	"$PG" start >&2 || return 1
	export PGHOST=$SIFT_TMP/sock PGPORT=$SIFT_PORT PGUSER=postgres
	sq() { psql -X -qAt -v ON_ERROR_STOP=1 "$@"; }
}

scratch_down() {
	"$PG" stop >&2 2>/dev/null || true
	[[ -n ${KEEP:-} ]] || rm -rf "$SIFT_TMP/data" "$SIFT_TMP/sock" "$SIFT_TMP/postgres.log"
}
