#!/usr/bin/env bash
# The signed APT and DNF repositories of a release, as one static tree for GitHub Pages.
#
#   packaging/repo/build-apt-dnf-repo.sh build PKGDIR SITEDIR    PKGDIR holds the .deb and .rpm files of the release (scripts/package.sh names)
#   packaging/repo/build-apt-dnf-repo.sh verify SITEDIR          every package of PGS x ARCHES is there, listed, hashed and signed
#   packaging/repo/build-apt-dnf-repo.sh --selftest              builds and verifies a repository of empty packages with a throwaway key
#
# SITEDIR/apt    pool/main/p/pg-sift/*.deb, dists/stable/{Release,InRelease,Release.gpg,main/binary-{amd64,arm64}/Packages{,.gz}}, pg_sift.gpg
# SITEDIR/rpm    el/{x86_64,aarch64}/ and amzn2023/{x86_64,aarch64}/ (each a createrepo_c repository, repomd.xml signed, the .rpm signed), pg_sift-*.repo
# SITEDIR/pg_sift.asc   the public key (ASCII armor)
#
# Environment: GPG_KEY_ID (the signing key, in GNUPGHOME), GPG_PASSPHRASE (empty: no passphrase), BASE_URL (where SITEDIR is served, for the .repo files),
# PGS, ARCHES, AMZN_PGS (the defaults of scripts/package.sh: the majors and architectures a release must carry), SUITE (stable), SOURCE_DATE_EPOCH (the
# date of Release and of the repodata revision, so that the same packages give the same files).
set -euo pipefail
export LC_ALL=C

PGS=${PGS:-17 18 19}
ARCHES=${ARCHES:-x86_64 aarch64}
AMZN_PGS=${AMZN_PGS:-17 18}
SUITE=${SUITE:-stable}
BASE_URL=${BASE_URL:-https://OWNER.github.io/pg_sift}
SDE=${SOURCE_DATE_EPOCH:-0}
POOL=pool/main/p/pg-sift

die() { echo "build-apt-dnf-repo: $*" >&2; exit 1; }
deb_arch() { case $1 in x86_64) echo amd64 ;; aarch64) echo arm64 ;; *) die "unknown architecture $1" ;; esac; }
rpm_family() { case $1 in *.el9.*) echo el ;; *.amzn2023.*) echo amzn2023 ;; *) die "$1 is neither an el9 nor an amzn2023 package" ;; esac; }
rpm_arch() { local a=${1%.rpm}; echo "${a##*.}"; }
date_rfc() { date -u -d "@$SDE" '+%a, %d %b %Y %H:%M:%S UTC'; }

PASSFILE=''
SCRATCH=''
init_passphrase() { # the passphrase goes through a private file, never an argument that ps shows
	[[ -z ${GPG_PASSPHRASE:-} ]] && return 0
	PASSFILE=$(mktemp) && chmod 600 "$PASSFILE" && printf '%s' "$GPG_PASSPHRASE" >"$PASSFILE" && trap 'rm -f "$PASSFILE"' EXIT
}
gpg_sign() {
	local a=(--batch --yes --quiet --pinentry-mode loopback --default-key "${GPG_KEY_ID:?GPG_KEY_ID names the signing key}")
	[[ -z $PASSFILE ]] || a+=(--passphrase-file "$PASSFILE")
	gpg "${a[@]}" "$@"
}

apt_build() { # PKGDIR SITE
	local pkgs=$1 apt=$2/apt arch dist
	dist=$apt/dists/$SUITE
	mkdir -p "$apt/$POOL"
	find "$pkgs" -maxdepth 1 -name '*.deb' -print0 | sort -z | xargs -0 -r cp -t "$apt/$POOL"
	for arch in $ARCHES; do
		arch=$(deb_arch "$arch")
		mkdir -p "$dist/main/binary-$arch"
		(cd "$apt" && dpkg-scanpackages --multiversion --arch "$arch" pool /dev/null 2>/dev/null) >"$dist/main/binary-$arch/Packages"
		gzip -n -9 -c "$dist/main/binary-$arch/Packages" >"$dist/main/binary-$arch/Packages.gz"
	done
	{
		echo "Origin: pg_sift"; echo "Label: pg_sift"; echo "Suite: $SUITE"; echo "Codename: $SUITE"; echo "Date: $(date_rfc)"
		echo "Architectures: $(for a in $ARCHES; do deb_arch "$a"; done | tr '\n' ' ' | sed 's/ $//')"; echo "Components: main"
		echo "Description: pg_sift packages for PostgreSQL ${PGS// /, }"; echo "SHA256:"
		(cd "$dist" && find main -type f | sort | while read -r f; do printf ' %s %d %s\n' "$(sha256sum <"$f" | cut -d' ' -f1)" "$(stat -c %s "$f")" "$f"; done)
	} >"$dist/Release"
	gpg_sign --clearsign -o "$dist/InRelease" "$dist/Release"
	gpg_sign --armor --detach-sign -o "$dist/Release.gpg" "$dist/Release"
	gpg --batch --export "$GPG_KEY_ID" >"$apt/pg_sift.gpg"
}

rpm_sign() { # FILE...
	local f
	for f in "$@"; do
		rpmsign --addsign --define "_gpg_name $GPG_KEY_ID" --define "__gpg_sign_cmd %{__gpg} gpg --batch --no-verbose --no-armor --pinentry-mode loopback \
${PASSFILE:+--passphrase-file $PASSFILE }--no-secmem-warning -u \"%{_gpg_name}\" -sbo %{__signature_filename} --digest-algo sha256 %{__plaintext_filename}" "$f" >/dev/null
	done
}

rpm_build() { # PKGDIR SITE
	local pkgs=$1 rpm=$2/rpm f fam arch dir
	mkdir -p "$rpm"
	while IFS= read -r -d '' f; do
		fam=$(rpm_family "$(basename "$f")"); arch=$(rpm_arch "$(basename "$f")")
		mkdir -p "$rpm/$fam/$arch" && cp "$f" "$rpm/$fam/$arch/"
	done < <(find "$pkgs" -maxdepth 1 -name '*.rpm' -print0 | sort -z)
	for dir in "$rpm"/*/*/; do
		rpm_sign "$dir"*.rpm
		createrepo_c --quiet --no-database --revision "$SDE" --set-timestamp-to-revision "$dir"
		gpg_sign --armor --detach-sign -o "$dir/repodata/repomd.xml.asc" "$dir/repodata/repomd.xml"
	done
	for fam in el amzn2023; do
		[[ -d $rpm/$fam ]] || continue
		cat >"$rpm/pg_sift-$fam.repo" <<EOF
[pg_sift]
name=pg_sift for $fam
baseurl=$BASE_URL/rpm/$fam/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$BASE_URL/pg_sift.asc
EOF
	done
	gpg --batch --armor --export "$GPG_KEY_ID" >"$2/pg_sift.asc"
}

build() { # PKGDIR SITE
	[[ -d $1 ]] || die "$1 is not a directory"
	init_passphrase
	rm -rf "$2" && mkdir -p "$2"
	apt_build "$1" "$2"
	rpm_build "$1" "$2"
	[[ -s $2/pg_sift.asc ]] || gpg --batch --armor --export "$GPG_KEY_ID" >"$2/pg_sift.asc"
	echo "built $2: $(find "$2" -name '*.deb' | wc -l) deb, $(find "$2" -name '*.rpm' | wc -l) rpm"
}

need() { [[ -e $1 ]] || bad+=("missing $1"); } # bad is the caller's array

verify_apt() { # SITE
	local apt=$1/apt dist pg arch darch f want
	dist=$apt/dists/$SUITE
	gpgv --keyring "$apt/pg_sift.gpg" "$dist/InRelease" >/dev/null 2>&1 || bad+=("InRelease is not signed by apt/pg_sift.gpg")
	gpgv --keyring "$apt/pg_sift.gpg" "$dist/Release.gpg" "$dist/Release" >/dev/null 2>&1 || bad+=("Release.gpg does not verify Release")
	for arch in $ARCHES; do
		darch=$(deb_arch "$arch")
		for f in Packages Packages.gz; do
			need "$dist/main/binary-$darch/$f"
			want=$(sha256sum <"$dist/main/binary-$darch/$f" | cut -d' ' -f1)
			grep -q " $want .* main/binary-$darch/$f\$" "$dist/Release" || bad+=("Release lists another hash for main/binary-$darch/$f")
		done
		for pg in $PGS; do
			f=$(find "$apt/$POOL" -name "postgresql-$pg-pg-sift_*_$darch.deb" | sort -V | tail -n 1)
			[[ -n $f ]] || { bad+=("no postgresql-$pg-pg-sift deb for $darch"); continue; }
			grep -q "^Filename: $POOL/$(basename "$f")\$" "$dist/main/binary-$darch/Packages" || bad+=("$(basename "$f") is not in Packages")
			grep -q "^SHA256: $(sha256sum <"$f" | cut -d' ' -f1)\$" "$dist/main/binary-$darch/Packages" || bad+=("Packages has another SHA256 for $(basename "$f")")
		done
	done
}

verify_rpm() { # SITE
	local rpm=$1/rpm tmp pg arch fam pgs f
	tmp=$(mktemp -d) && mkdir "$tmp/gnupg" && chmod 700 "$tmp/gnupg"
	rpmkeys --dbpath "$tmp/db" --import "$1/pg_sift.asc"
	GNUPGHOME=$tmp/gnupg gpg --batch --quiet --import "$1/pg_sift.asc" 2>/dev/null
	for fam in el amzn2023; do
		pgs=$PGS; [[ $fam == amzn2023 ]] && pgs=$AMZN_PGS
		for arch in $ARCHES; do
			need "$rpm/$fam/$arch/repodata/repomd.xml"
			GNUPGHOME=$tmp/gnupg gpg --batch --verify "$rpm/$fam/$arch/repodata/repomd.xml.asc" "$rpm/$fam/$arch/repodata/repomd.xml" >/dev/null 2>&1 ||
				bad+=("repomd.xml of $fam/$arch is not signed by pg_sift.asc")
			for pg in $pgs; do
				f=$(find "$rpm/$fam/$arch" -name "pg_sift_$pg-*.rpm" | sort -V | tail -n 1)
				[[ -n $f ]] || { bad+=("no pg_sift_$pg rpm for $fam/$arch"); continue; }
				rpmkeys --dbpath "$tmp/db" -K "$f" 2>&1 | grep 'signatures OK\|OK$' >/dev/null || bad+=("$(basename "$f") is not signed by pg_sift.asc")
			done
		done
		need "$rpm/pg_sift-$fam.repo"
	done
	rm -rf "$tmp"
}

verify() { # SITE
	local bad=()
	need "$1/pg_sift.asc"
	verify_apt "$1"
	verify_rpm "$1"
	((${#bad[@]} == 0)) || { printf 'build-apt-dnf-repo: %s\n' "${bad[@]}" >&2; return 1; }
	echo "verify: $1 has PostgreSQL ${PGS// /, } for ${ARCHES// /, } (rpm for Amazon Linux 2023: ${AMZN_PGS// /, }), signed"
}

fingerprint() { # SITE: the files that do not carry a signature time or a compressor stamp
	(cd "$1" && sha256sum apt/dists/$SUITE/main/*/Packages* apt/dists/$SUITE/Release rpm/*/*/repodata/*primary.xml.gz 2>/dev/null)
}

selftest() {
	local t pg arch name v=9.9.9
	SCRATCH=$(mktemp -d /tmp/sift-cbpkg-repo.XXXXXX); t=$SCRATCH; trap 'rm -rf "$SCRATCH"' EXIT
	export GNUPGHOME=$t/gnupg; mkdir -m 700 "$GNUPGHOME"
	gpg --batch --quiet --passphrase '' --quick-generate-key 'pg_sift selftest <selftest@example.com>' rsa3072 sign never 2>/dev/null
	GPG_KEY_ID=$(gpg --batch --list-keys --with-colons | awk -F: '/^fpr/ {print $10; exit}'); export GPG_KEY_ID
	export SOURCE_DATE_EPOCH=1700000000 BASE_URL=https://example.com/pg_sift GPG_PASSPHRASE=
	mkdir -p "$t/pkgs"
	for pg in $PGS; do
		for arch in $ARCHES; do
			name=postgresql-$pg-pg-sift_$v-1_$(deb_arch "$arch")
			mkdir -p "$t/d/$name/DEBIAN"
			printf 'Package: postgresql-%s-pg-sift\nVersion: %s-1\nArchitecture: %s\nMaintainer: Example <maintainer@example.com>\nDescription: selftest\n' \
				"$pg" "$v" "$(deb_arch "$arch")" >"$t/d/$name/DEBIAN/control"
			dpkg-deb --build "$t/d/$name" "$t/pkgs/$name.deb" >/dev/null
			selftest_rpm "$t" "$pg" "$arch" el9 "$v" || return 1
			[[ " $AMZN_PGS " != *" $pg "* ]] || selftest_rpm "$t" "$pg" "$arch" amzn2023 "$v" || return 1
		done
	done
	build "$t/pkgs" "$t/site"
	verify "$t/site"
	build "$t/pkgs" "$t/site2" >/dev/null
	[[ $(fingerprint "$t/site") == "$(fingerprint "$t/site2")" ]] || die "two builds of the same packages differ"
	[[ -z ${SELFTEST_KEEP:-} ]] || cp -a "$t/site" "$SELFTEST_KEEP"
	rm "$t"/site/apt/pool/main/p/pg-sift/postgresql-18-pg-sift_"$v"-1_arm64.deb
	! verify "$t/site" >/dev/null 2>&1 || die "a missing package passed verify"
	echo "selftest: ok (signed apt and dnf repositories of $(echo $PGS | wc -w) majors x $(echo $ARCHES | wc -w) architectures, reproducible, a missing package is refused)"
}

selftest_rpm() { # TMP PG ARCH FAMILY VERSION: an empty signed-later package of that name
	local t=$1 pg=$2 arch=$3 fam=$4 v=$5 spec
	spec=$t/rpm-$pg-$fam-$arch.spec
	printf 'Name: pg_sift_%s\nVersion: %s\nRelease: 1.%s\nSummary: selftest\nLicense: PostgreSQL\n%%description\nselftest\n%%files\n' "$pg" "$v" "$fam" >"$spec"
	rpmbuild -bb --quiet --target "$arch-linux" --define "_topdir $t/rpmtop" --define "_rpmdir $t/pkgs" --define '_build_name_fmt %{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}.rpm' "$spec" >/dev/null ||
		die "rpmbuild failed for pg_sift_$pg $fam $arch"
}

case ${1:-} in
	build) [[ $# == 3 ]] || die "usage: build PKGDIR SITEDIR"; build "$2" "$3" ;;
	verify) [[ $# == 2 ]] || die "usage: verify SITEDIR"; verify "$2" ;;
	--selftest) selftest ;;
	*) sed -n '2,19p' "$0" | sed 's/^# \?//'; exit 2 ;;
esac
