#!/usr/bin/env bash
# Replication test (test/t/replication.sh): a physical hot standby and a logical subscriber against the same primary, with
# the results compared after every kind of write. Exit 1 on a failed check; the nodes and temporary files are removed.
#
#   scripts/replication.sh              nodes are clusters in a temp dir (pgtest.sh staging, unix sockets, free port range)
#   scripts/replication.sh --container  nodes are docker containers with separate filesystems: PostgreSQL ($PG_IMAGE, default
#                                       postgres:18, major 17 or 18) with pg_sift built from this tree, one docker network
#
# PROVE_FLAGS (default -v) goes to prove.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
export PROVE_FLAGS=${PROVE_FLAGS--v}
TMP=$(mktemp -d /tmp/sift-replication.XXXXXX)
NET=pg_sift-repl-$$
cleanup() {
	local rc=$?
	docker ps -aq --filter "name=^$NET-" 2>/dev/null | xargs -r docker rm -f >/dev/null
	docker network rm "$NET" >/dev/null 2>&1 || true
	if (( rc )); then echo "replication: failed, logs in $TMP/tap" >&2; else rm -rf "$TMP"; fi
}
trap cleanup EXIT

local_nodes() {
	export SIFT_TMP=$TMP SIFT_PORT=${SIFT_PORT:-$((56100 + RANDOM % 60))}
	"$REPO/scripts/pgtest.sh" install >/dev/null
	"$REPO/scripts/pgtest.sh" tap replication
}

# the image: PG_IMAGE plus the pg_sift sources of the working tree (tracked files), built and installed with make
container_nodes() {
	local ctx=$TMP/image major
	mkdir "$ctx"
	(cd "$REPO" && git ls-files -z Makefile pg_sift.control 'pg_sift--*.sql' src scripts/harden.mk | tar --null -T - -cf -) | tar -xf - -C "$ctx"
	cat >"$ctx/Dockerfile" <<-EOF
		FROM ${PG_IMAGE:-postgres:18}
		RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends build-essential liblz4-dev libzstd-dev postgresql-server-dev-\$PG_MAJOR
		COPY . /src
		RUN make -C /src -j4 USE_PGXS=1 install && rm -rf /src
	EOF
	nice docker build -q -t "$NET" "$ctx" >/dev/null
	docker network create "$NET" >/dev/null
	cd "$REPO/test"
	SIFT_NODES=docker SIFT_IMAGE=$NET SIFT_DOCKER_NET=$NET SIFT_TMP=$TMP prove ${PROVE_FLAGS} --exec bash t/replication.sh
	docker rmi "$NET" >/dev/null
}

case ${1:-} in
	'') local_nodes ;;
	--container) container_nodes ;;
	*) sed -n '2,11p' "$0"; exit 1 ;;
esac
