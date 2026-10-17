#!/usr/bin/env bash
# Runs inside a throwaway container (scripts/package-sqlite.sh):
#   container.sh build   build /src (sqlite/, src/, LICENSE, NOTICE of a git ref) into /out/sift-sqlite-$VER-linux-x86_64.tar.gz
#   container.sh test    unpack /out/$TARBALL in a fresh directory and run the smoke test (/smoke.py) against the system SQLite
# Env: VER, EPOCH (source date), HOST_UID (owner of the /out files), TARBALL (test).
set -euo pipefail

command -v apt-get >/dev/null && FMT=deb || FMT=rpm
# shellcheck source=/dev/null
. /etc/os-release
NAME=sift-sqlite-$VER-linux-x86_64
# the oldest glibc the build image carries is the oldest one the extension runs on; the build fails when it needs a newer one
MAX_GLIBC=${MAX_GLIBC:-2.34}

install_pkgs() {
	if [[ $FMT == deb ]]; then
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq && apt-get install -y -qq --no-install-recommends "$@" >/dev/null
	else
		dnf -y -q install "$@" >/dev/null
	fi
}

build() {
	if [[ $FMT == deb ]]; then install_pkgs build-essential libsqlite3-dev; else install_pkgs gcc make binutils sqlite-devel; fi
	cp -r /src /build && cd /build/sqlite
	nice make -j4 OPT=-O2 SIFT_VERSION="$VER" build/sift.so
	make isa-check SIFT_VERSION="$VER"
	strip --strip-unneeded build/sift.so
	local glibc; glibc=$(objdump -T build/sift.so | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
	echo "requires ${glibc:-no versioned glibc symbol}, built on $PRETTY_NAME"
	[[ -z $glibc || $(printf '%s\n' "${glibc#GLIBC_}" "$MAX_GLIBC" | sort -V | tail -1) == "$MAX_GLIBC" ]] ||
		{ echo "sift.so needs $glibc, newer than the supported $MAX_GLIBC" >&2; exit 1; }
	! objdump -p build/sift.so | awk '/NEEDED/ {print $2}' | grep -Evx 'lib(m|c|pthread|dl)\.so\.[0-9]+|ld-linux-x86-64\.so\.2' ||
		{ echo "sift.so needs more than libc and libm" >&2; exit 1; }

	local dir=/tmp/pkg/$NAME
	mkdir -p "$dir"
	install -m 755 build/sift.so "$dir/sift.so"
	install -m 644 ../LICENSE ../NOTICE "$dir"
	install -m 644 README.md "$dir/README.md"
	# reproducible: sorted names, the commit time, no owner, no gzip name or time
	tar --sort=name --mtime="@$EPOCH" --owner=0 --group=0 --numeric-owner -C /tmp/pkg -cf - "$NAME" | gzip -n -9 > "/out/$NAME.tar.gz"
	chown "$HOST_UID" "/out/$NAME.tar.gz"
}

test_tarball() {
	if [[ $FMT == deb ]]; then install_pkgs python3 sqlite3; else install_pkgs python3 sqlite; fi
	local t; t=$(mktemp -d)
	tar xzf "/out/$TARBALL" -C "$t"
	local dir; dir=$(echo "$t"/*/)
	[[ -x ${dir}sift.so && -f ${dir}LICENSE && -f ${dir}NOTICE && -f ${dir}README.md ]] || { echo "the tarball lacks a file" >&2; ls -l "$dir" >&2; exit 1; }
	echo "== $PRETTY_NAME: sqlite $(sqlite3 --version | cut -d' ' -f1), python $(python3 -c 'import sys; print(sys.version.split()[0])')"
	python3 -I /smoke.py "${dir}sift.so" "$VER"
	SIFT_SIMD=scalar python3 -I /smoke.py "${dir}sift.so" "$VER"
}

case $1 in build) build ;; test) test_tarball ;; esac
