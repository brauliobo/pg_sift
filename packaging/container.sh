#!/usr/bin/env bash
# Runs inside a throwaway container (scripts/package.sh): the PostgreSQL server and its development files come from PGDG (Amazon Linux 2023: from the distribution).
#   container.sh build   build the package of /src/pg_sift-$VER.tar.gz into /out
#   container.sh test    install /out/$PKG, smoke-test it (fresh install and 1.0 upgrade), remove it
# /pkg is the packaging/ directory. Env: PG (the major, build; the test reads it from the package name), VER, EPOCH (source date),
# HOST_UID (owner of /out files), PKG (test).
set -euo pipefail

JOBS=4
command -v apt-get >/dev/null && FMT=deb || FMT=rpm
# shellcheck source=/dev/null
. /net.sh
. /etc/os-release

pgdg_repo() {
	[[ $ID != amzn ]] || return 0 # Amazon Linux 2023: the distribution has postgresql17 and 18 (no 19), no PGDG repository for it
	if [[ $FMT == deb ]]; then
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends ca-certificates curl gnupg lsb-release postgresql-common
		sed -i 's/^#\?create_main_cluster.*/create_main_cluster = false/' /etc/postgresql-common/createcluster.conf
		# the component of the major too: a major still in beta or release candidate (19 today) lives only there
		/usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y -c "$PG" >/dev/null
	else
		local major=${VERSION_ID%%.*}
		dnf -y -q install "https://download.postgresql.org/pub/repos/yum/reporpms/EL-$major-$(uname -m)/pgdg-redhat-repo-latest.noarch.rpm"
		# only the repo of the version built: a broken signature of another major's metadata must not fail the build; a major not
		# released yet (19 today) has only its testing repo, which is enabled then
		local keep=pgdg$PG
		grep -q "^\[$keep\]" /etc/yum.repos.d/pgdg-redhat-all.repo || keep=$keep-updates-testing
		awk -v keep="[$keep]" '/^\[/ {sec = $0; off = sec != keep && sec ~ /^\[pgdg[0-9]+\]$/} /^enabled *=/ {if (sec == keep) $0 = "enabled=1"; else if (off) $0 = "enabled=0"} 1' \
			/etc/yum.repos.d/pgdg-redhat-all.repo > /tmp/pgdg.repo && mv /tmp/pgdg.repo /etc/yum.repos.d/pgdg-redhat-all.repo
		(( major >= 10 )) || dnf -y -q module disable postgresql # the AppStream module would shadow PGDG before EL 10
	fi
}

build_deb() {
	apt-get install -y -qq --no-install-recommends build-essential fakeroot debhelper postgresql-server-dev-$PG liblz4-dev libzstd-dev libicu-dev
	cp -r /pkg/debian debian
	sed -i "s/@PG@/$PG/g" debian/*
	cat > debian/changelog <<LOG
pg-sift ($VER-1) unstable; urgency=medium

  * Release $VER.

 -- Braulio Oliveira <brauliobo@gmail.com>  $(date -R -d "@$EPOCH")
LOG
	nice dpkg-buildpackage -b -us -uc -j$JOBS >/dev/null
	cp ../*.deb /out
}

build_rpm() {
	local crb=--enablerepo=crb devel=postgresql$PG-devel
	[[ $ID != amzn ]] || { crb=; devel=postgresql$PG-server-devel; }
	dnf -y -q $crb install rpm-build gcc make redhat-rpm-config $devel lz4-devel libzstd-devel libicu-devel
	nice rpmbuild -bb /pkg/pg_sift.spec --define "pgmajor $PG" --define "pkg_version $VER" --define "_sourcedir /src" \
		--define "_topdir /tmp/rpmbuild" --define "_smp_mflags -j$JOBS" >/dev/null
	cp /tmp/rpmbuild/RPMS/*/*.rpm /out
}

build() {
	mkdir /build && tar xzf "/src/pg_sift-$VER.tar.gz" -C /build --strip-components=1 && cd /build
	build_$FMT
	chown -R "$HOST_UID" /out
}

install_pkg() { if [[ $FMT == deb ]]; then apt-get install -y -qq -o Dpkg::Use-Pty=0 --no-install-recommends "$1"; else dnf -y -q install "$1"; fi; }
remove_pkg()  { if [[ $FMT == deb ]]; then apt-get remove -y -qq "$1"; else dnf -y -q remove "$1"; fi; }
pkg_name()    { if [[ $FMT == deb ]]; then dpkg-deb -f "$1" Package; else rpm -qp --qf '%{NAME}' "$1"; fi; }

test_pkg() {
	local pkg=/out/$PKG name; name=$(pkg_name "$pkg")
	install_pkg "$pkg"
	local bin=/usr/lib/postgresql/$PG/bin libdir=/usr/lib/postgresql/$PG/lib share=/usr/share/postgresql/$PG ver t
	[[ $FMT == deb ]] || { bin=/usr/pgsql-$PG/bin libdir=/usr/pgsql-$PG/lib share=/usr/pgsql-$PG/share; }
	[[ $ID != amzn ]] || { bin=/usr/bin libdir=/usr/lib64/pgsql share=/usr/share/pgsql; }
	export PATH=$bin:$PATH
	ver=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$share/extension/pg_sift.control")
	t=$(mktemp -d) && chown postgres "$t"
	pg() { runuser -u postgres -- env PATH="$PATH" PGHOST="$t" PGPORT=55432 "$@"; }

	echo "== $PRETTY_NAME: $name $(postgres --version), pg_sift $ver"
	pg initdb -D "$t/data" -A trust --no-sync >/dev/null
	pg pg_ctl -D "$t/data" -l "$t/log" -w -o "-c listen_addresses= -c unix_socket_directories=$t -c port=55432" start >/dev/null
	for from in "$ver" 1.0; do
		pg createdb "from_$from"
		pg psql -X -q -v ON_ERROR_STOP=1 -v from="$from" -v to="$ver" -d "from_$from" -f /pkg/smoke.sql
		echo "ok: create at $from, update to $ver"
	done
	pg pg_ctl -D "$t/data" -m fast -w stop >/dev/null
	remove_pkg "$name"
	[[ ! -e $libdir/pg_sift.so && ! -e $share/extension/pg_sift.control ]] || { echo "files left after removal" >&2; exit 1; }
	echo "ok: removed $name"
}

[[ $1 == build ]] || { PG=$(pkg_name "/out/$PKG"); PG=${PG//[^0-9]/}; } # the major of postgresql-17-pg-sift, pg_sift_17
: "${PG:?}"
pgdg_repo
case $1 in build) build ;; test) test_pkg ;; esac
