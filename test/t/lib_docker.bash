# Container nodes for the shell TAP tests (SIFT_NODES=docker, run by scripts/replication.sh --container).
# Sourced at the end of lib.bash, it replaces the node functions: every node is a throwaway container of
# $SIFT_IMAGE (PostgreSQL with pg_sift installed) with its own filesystem, on the docker network $SIFT_DOCKER_NET,
# which also prefixes the container names. Only the functions replication.sh needs are provided.
PGD=/var/lib/postgresql/node

cname() { echo "$SIFT_DOCKER_NET-$1"; }
dx() { local name=$1; shift; docker exec -i -u postgres "$(cname "$name")" "$@"; }

container_up() {
	NODES+=("$1")
	docker run -d --name "$(cname "$1")" --network "$SIFT_DOCKER_NET" --entrypoint sleep "$SIFT_IMAGE" infinity >/dev/null ||
		bail "container $1 failed"
}

cleanup() {
	local name
	for name in "${NODES[@]}"; do
		if [[ $NFAIL -ne 0 ]]; then
			mkdir -p "$(dir "$name")"
			docker cp "$(cname "$name"):$PGD/postgres.log" "$(dir "$name")/postgres.log" >/dev/null 2>&1
		fi
		docker rm -f "$(cname "$name")" >/dev/null 2>&1
	done
}

# no process to inspect from outside: stop the container's server cleanly and scan its log
node_leakcheck() {
	node_stop "$1"
	mkdir -p "$(dir "$1")"
	docker cp "$(cname "$1"):$PGD/postgres.log" "$(dir "$1")/postgres.log" >/dev/null 2>&1
	leak_scan "$(dir "$1")"
}

node_conf() { printf '%s\n' "$2" | dx "$1" tee -a "$PGD/postgresql.conf" >/dev/null; }

# node_init NAME OFFSET [conf lines]: the offset only numbers ports in the unix socket mode
node_init() {
	container_up "$1"
	dx "$1" initdb -D "$PGD" -A trust -U postgres -E UTF8 --no-locale --no-instructions >/dev/null || bail "initdb $1 failed"
	printf 'host all all all trust\nhost replication all all trust\n' | dx "$1" tee -a "$PGD/pg_hba.conf" >/dev/null
	node_conf "$1" "listen_addresses = '*'
fsync = off
max_wal_size = 256MB
restart_after_crash = off
shared_buffers = 128MB
max_connections = 60
${3:-}"
}

node_clone() {
	container_up "$2"
	dx "$2" pg_basebackup -h "$(cname "$1")" -U postgres -D "$PGD" -R -X stream -c fast >/dev/null 2>&1 ||
		bail "pg_basebackup $2 failed"
	node_conf "$2" "hot_standby = on
${4:-}"
}

node_start() { dx "$1" pg_ctl -D "$PGD" -l "$PGD/postgres.log" -w -t 120 start >/dev/null 2>&1; }
node_stop() { dx "$1" pg_ctl -D "$PGD" -m fast -w stop >/dev/null 2>&1; true; }
node_promote() { dx "$1" pg_ctl -D "$PGD" -w promote >/dev/null; }
conninfo() { echo "host=$(cname "$1") user=postgres dbname=postgres"; }

sql() {
	local name=$1
	shift
	docker exec -i -u postgres -e PGOPTIONS="-c client_min_messages=warning" "$(cname "$name")" \
		psql -X -At -q -v ON_ERROR_STOP=1 -U postgres -d postgres "${@/#/--command=}"
}
