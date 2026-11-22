#!/usr/bin/env bash
# One release, one set of PostgreSQL majors and architectures in every place that ships it, so that a package, an image, a repository or a tarball cannot be left behind
# (scripts/check-versions.sh keeps the version numbers; this keeps what is built for it):
#   majors          scripts/package.sh PGS = release.yml DOCKER_PGS = publish-repos.yml PGS = build-apt-dnf-repo.sh PGS = packaging/docker/build.sh tags =
#                   packaging/pgxman.yaml pgVersions = the postgresql@N dependencies of Formula/pg_sift.rb (less the majors Homebrew has no formula for: NO_BREW_PGS)
#   architectures   scripts/package.sh ARCHES and AMZN_PGS = publish-repos.yml = build-apt-dnf-repo.sh; the docker matrix builds each architecture natively;
#                   pgxman.yaml lists amd64 and arm64
#   images          release.yml pushes pg<N>, <X.Y.Z>-pg<N>, <X.Y>-pg<N> and <X>-pg<N> for the server and the -extension image; the Dockerfile labels the version
#   tarballs        release.yml builds the sqlite and mariadb tarballs for linux x86_64 and aarch64 (the two ports are Linux-only) and publish counts two of each
#   SECURITY.md     its table of supported versions is exactly `| MAJOR.MINOR.x | yes |` and `| older | no |` for the release (packaging/PKGBUILD), and the text that
#                   names the line says MAJOR.MINOR.x too: the newest minor line gets fixes (docs/SUPPORT.md)
#   placeholders    OWNER (the GitHub owner in docs/ and packaging/README.md) is replaced before a release
#   PostgreSQL 19   the GA release only: no release candidate in a recipe (19rc1, 19~rc1, 19-rc1); Vulkan stays opt-in (VULKAN=1 in the Makefile, in no recipe)
#
#   scripts/check-release-sync.sh [ROOT]    default: the repository
#   scripts/check-release-sync.sh --selftest
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail
export LC_ALL=C
NO_BREW_PGS=19   # Homebrew has no postgresql@19 formula yet: the formula lists the other majors

set_of() { tr -s ' ,\n"' '\n' | grep -v '^$' | sort -Vu | tr '\n' ' ' | sed 's/ $//'; }
dflt() { sed -n "s/^$2=\${$2:-\(.*\)}\$/\1/p" "$1" | head -n 1 | set_of; } # FILE VAR: the default of VAR=${VAR:-...}
yml_env() { sed -n "s/^  $2: '\(.*\)'\$/\1/p" "$1" | head -n 1 | set_of; } # FILE VAR: a workflow env value

check() { # ROOT: prints every mismatch; exits 1 if there is one
	local r=$1 pgs bad=() f got rel
	pgs=$(dflt "$r/scripts/package.sh" PGS)
	same() { [[ $2 == "$pgs" ]] || bad+=("$1: majors '$2', not '$pgs' (scripts/package.sh PGS)"); }
	same "release.yml DOCKER_PGS" "$(yml_env "$r/.github/workflows/release.yml" DOCKER_PGS)"
	same "publish-repos.yml PGS" "$(yml_env "$r/.github/workflows/publish-repos.yml" PGS)"
	same "build-apt-dnf-repo.sh PGS" "$(dflt "$r/packaging/repo/build-apt-dnf-repo.sh" PGS)"
	same "docker/build.sh tags" "$(sed -n 's/.*|| tags=(\(.*\))$/\1/p' "$r/packaging/docker/build.sh" | head -n 1 | set_of)"
	same "pgxman.yaml pgVersions" "$(sed -n '/^pgVersions:/,$p' "$r/packaging/pgxman.yaml" | sed -n 's/^  - "\(.*\)"$/\1/p' | set_of)"
	same "Formula postgresql@N (and the majors without a Homebrew formula: $NO_BREW_PGS)" "$({ sed -n 's/^ *depends_on "postgresql@\([0-9]*\)".*/\1/p' "$r/Formula/pg_sift.rb"; echo "$NO_BREW_PGS"; } | set_of)"
	for f in ARCHES AMZN_PGS; do
		got=$(dflt "$r/scripts/package.sh" $f)
		[[ $(yml_env "$r/.github/workflows/publish-repos.yml" $f) == "$got" ]] || bad+=("publish-repos.yml $f is not '$got' (scripts/package.sh)")
		[[ $(dflt "$r/packaging/repo/build-apt-dnf-repo.sh" $f) == "$got" ]] || bad+=("build-apt-dnf-repo.sh $f is not '$got' (scripts/package.sh)")
	done
	grep -q '^  - amd64$' "$r/packaging/pgxman.yaml" && grep -q '^  - arm64$' "$r/packaging/pgxman.yaml" || bad+=("packaging/pgxman.yaml arch is not amd64 and arm64")
	f=$r/.github/workflows/release.yml
	grep -q 'for arch in x86_64 aarch64' "$f" || bad+=("release.yml: the docker matrix does not build x86_64 and aarch64 natively")
	grep -q 'for v in "\$version" "\${version%.\*}" "\${version%%.\*}"' "$f" || bad+=("release.yml: the manifests lack the X.Y.Z-pgN, X.Y-pgN and X-pgN tags")
	grep -q 'tags=(-t "\$image:pg\$major")' "$f" || bad+=("release.yml: the manifests lack the pgN tag")
	grep -q '"\${IMAGE_REPO,,}" "\${IMAGE_REPO,,}-extension"' "$f" || bad+=("release.yml: the image-volume image is not pushed next to the server image")
	grep -q 'org.opencontainers.image.version="\${VERSION}"' "$r/packaging/docker/Dockerfile" || bad+=("packaging/docker/Dockerfile does not label the image with its version")
	[[ $(grep -c 'arch: \[x86_64, aarch64\]' "$f") == 2 ]] || bad+=("release.yml: the sqlite and mariadb jobs do not both build x86_64 and aarch64")
	[[ $(grep -c '\[ \${#\(sqlite\|mariadb\)\[@\]} -eq 2 \]' "$f") == 2 ]] || bad+=("release.yml: publish does not require two sqlite and two mariadb tarballs")
	rel=$(sed -n 's/^pkgver=//p' "$r/packaging/PKGBUILD")
	got=$(sed -n '/^## Supported versions$/,/^## [^S]/{/^| [0-9a-z]/p}' "$r/SECURITY.md")
	[[ $got == "$(printf '| %s.x | yes |\n| older | no |' "${rel%.*}")" ]] || bad+=("SECURITY.md: the table of supported versions is not '${rel%.*}.x yes, older no' (release $rel; docs/SUPPORT.md: the newest minor line gets fixes)")
	grep -q "newest minor line (${rel%.*}\.x)" "$r/SECURITY.md" || bad+=("SECURITY.md: the text before the report instructions does not name the newest minor line ${rel%.*}.x")
	got=$(cd "$r" && grep -rln 'OWNER' docs packaging/README.md 2>/dev/null || true)
	[[ -z $got ]] || bad+=("OWNER is not replaced by the GitHub owner in: $(echo "$got" | tr '\n' ' ')")
	grep -q 'VULKAN=1' "$r/Makefile" || bad+=("Makefile: the VULKAN=1 option is gone")
	got=$(grep -rEln 'VULKAN=0|SIFT_VULKAN=0' "$r/packaging" "$r/.github" "$r/scripts/package.sh" 2>/dev/null || true)
	[[ -z $got ]] || bad+=("a recipe switches Vulkan off or on: it is opt-in (VULKAN=1) and in no recipe")
	got=$(cd "$r" && grep -rEln --exclude-dir=releases '19rc1|19~rc1|19-rc1|19beta' packaging .github Formula docs scripts/package.sh 2>/dev/null || true)
	[[ -z $got ]] || bad+=("a PostgreSQL 19 release candidate is named in: $(echo "$got" | tr '\n' ' ')")
	((${#bad[@]} == 0)) || { printf 'check-release-sync: %s\n' "${bad[@]}" >&2; return 1; }
	echo "check-release-sync: ok, PostgreSQL $pgs in packages, images, repositories, pgxman and the formula; one tarball set for every architecture"
}

selftest() {
	local repo expect=0 cur next
	repo=$(cd "$(dirname "$0")/.." && pwd); cur=$(sed -n 's/^pkgver=//p' "$repo/packaging/PKGBUILD"); cur=${cur%.*}; next=${cur%%.*}.$((${cur#*.} + 1)); d=$(mktemp -d /tmp/sift-cbpkg-sync.XXXXXX); trap 'rm -rf "$d"' EXIT
	fixture() { # DIR: the files the check reads
		rm -rf "$1" && mkdir -p "$1/scripts" "$1/.github/workflows" "$1/packaging/docker" "$1/packaging/repo" "$1/Formula" "$1/docs"
		(cd "$repo" && cp scripts/package.sh "$1/scripts/" && cp SECURITY.md "$1/" && cp packaging/PKGBUILD "$1/packaging/" && cp .github/workflows/release.yml .github/workflows/publish-repos.yml "$1/.github/workflows/" &&
			cp packaging/pgxman.yaml "$1/packaging/" && cp packaging/docker/build.sh packaging/docker/Dockerfile "$1/packaging/docker/" &&
			cp packaging/repo/build-apt-dnf-repo.sh "$1/packaging/repo/" && cp Formula/pg_sift.rb "$1/Formula/" && cp Makefile "$1/") && echo ok >"$1/docs/INSTALL-REPOS.md"
	}
	mutate() { # NAME FILE SED: the fixture with one place changed must fail
		fixture "$d/t"; sed -i "$3" "$d/t/$2"
		if check "$d/t" >/dev/null 2>&1; then echo "check-release-sync selftest: '$1' passed" >&2; expect=1; fi
	}
	fixture "$d/t"; check "$d/t" >/dev/null || { check "$d/t"; echo "check-release-sync selftest: a consistent tree failed" >&2; exit 1; }
	mutate package-pgs scripts/package.sh 's/^PGS=${PGS:-17 18 19}/PGS=${PGS:-17 18}/'
	mutate docker-pgs .github/workflows/release.yml "s/DOCKER_PGS: '17 18 19'/DOCKER_PGS: '17 18'/"
	mutate repos-pgs .github/workflows/publish-repos.yml "s/  PGS: '17 18 19'/  PGS: '17 19'/"
	mutate repo-script packaging/repo/build-apt-dnf-repo.sh 's/^PGS=${PGS:-17 18 19}/PGS=${PGS:-18 19}/'
	mutate repos-arches .github/workflows/publish-repos.yml "s/  ARCHES: 'x86_64 aarch64'/  ARCHES: 'x86_64'/"
	mutate build-sh packaging/docker/build.sh 's/tags=(17 18 19)/tags=(17 18)/'
	mutate pgxman packaging/pgxman.yaml 's/^  - "19"$/  - "20"/'
	mutate pgxman-arch packaging/pgxman.yaml 's/^  - arm64$//'
	mutate formula Formula/pg_sift.rb 's/postgresql@18/postgresql@20/'
	mutate manifest-tags .github/workflows/release.yml 's/"\${version%.\*}" //'
	mutate label packaging/docker/Dockerfile 's/image.version=/image.v=/'
	mutate tarball-arch .github/workflows/release.yml '0,/arch: \[x86_64, aarch64\]/s//arch: [x86_64]/'
	mutate tarball-count .github/workflows/release.yml 's/\${#sqlite\[@\]} -eq 2/${#sqlite[@]} -eq 1/'
	mutate security SECURITY.md "s/^| $cur\\.x | yes |\$/| $next.x | yes |/"
	mutate security-two-lines SECURITY.md "s/^| older | no |\$/| $next.x | yes |\n| older | no |/"
	mutate security-older SECURITY.md 's/^| older | no |$/| older | yes |/'
	mutate security-text SECURITY.md "s/newest minor line ($cur\\.x)/newest minor line ($next.x)/"
	mutate security-two-newest SECURITY.md "s/newest minor line ($cur\\.x)/two newest minor releases/"
	fixture "$d/t"; sed -i "s/^pkgver=.*/pkgver=$next.0/" "$d/t/packaging/PKGBUILD"
	check "$d/t" >/dev/null 2>&1 && { echo "check-release-sync selftest: a $next.0 release with the $cur.x table passed" >&2; expect=1; }
	sed -i "s/$cur\\.x/$next.x/g" "$d/t/SECURITY.md"
	check "$d/t" >/dev/null 2>&1 || { check "$d/t"; echo "check-release-sync selftest: the $next.x table of a $next.0 release failed" >&2; expect=1; }
	sed -i "s/^| older | no |\$/| $cur.x | yes |\n| older | no |/" "$d/t/SECURITY.md"
	check "$d/t" >/dev/null 2>&1 && { echo "check-release-sync selftest: $next.x and $cur.x both supported passed" >&2; expect=1; }
	mutate owner docs/INSTALL-REPOS.md 's#ok#ghcr.io/OWNER/pg_sift#'
	mutate vulkan Makefile "s/VULKAN=1/VULKAN_X/g"
	fixture "$d/t"; echo 'postgres:19rc1' >>"$d/t/packaging/docker/Dockerfile"; mkdir -p "$d/t/docs"
	if check "$d/t" >/dev/null 2>&1; then echo "check-release-sync selftest: a 19rc1 passed" >&2; expect=1; fi
	((expect == 0)) || exit 1
	echo "check-release-sync selftest: ok"
}

case ${1:-} in
	--selftest) selftest ;;
	*) check "${1:-$(cd "$(dirname "$0")/.." && pwd)}" ;;
esac
