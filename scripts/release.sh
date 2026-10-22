#!/usr/bin/env bash
# The release, from a green head to the published assets and the AUR, in one command (docs/RELEASE.md).
#
#   scripts/release.sh VERSION [--dry-run] [--resume]
#
# It does not run the test matrix again: GitHub CI is the gate. In this order, stopping at the first failure:
#   preflight  clean tree on main, equal to origin/main; the notes (docs/releases/vVERSION.md), CHANGELOG, control and SQL scripts are in place
#   checks     at once: ci (a green run of the workflow `ci` with "Full scope" on a commit whose build inputs are HEAD's: waited for when it runs,
#              dispatched when none exists), upgrade (the previous release's library writes a BM25 index, a sift_col index and a sift_table, this
#              tree takes them over, ALTER EXTENSION UPDATE, verify, REINDEX; then the previous library reads what this one writes in the previous
#              column format), package (make install DESTDIR, the files, packaging/smoke.sql from the first and from the last version)
#   bump       CHANGELOG, sqlite/mariadb CHANGELOG and VERSION, PKGBUILD, README badge, install lines: one commit, after a check that every place agrees
#   publish    push main and the tag, watch the workflow `release`, verify the assets (names, SHA256SUMS), then scripts/release-aur.sh
# A step that passed is in $RELEASE_STATE/VERSION/manifest (default ~/.local/state/pg_sift-release): --resume skips it. --dry-run does it all
# on a scratch clone and pushes, tags and publishes nothing (the AUR is cloned over https, the diff shown); a failed run says what to run.
# Environment: RELEASE_TMP (scratch, default ~/.cache), SIFT_PORT (first port of two, default 56210), AUR_SSH_KEY, SSH_ASKPASS (release-aur.sh).
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
VERSION='' DRY='' RESUME=''
for a in "$@"; do
	case $a in --dry-run) DRY=1 ;; --resume) RESUME=1 ;; -h|--help) sed -n '2,19p' "$0"; exit 0 ;; -*) sed -n '2,19p' "$0" >&2; exit 2 ;; *) VERSION=$a ;; esac
done
[[ $VERSION =~ ^([0-9]+)\.([0-9]+)\.[0-9]+$ ]] || { echo "release: the version is MAJOR.MINOR.PATCH" >&2; exit 2; }
MM=${BASH_REMATCH[1]}.${BASH_REMATCH[2]} TAG=v$VERSION
# the whole run in 2 cores and 12 GB, whatever the caller did
if [[ -z ${SIFT_RELEASE_SCOPED:-} ]] && command -v systemd-run >/dev/null; then
	exec systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 -p OOMPolicy=continue -p CPUQuota=200% env SIFT_RELEASE_SCOPED=1 "$0" "$@"
fi

STATE=${RELEASE_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/pg_sift-release}/$VERSION${DRY:+.dry}
MAN=$STATE/manifest PORT0=${SIFT_PORT:-56210}
# the build inputs of the shipped files: a commit that changes none of them is covered by the CI run of the one before (notes, versions and docs are not)
GUARDED=(src test scripts Makefile pg_sift.control 'sql/pg_sift--*.sql' sqlite mariadb ':!*.md' ':!*/VERSION' ':!scripts/release*')
log() { printf '[%s] %s\n' "$(date +%T)" "$*" >&2; }
die() { echo "release: $*" >&2; exit 1; }
soft() { if [[ -n $DRY ]]; then log "dry run, would stop: $*"; else die "$*"; fi; }	# a precondition of the real release, a note in a dry run

if [[ -f $MAN && -z $RESUME ]]; then die "$MAN exists: --resume continues that release, removing $STATE starts over"; fi
mkdir -p "$STATE"
step() { # NAME KEY CMD...: CMD, unless the manifest has the step passed with that key and this is a --resume
	local n=$1 k=$2 t=$SECONDS rc=0
	shift 2
	if [[ -n $RESUME ]] && cut -f1-3 "$MAN" 2>/dev/null | grep -qxF "$n"$'\t'pass$'\t'"$k"; then log "$n: passed before"; return 0; fi
	log "$n"
	"$@" || rc=$?
	printf '%s\t%s\t%s\t%ss\t%s\n' "$n" "$( (( rc )) && echo fail || echo pass)" "$k" $((SECONDS - t)) "$(date '+%F %T')" >>"$MAN"
	(( rc == 0 )) || log "$n failed (rc $rc); the logs are in $STATE/logs, --resume continues"
	return $rc
}

W=$(mktemp -d "${RELEASE_TMP:-$HOME/.cache}/sift-release.XXXXXX") && mkdir -p "$STATE/logs" || exit 1
trap 'cp -f "$W"/*.log "$STATE/logs/" 2>/dev/null; rm -rf "$W"' EXIT
if [[ -n $DRY ]]; then R=$W/repo; git clone -q "$REPO" "$R" && git -C "$R" remote set-url origin "$(git -C "$REPO" remote get-url origin)" && git -C "$R" remote set-url --push origin no-push://dry-run; else R=$REPO; fi
g() { git -C "$R" "$@"; }
tree() { mkdir -p "$2" && git -C "$R" archive "$1" | tar -x -C "$2"; }	# REF DIR: a tree without a checkout
pgt() { local d=$1 tmp=$2 port=$3; shift 3; ( cd "$d" && SIFT_TMP=$tmp SIFT_PORT=$port scripts/pgtest.sh "$@" ); }
q() { local sock=$1 port=$2 db=$3; shift 3; psql -X -q -At -v ON_ERROR_STOP=1 -h "$sock" -p "$port" -U postgres -d "$db" "$@"; }
control_version() { sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$R/pg_sift.control"; }

# ---------- preflight
preflight() {
	local notes=docs/releases/$TAG.md f pm
	[[ -z $(g status --porcelain) ]] || { g status --short; die "the tree is not clean"; }
	g fetch -q --tags origin main
	g rev-parse -q --verify "refs/tags/$TAG" >/dev/null && [[ -z $RESUME ]] && die "$TAG exists"
	[[ $(g rev-parse --abbrev-ref HEAD) == main ]] || soft "not on main"
	# HEAD is origin/main; a resumed release may be one commit ahead (the bump) or already pushed
	[[ -n $RESUME ]] || [[ $(g rev-parse HEAD) == "$(g rev-parse origin/main)" ]] || soft "HEAD is not origin/main: push or pull first"
	[[ -f $R/$notes ]] || soft "write $notes first (docs/releases/TEMPLATE.md)"
	! grep -qsE 'X\.Y\.Z|<one line|<feature>' "$R/$notes" || soft "$notes still has template text"
	for f in sqlite mariadb; do [[ ! -f $R/$f/VERSION ]] || [[ ! -f $R/$notes ]] || grep -qi "^## $f extension" "$R/$notes" || soft "$notes has no '$f' section"; done
	[[ -n $RESUME ]] || sed -n '/^## Unreleased/,/^## [0-9]/p' "$R/CHANGELOG.md" | grep -q '^- ' || soft "CHANGELOG.md has nothing under Unreleased"
	[[ $(control_version) == "$MM" ]] || die "pg_sift.control default_version is $(control_version), not $MM"
	[[ -f $R/sql/pg_sift--$MM.sql ]] || die "sql/pg_sift--$MM.sql is missing"
	PREV=$(g describe --tags --abbrev=0 --match 'v*' --exclude "$TAG" HEAD) || die "no previous release tag"
	# the upgrade script from the previous release's version to this one (a patch release keeps the version)
	pm=$(g show "$PREV:pg_sift.control" | sed -n "s/^default_version *= *'\(.*\)'/\1/p")
	[[ $pm == "$MM" ]] || [[ -f $R/sql/pg_sift--$pm--$MM.sql ]] || die "sql/pg_sift--$pm--$MM.sql is missing: the upgrade from $PREV has no script"
	# the guarded tree: the last commit that changed a build input; CI evidence is any run on it or on a later commit
	GCOMMIT=$(g log -1 --format=%H -- "${GUARDED[@]}")
}

# ---------- the checks
# ci_runs: the runs of the workflow ci on the guarded commit and the ones after it: id, status, conclusion, event (a run on an older commit is not evidence)
ci_runs() {
	local shas; shas=" $(g rev-list "$GCOMMIT^..HEAD" | tr '\n' ' ') "
	gh run list --workflow ci.yml --branch main --limit 100 --json databaseId,headSha,status,conclusion,event |
		jq -r --arg shas "$shas" '.[] | .headSha as $h | select($shas | contains(" " + $h + " ")) | [.databaseId, .status, .conclusion, .event, .headSha] | @tsv'
}
check_ci() {
	local id st concl ev sha wait='' dispatched='' n
	for n in 1 2 3 4 5 6; do
		wait=
		while IFS=$'\t' read -r id st concl ev sha; do
			if [[ $st == completed && $concl == success ]]; then
				gh run view "$id" --json jobs --jq '[.jobs[] | select(.name == "changes") | .steps[] | select(.name == "Full scope" and .conclusion == "success")] | length' | grep -qx 1 || continue
				echo "ci: run $id ($ev, ${sha:0:7}) is green with full scope"; return 0
			elif [[ $st != completed && $ev != schedule ]]; then wait=$id; fi
		done < <(ci_runs)
		if [[ -n $wait ]]; then echo "ci: waiting for run $wait"; gh run watch "$wait" --exit-status >/dev/null 2>&1 || echo "ci: run $wait did not pass"; continue; fi
		[[ -z $dispatched ]] || { echo "ci: the full run is not green: gh run list --workflow ci.yml"; return 1; }
		if [[ -n $DRY ]]; then echo "ci: no green full-scope run on ${GCOMMIT:0:7} or later: a release dispatches one now (gh workflow run ci.yml --ref main)"; return 0; fi
		[[ $(g rev-parse HEAD) == "$(g rev-parse origin/main)" ]] || { echo "ci: no green full-scope run, and HEAD is not origin/main to dispatch one"; return 1; }
		gh workflow run ci.yml --ref main >/dev/null && dispatched=1
		for id in $(seq 12); do sleep 5; [[ -z $(ci_runs | awk -F'\t' '$2 != "completed"') ]] || break; done
	done
	echo "ci: no green full-scope run on the guarded commit ${GCOMMIT:0:7} or later"; return 1
}

DATA="CREATE EXTENSION pg_sift;
	CREATE TABLE docs (id int PRIMARY KEY, g int, n int, body text);
	INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 30000) i;
	CREATE INDEX docs_idx ON docs USING sift (body);
	CREATE INDEX docs_col ON docs USING sift_col (id, g, n);
	CREATE TABLE tbl (id int, g int, n int) USING sift_table;
	INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(1, 30000) i;
	VACUUM docs; VACUUM tbl;"
ANSC="SET sift.enable_agg = on; SET cpu_tuple_cost = 0.5;
	SELECT g, count(*), sum(n) FROM docs GROUP BY g ORDER BY g;
	SELECT g, count(*), sum(n) FROM tbl GROUP BY g ORDER BY g;
	SELECT count(*) FROM tbl WHERE id % 7 = 3;"
ANS="SELECT count(*) FROM docs WHERE body @@> 'alpha3 beta4';
	SELECT count(*), sum(n) FROM docs WHERE body @@> 'beta7 NOT alpha1';
	$ANSC"
check_upgrade() {
	local U=$W/upgrade sock=$W/upgrade/sock port=$PORT0 a b ver min
	export SIFT_TESTING=
	tree "$PREV" "$W/old"; tree HEAD "$W/new"
	pgt "$W/old" "$U" $port install >/dev/null && pgt "$W/old" "$U" $port start >/dev/null || return 1
	q "$sock" $port sift <<<"$DATA"
	a=$(q "$sock" $port sift <<<"$ANS"); pgt "$W/old" "$U" $port stop >/dev/null
	# this tree's files replace the previous release's in the same cluster
	pgt "$W/new" "$U" $port install >/dev/null && pgt "$W/new" "$U" $port start >/dev/null || return 1
	q "$sock" $port sift -c "ALTER EXTENSION pg_sift UPDATE"
	ver=$(q "$sock" $port sift -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")
	[[ $ver == "$MM" ]] || { echo "extversion is $ver, not $MM"; return 1; }
	[[ $(q "$sock" $port sift <<<"$ANS") == "$a" ]] || { echo "the answers changed by the update"; return 1; }
	q "$sock" $port sift -c "SELECT sift.verify('docs_idx', true), sift.col_verify('docs_col', true)" -c "REINDEX INDEX docs_idx" -c "REINDEX INDEX docs_col" >/dev/null
	[[ $(q "$sock" $port sift <<<"$ANS") == "$a" ]] || { echo "the answers changed by REINDEX"; return 1; }
	# the format the previous release reads: the column format this library writes when asked (the BM25 segment format has no such
	# setting: the previous release refuses a newer one by design, so the BM25 index is dropped first).
	# The setting exists once a function of the library ran in the session.
	min=$(q "$sock" $port sift <<<"SELECT sift.verify('docs_idx', true); SELECT min_val FROM pg_settings WHERE name = 'sift.col_format_minor'" | tail -1) || true
	if [[ -n $min ]]; then
		q "$sock" $port sift <<<"DROP INDEX docs_idx; SET sift.col_format_minor = $min; REINDEX INDEX docs_col; INSERT INTO docs SELECT i, i % 9, i % 100, 'x' FROM generate_series(30001, 30500) i; VACUUM docs"
		q "$sock" $port sift <<<"SELECT sift.col_verify('docs_col', true)" >/dev/null
		b=$(q "$sock" $port sift <<<"$ANSC"); pgt "$W/new" "$U" $port stop >/dev/null
		pgt "$W/old" "$U" $port install >/dev/null && pgt "$W/old" "$U" $port start >/dev/null || return 1
		[[ $(q "$sock" $port sift <<<"$ANSC") == "$b" ]] || { echo "$PREV answers differently on the data of the previous column format (minor $min)"; return 1; }
		q "$sock" $port sift <<<"SELECT sift.col_verify('docs_col', true)" >/dev/null
		pgt "$W/old" "$U" $port stop >/dev/null
		echo "upgrade ok: $PREV to $VERSION, and $PREV reads the column minor $min this tree writes"
	else echo "no sift.col_format_minor: the previous-format check is skipped"; pgt "$W/new" "$U" $port stop >/dev/null; fi
}
check_package() {
	local U=$W/package sock=$W/package/sock port=$((PORT0 + 10)) root=$W/package/root f from ver=$MM
	tree HEAD "$W/pkg"
	( cd "$W/pkg" && nice make -j2 USE_PGXS=1 SIFT_TESTING= >/dev/null && make USE_PGXS=1 SIFT_TESTING= DESTDIR="$root" install >/dev/null ) || { echo "make install failed"; return 1; }
	for f in "$R/pg_sift.control" "$R"/sql/pg_sift--*.sql; do
		cmp "$f" "$root$(pg_config --sharedir)/extension/${f##*/}" || { echo "the install target did not preserve ${f##*/}"; return 1; }
	done
	[[ -f $root$(pg_config --pkglibdir)/pg_sift.so && ! -e $root$(pg_config --sharedir)/extension/pg_sift_test.control ]] || { echo "the library is missing, or the test-only extension is installed"; return 1; }
	( cd "$W/pkg" && SIFT_TMP=$U SIFT_ROOT=$root SIFT_PORT=$port scripts/pgtest.sh start >/dev/null ) || return 1
	for from in "$ver" 1.0; do
		q "$sock" $port postgres -c "CREATE DATABASE smoke_${from/./_}"
		q "$sock" $port "smoke_${from/./_}" -v from="$from" -v to="$ver" -f "$R/packaging/smoke.sql" >/dev/null || { echo "packaging/smoke.sql failed from $from"; return 1; }
	done
	( cd "$W/pkg" && SIFT_TMP=$U SIFT_PORT=$port scripts/pgtest.sh stop >/dev/null ) && echo "package ok: the installed files create $ver and update from 1.0"
}
# versions: every place that names the release agrees (VERSION: the check after the bump, where the files carry it)
check_versions() {
	local f bad=()
	[[ $(control_version) == "$MM" ]] || bad+=("pg_sift.control")
	[[ $(sed -n 's/^pkgver=//p' "$R/packaging/PKGBUILD") == "$VERSION" ]] || bad+=("packaging/PKGBUILD pkgver")
	for f in sqlite mariadb; do [[ ! -f $R/$f/VERSION ]] || [[ $(<"$R/$f/VERSION") == "$VERSION" ]] || bad+=("$f/VERSION"); done
	grep -q "release-$VERSION-blue" "$R/README.md" || bad+=("README.md badge")
	grep -q "^version: \"$VERSION\"" "$R/packaging/pgxman.yaml" || bad+=("packaging/pgxman.yaml")
	[[ $(grep -c "\"version\": \"$VERSION\"" "$R/META.json") == 2 ]] || bad+=("META.json")
	grep -q "^## $VERSION - " "$R/CHANGELOG.md" || bad+=("CHANGELOG.md heading")
	(( ${#bad[@]} == 0 )) || { echo "not at $VERSION: ${bad[*]}"; return 1; }
}
checks() {
	local n names=(ci upgrade package) rc=0
	( exec 9>/tmp/sift-ci.lock; flock -n 9 || { log "waiting for /tmp/sift-ci.lock"; flock 9; }
	  check_upgrade >"$W/upgrade.log" 2>&1 & p1=$!; check_package >"$W/package.log" 2>&1 & p2=$!
	  r=0; wait $p1 || r=1; wait $p2 || r=1; exit $r ) &
	local local_pid=$!
	check_ci >"$W/ci.log" 2>&1 || rc=1
	wait "$local_pid" || rc=1
	for n in "${names[@]}"; do echo "== $n"; tail -5 "$W/$n.log"; done
	return $rc
}

# ---------- the bump
bump() {
	local old date oldre f; old=$(sed -n 's/^pkgver=//p' "$R/packaging/PKGBUILD") date=$(date +%F) oldre=${old//./\\.}
	[[ $(g log -1 --format=%s) == "release: $VERSION"* ]] && return 0
	sed -i "s/^## Unreleased\$/## Unreleased\n\n## $VERSION - $date/" "$R/CHANGELOG.md"
	for f in sqlite mariadb; do
		[[ -f $R/$f/VERSION ]] || continue
		echo "$VERSION" >"$R/$f/VERSION"
		# the lines under Unreleased move under the new heading; an empty section gets one line
		awk -v v="$VERSION" 'function fill() { if (!has) print "- Rebuilt with the PostgreSQL release " v ".\n"; inu = 0 }
			/^## Unreleased$/ { print; print ""; print "## " v; inu = 1; next }
			inu && /^## / { fill() }
			inu && /[^[:space:]]/ { has = 1 }
			{ print } END { if (inu) fill() }' "$R/$f/CHANGELOG.md" >"$W/changelog" && cp "$W/changelog" "$R/$f/CHANGELOG.md"
	done
	sed -i "s/^pkgver=.*/pkgver=$VERSION/; s/^pkgrel=.*/pkgrel=1/" "$R/packaging/PKGBUILD"
	sed -i "s/release-$oldre-blue/release-$VERSION-blue/" "$R/README.md"
	sed -i "s/$oldre/$VERSION/g" "$R/packaging/pgxman.yaml" "$R/META.json"
	sed -i "s/\([^0-9.]\)$oldre\([^0-9]\)/\1$VERSION\2/g" "$R/docs/USAGE.md"
	sed -i "s/^V=$oldre\$/V=$VERSION/" "$R/sqlite/README.md"
	check_versions || return 1
	g add CHANGELOG.md README.md docs/USAGE.md packaging/PKGBUILD packaging/pgxman.yaml META.json sqlite/README.md
	for f in sqlite mariadb; do [[ ! -f $R/$f/VERSION ]] || g add "$f/VERSION" "$f/CHANGELOG.md"; done
	g commit -q -m "release: $VERSION

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
}

# ---------- publish
publish() {
	if [[ -n $DRY ]]; then g tag -f -a "$TAG" -m "pg_sift $VERSION"; echo "dry run: $(g rev-parse --short HEAD) would be pushed to main and tagged $TAG"; return 0; fi
	[[ -z $(g ls-remote origin "refs/tags/$TAG") ]] || return 0
	g rev-parse -q --verify "refs/tags/$TAG" >/dev/null || g tag -a "$TAG" -m "pg_sift $VERSION"
	g push -q --atomic origin HEAD:refs/heads/main "refs/tags/$TAG"
}
workflow() {
	local id n
	[[ -z $DRY ]] || { echo "dry run: the release workflow of $TAG is not started"; return 0; }
	for n in $(seq 60); do
		id=$(gh run list --workflow release.yml --branch "$TAG" --limit 1 --json databaseId --jq '.[0].databaseId')
		[[ -z $id ]] || break
		sleep 5
	done
	[[ -n $id ]] || { echo "no release workflow run for $TAG"; return 1; }
	gh run watch "$id" --exit-status >/dev/null || { echo "the release workflow failed: gh run view $id --log-failed"; return 1; }
}
assets() { # the downloads are the files the workflow built: names with the version, every one in SHA256SUMS
	local d=$W/assets f n
	[[ -z $DRY ]] || { echo "dry run: nothing is published to download"; return 0; }
	mkdir -p "$d" && gh release download "$TAG" -D "$d" || return 1
	( cd "$d" && sha256sum -c SHA256SUMS ) || return 1
	ls "$d"/*pg-sift_"$VERSION"-*.deb "$d"/*pg_sift*-"$VERSION"-*.rpm >/dev/null || { echo "a package of $VERSION is missing"; ls "$d"; return 1; }
	for f in sqlite mariadb; do
		[[ ! -f $R/$f/VERSION ]] || [[ -f $d/sift-$f-$VERSION-linux-x86_64.tar.gz ]] || { echo "the $f tarball of $VERSION is missing"; return 1; }
	done
	for n in "$d"/*.deb "$d"/*.rpm "$d"/*.tar.gz; do grep -q "$(basename "$n")" "$d/SHA256SUMS" || { echo "$(basename "$n") is not in SHA256SUMS"; return 1; }; done
}
aur() {
	[[ -z $DRY ]] && { "$R/scripts/release-aur.sh"; return; }
	# no key and a script that knows --dry-run: an older one would push
	grep -q -- --dry-run "$R/scripts/release-aur.sh" || { echo "dry run: $R/scripts/release-aur.sh has no --dry-run, skipped"; return 0; }
	env -u SSH_AUTH_SOCK -u AUR_SSH_KEY "$R/scripts/release-aur.sh" --dry-run
}

preflight
T0=$SECONDS
step checks "$GCOMMIT" checks || exit 1
step bump "$VERSION" bump || exit 1
step publish "$VERSION" publish || exit 1
step workflow "$VERSION" workflow || exit 1
step assets "$VERSION" assets || exit 1
step aur "$VERSION" aur || exit 1
printf 'done\t%s\t%ss\n' "$VERSION" $((SECONDS - T0)) >>"$MAN"
echo "release: $VERSION ${DRY:+(dry run) }done in $((SECONDS - T0)) s"
