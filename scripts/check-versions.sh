#!/usr/bin/env bash
# Every place that names a version agrees, so that a release cannot ship with one of them left behind:
#   extension version   pg_sift.control default_version (C) = SIFT_MODULE_VERSION in src/core/ops.h; sql/pg_sift--C.sql and an update script into C exist,
#                       and no script of sql/ names a version above C
#   release version     packaging/PKGBUILD pkgver (R) = packaging/pgxman.yaml (version and source URL) = META.json (version, provides version) =
#                       the README badge = sqlite/VERSION = mariadb/VERSION = a heading of CHANGELOG.md;
#                       the Formula URL names R or an earlier release (scripts/release.sh points it at the tag after the tag exists);
#                       C is the MAJOR.MINOR of R, or R itself (a patch release that moves the extension version)
#   META.json           provides file is sql/pg_sift--C.sql and the docfile exists
#   packaging           the spec takes its version from the build (%{pkg_version}) and the Debian control file carries none
#
#   scripts/check-versions.sh [ROOT]    default: the repository
#   scripts/check-versions.sh --selftest
set -euo pipefail
export LC_ALL=C

check() { # ROOT: prints every mismatch; exits 1 if there is one
	local r=$1 c m rel f newest bad=()
	c=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$r/pg_sift.control")
	rel=$(sed -n 's/^pkgver=//p' "$r/packaging/PKGBUILD")
	[[ -n $c && $rel =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "check-versions: no extension version in pg_sift.control or no X.Y.Z pkgver in packaging/PKGBUILD" >&2; return 1; }
	m=${rel%.*}
	[[ $c == "$m" || $c == "$rel" ]] || bad+=("pg_sift.control is $c, not $m or $rel (release $rel)")
	[[ $(sed -n 's/^#define SIFT_MODULE_VERSION[[:space:]]*"\(.*\)"/\1/p' "$r/src/core/ops.h") == "$c" ]] || bad+=("src/core/ops.h SIFT_MODULE_VERSION is not $c")
	for f in sqlite/VERSION mariadb/VERSION; do
		[[ $(<"$r/$f") == "$rel" ]] || bad+=("$f is $(<"$r/$f"), not $rel")
	done
	grep -q "^version: \"$rel\"$" "$r/packaging/pgxman.yaml" || bad+=("packaging/pgxman.yaml version is not $rel")
	grep -q "^source: .*/v$rel\.tar\.gz$" "$r/packaging/pgxman.yaml" || bad+=("packaging/pgxman.yaml source is not the tag v$rel")
	f=$(sed -n 's#^  url ".*/v\([0-9.]*\)\.tar\.gz"$#\1#p' "$r/Formula/pg_sift.rb")
	[[ -n $f && $(printf '%s\n' "$f" "$rel" | sort -V | tail -n 1) == "$rel" ]] || bad+=("Formula/pg_sift.rb url tag is '$f', not a release up to $rel")
	grep -q "release-$rel-blue" "$r/README.md" || bad+=("README.md badge is not $rel")
	grep -q "^## $rel - " "$r/CHANGELOG.md" || bad+=("CHANGELOG.md has no heading for $rel")
	[[ $(grep -c "^ *\"version\": \"$rel\",\?$" "$r/META.json") == 2 ]] || bad+=("META.json version and provides version are not both $rel")
	grep -q "\"file\": \"sql/pg_sift--$c.sql\"" "$r/META.json" || bad+=("META.json provides file is not sql/pg_sift--$c.sql")
	f=$(sed -n 's/^ *"docfile": "\(.*\)",\?$/\1/p' "$r/META.json")
	[[ -f $r/$f ]] || bad+=("META.json docfile '$f' does not exist")
	[[ -f $r/sql/pg_sift--$c.sql ]] || bad+=("sql/pg_sift--$c.sql does not exist")
	compgen -G "$r/sql/pg_sift--*--$c.sql" >/dev/null || [[ -d $r/sql/$c ]] || bad+=("no update script into $c in sql/ (sql/pg_sift--FROM--$c.sql or the parts of sql/$c/)")
	newest=$(ls "$r/sql" | sed -n 's/^pg_sift--\([0-9][0-9.]*\(--[0-9][0-9.]*\)*\)\.sql$/\1/p' | sed 's/--/\n/g' | sort -V | tail -n 1)
	[[ $(printf '%s\n' "$newest" "$c" | sort -V | tail -n 1) == "$c" ]] || bad+=("sql/ has a script that names $newest, newer than $c")
	grep -q '^Version: *%{pkg_version}$' "$r/packaging/pg_sift.spec" || bad+=("packaging/pg_sift.spec Version is not %{pkg_version}")
	! grep -q '^Version:' "$r/packaging/debian/control" || bad+=("packaging/debian/control carries a Version")
	((${#bad[@]} == 0)) || { printf 'check-versions: %s\n' "${bad[@]}" >&2; return 1; }
	echo "check-versions: ok, extension $c, release $rel in PKGBUILD, pgxman, META.json, Formula, README, CHANGELOG, sqlite and mariadb"
}

selftest() {
	local repo expect=0
	repo=$(cd "$(dirname "$0")/.." && pwd); d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
	fixture() { # DIR: the files the check reads, the sql/ names as empty files
		rm -rf "$1" && mkdir -p "$1/src/core" "$1/packaging/debian" "$1/Formula" "$1/sqlite" "$1/mariadb" "$1/sql" "$1/docs"
		(cd "$repo" && cp pg_sift.control META.json README.md CHANGELOG.md "$1/" && cp src/core/ops.h "$1/src/core/" && cp Formula/pg_sift.rb "$1/Formula/" &&
			cp packaging/PKGBUILD packaging/pgxman.yaml packaging/pg_sift.spec "$1/packaging/" && cp packaging/debian/control "$1/packaging/debian/" &&
			cp sqlite/VERSION "$1/sqlite/" && cp mariadb/VERSION "$1/mariadb/")
		touch "$1/docs/QUICKSTART.md" "$1/docs/USAGE.md"
		(cd "$repo/sql" && for f in pg_sift--[0-9]*.sql; do touch "$1/sql/$f"; done)
	}
	mutate() { # NAME FILE SED: the fixture with one place changed must fail
		fixture "$d/t"; sed -i.bak "$3" "$d/t/$2"; rm -f "$d/t/$2.bak"
		if check "$d/t" >/dev/null 2>&1; then echo "check-versions selftest: '$1' passed" >&2; expect=1; fi
	}
	fixture "$d/t"; check "$d/t" >/dev/null || { echo "check-versions selftest: a consistent tree failed" >&2; exit 1; }
	mutate control pg_sift.control "s/^default_version = .*/default_version = '9.9'/"
	mutate module src/core/ops.h 's/\(SIFT_MODULE_VERSION[[:space:]]*"\)[^"]*/\19.9/'
	mutate pkgver packaging/PKGBUILD 's/^pkgver=.*/pkgver=9.9.9/'
	mutate pgxman packaging/pgxman.yaml 's/^version: .*/version: "9.9.9"/'
	mutate pgxman-source packaging/pgxman.yaml 's#/v[0-9.]*\.tar\.gz#/v9.9.9.tar.gz#'
	mutate formula Formula/pg_sift.rb 's#/v[0-9.]*\.tar\.gz#/v9.9.9.tar.gz#'
	mutate meta META.json 's/"version": "[0-9.]*"/"version": "9.9.9"/'
	mutate meta-file META.json 's#sql/pg_sift--[0-9.]*\.sql#sql/pg_sift--3.3.sql#'
	mutate badge README.md 's/release-[0-9.]*-blue/release-9.9.9-blue/'
	mutate changelog CHANGELOG.md 's/^## \([0-9][0-9.]*\) - /## x\1 - /'
	mutate sqlite sqlite/VERSION 's/.*/9.9.9/'
	mutate mariadb mariadb/VERSION 's/.*/9.9.9/'
	mutate spec packaging/pg_sift.spec 's/^Version:.*/Version: 9.9.9/'
	fixture "$d/t"; touch "$d/t/sql/pg_sift--5.1--99.0.sql"
	if check "$d/t" >/dev/null 2>&1; then echo "check-versions selftest: a script newer than the control passed" >&2; expect=1; fi
	((expect == 0)) || exit 1
	echo "check-versions selftest: ok"
}

case ${1:-} in
	--selftest) selftest ;;
	*) check "${1:-$(cd "$(dirname "$0")/.." && pwd)}" ;;
esac
