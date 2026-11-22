#!/usr/bin/env bash
# Smoke-test a pg_sift server image (packaging/docker/Dockerfile): start it, wait until it accepts TCP connections (the entrypoint first runs a
# temporary server that listens on its socket only), check that a new database has the extension from the initialization script, and run
# packaging/smoke.sql in a database made from template0.
#
#   scripts/docker-smoke.sh IMAGE
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
image=${1:?usage: docker-smoke.sh IMAGE}
ver=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$REPO/pg_sift.control")
c=$(docker run -d --rm -e POSTGRES_PASSWORD=example "$image")
trap 'docker stop "$c" >/dev/null' EXIT
psql() { docker exec -i "$c" psql -U postgres -X -q -v ON_ERROR_STOP=1 "$@"; }

timeout 120 bash -c "until docker exec $c pg_isready -q -h 127.0.0.1; do sleep 0.5; done" || { docker logs "$c" >&2; exit 1; }
[[ $(psql -At -c "SELECT count(*) FROM pg_extension WHERE extname = 'pg_sift'") == 1 ]] || { echo "docker-smoke: $image has no pg_sift in its first database" >&2; exit 1; }
psql -c 'CREATE DATABASE smoke TEMPLATE template0'
psql -v from="$ver" -v to="$ver" -d smoke <"$REPO/packaging/smoke.sql"
echo "docker-smoke: ok, $image runs pg_sift $ver"
