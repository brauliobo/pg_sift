#!/usr/bin/env bash
# Runs inside a throwaway container of the stock MariaDB image of one series (scripts/package-mariadb.sh):
#   container.sh build   build /src (mariadb/, src/, sqlite/host/ of a git ref) against the server headers of this image, which the
#                        MariaDB repository of the image pins to the very version of its server, into /out/lib/$SERIES/
#   container.sh test    unpack /out/$TARBALL and run the smoke test (/smoke.sh) against the server of this image
# Env: SERIES (for example 11.8), VER, HOST_UID (owner of the /out files), TARBALL (test).
set -euo pipefail

. /etc/os-release
# the library binds to the glibc of its build image; the floor of each series is printed and goes into the tarball (BUILD-INFO)
build() {
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq && apt-get install -y -qq --no-install-recommends build-essential libmariadbd-dev libmariadb-dev >/dev/null
	cp -r /src /build && cd /build/mariadb
	nice make -j4 OUT=/build/out SIFT_VERSION="$VER" /build/out/libsift_udf.so
	make isa-check OUT=/build/out SIFT_VERSION="$VER"
	strip --strip-unneeded /build/out/libsift_udf.so
	! objdump -p /build/out/libsift_udf.so | awk '/NEEDED/ {print $2}' | grep -Evx 'lib(m|c|pthread|dl)\.so\.[0-9]+|ld-linux-x86-64\.so\.2' ||
		{ echo "libsift_udf.so needs more than libc and libm" >&2; exit 1; }
	local server glibc dir=/out/lib/$SERIES
	server=$(mariadbd --version | sed -E 's/.*Ver ([^ ]*) .*/\1/')
	glibc=$(objdump -T /build/out/libsift_udf.so | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
	echo "series $SERIES: built against MariaDB $server headers on $PRETTY_NAME, requires ${glibc:-no versioned glibc symbol}"
	mkdir -p "$dir"
	install -m 755 /build/out/libsift_udf.so "$dir/libsift_udf.so"
	echo "$SERIES MariaDB $server ${glibc/GLIBC_/glibc >= } $PRETTY_NAME" > "$dir/BUILD-INFO"
	chown -R "$HOST_UID" /out/lib
}

test_tarball() {
	local t dir
	t=$(mktemp -d)
	tar xzf "/out/$TARBALL" -C "$t"
	dir=$(echo "$t"/*/)
	[[ -f ${dir}lib/$SERIES/libsift_udf.so && -f ${dir}sql/functions.sql && -f ${dir}sql/sift.sql && -f ${dir}LICENSE && -f ${dir}NOTICE &&
		-f ${dir}README.md && -f ${dir}BUILD-INFO ]] || { echo "the tarball lacks a file" >&2; ls -lR "$dir" >&2; exit 1; }
	echo "== $PRETTY_NAME: $(mariadbd --version)"
	bash /mdb/tests/smoke.sh "$dir" "$VER"
}

case $1 in build) build ;; test) test_tarball ;; esac
