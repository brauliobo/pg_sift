#!/bin/sh
# Builds pg_sift on the musl libc and proves it loads there. Runs as root inside postgres:<major>-alpine with the repository at /src (read only):
# musl has no GNU IFUNC, so a library with an IFUNC symbol or an IRELATIVE relocation fails at dlopen (scripts/check-no-ifunc.sh); then the
# library is installed in the image's server and packaging/smoke.sql runs on it (the FMV dispatch of src/core keeps the target_clones, which are IFUNCs, off musl).
#
#   docker run --rm -v "$PWD:/src:ro" postgres:18-alpine sh /src/scripts/musl-check.sh
set -eu
DLSUFFIX=.so

apk add --no-cache build-base lz4-dev zstd-dev icu-dev bash su-exec python3 coreutils >/dev/null
cp -r /src /build && cd /build
ver=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" pg_sift.control)
nice make -j4 USE_PGXS=1 SIFT_LTO=1
scripts/check-no-ifunc.sh "pg_sift$DLSUFFIX"
make USE_PGXS=1 SIFT_LTO=1 install

data=/tmp/pgdata
mkdir "$data" && chown postgres "$data"
su-exec postgres initdb -D "$data" >/dev/null
su-exec postgres pg_ctl -D "$data" -o "-c listen_addresses=''" -w start >/dev/null
su-exec postgres psql -X -q -v ON_ERROR_STOP=1 -v from="$ver" -v to="$ver" -d postgres -f packaging/smoke.sql
echo "musl-check: ok, pg_sift $ver loads and passes packaging/smoke.sql on $(. /etc/os-release && echo "$PRETTY_NAME") (musl)"
