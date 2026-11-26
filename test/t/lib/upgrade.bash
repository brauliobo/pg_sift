# The shared part of the pg_upgrade tests (pg_upgrade.sh, col_track_revouch.sh), sourced after common.bash: psql helpers, the server of a PG_CONFIG copied to its own
# absolute paths with pg_sift next to it (overlay), the build of pg_sift for another major (build_old: REPO, TAPDIR, OLD_REF), and the cluster both sides start from.
q() { local n=$1 db=$2; shift 2; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$n")/sock" -p "$(port "$n")" -U postgres -d "$db" "${@/#/--command=}"; }
# qs NODE DB: the script on stdin, one statement at a time (CREATE TABLESPACE cannot share a transaction)
qs() { PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$1")/sock" -p "$(port "$1")" -U postgres -d "$2" -f -; }
quiet() { "$@" >/dev/null; }
PG_BINS="postgres initdb pg_ctl pg_upgrade vacuumdb pg_resetwal pg_controldata pg_dump pg_dumpall pg_restore pg_checksums pg_config psql"

# overlay PG_CONFIG DEST ROOT: the server of PG_CONFIG at its absolute paths under DEST, pg_sift of the install ROOT in it
overlay() {
	local bin lib share f
	bin=$("$1" --bindir); lib=$("$1" --pkglibdir); share=$("$1" --sharedir)
	mkdir -p "$2$bin" "$2$lib" "$2$share/extension"
	for f in $PG_BINS; do cp "$bin/$f" "$2$bin/"; done
	for f in "$lib"/*; do ln -s "$f" "$2$lib/"; done
	for f in "$share"/*; do [[ $f == */extension ]] || ln -s "$f" "$2$share/"; done
	for f in "$share"/extension/*; do ln -s "$f" "$2$share/extension/"; done
	[[ -f $3$lib/pg_sift$DLSUFFIX ]] || return 0   # PostgreSQL 17 has no staged root: sift is in the server's own directories
	rm -f "$2$lib/pg_sift$DLSUFFIX" "$2$share"/extension/pg_sift*
	cp "$3$lib/pg_sift$DLSUFFIX" "$2$lib/"
	cp "$3$share"/extension/pg_sift* "$2$share/extension/"
}

# build_old PG_CONFIG ROOT: pg_sift for another major, built in a copy of this tree (the tree under test keeps its objects); OLD_MAJOR names that major.
# The copy of this tree is the testing library like the staged one (the SQL of the test sets sift.test_* settings on both sides); a release is the release library.
# Returns 77 after the SKIP line when the compile of a tagged release (OLD_REF) fails: a release cannot follow a newer major; any other failure is 1
build_old() {
	local src=$TAPDIR/old-src testing=1 cache= tmp
	[[ -z $OLD_REF ]] || testing=
	# a release is the same files for the same commit and major: with SIFT_OLD_BUILD_CACHE (a directory) its install is kept there and copied, not compiled again
	[[ -z $OLD_REF || -z ${SIFT_OLD_BUILD_CACHE:-} ]] || cache=$SIFT_OLD_BUILD_CACHE/$(git -C "$REPO" rev-parse "$OLD_REF^{commit}")-$("$1" --version | tr -c 'A-Za-z0-9.\n' _)
	[[ -z $cache || ! -d $cache ]] || { mkdir -p "$2" && cp -a "$cache/." "$2/"; return; }
	mkdir -p "$src"
	if [[ -n $OLD_REF ]]; then git -C "$REPO" archive "$OLD_REF" | tar -x -C "$src"
	else tar -C "$REPO" --exclude=.git --exclude='*.o' --exclude='*.d' --exclude='*.so' --exclude='*.dylib' --exclude='*.bc' --exclude=.build-flavor --exclude=bench/out -cf - . | tar -x -C "$src"; fi
	nice env -u SIFT_BUILD_DIR -u SIFT_BUILD_ROOT make -C "$src" -j4 USE_PGXS=1 PG_CONFIG="$1" SIFT_TESTING=$testing >"$TAPDIR/old-build.log" 2>&1 || {
		[[ -n $OLD_REF ]] || return 1
		echo "1..0 # SKIP the release $OLD_REF cannot be built for PostgreSQL $OLD_MAJOR"; return 77
	}
	env -u SIFT_BUILD_DIR -u SIFT_BUILD_ROOT make -C "$src" USE_PGXS=1 PG_CONFIG="$1" SIFT_TESTING=$testing DESTDIR="$2" install >>"$TAPDIR/old-build.log" 2>&1 || return 1
	[[ -z $cache ]] || { mkdir -p "${cache%/*}" && tmp=$(mktemp -d "${cache%/*}/.tmp.XXXXXX") && cp -a "$2/." "$tmp/" && { mv -T "$tmp" "$cache" 2>/dev/null || rm -rf "$tmp"; }; }
}

# checksum_template BINDIR: the data directory initdb of BINDIR makes, with data checksums: made once for the binaries of BINDIR under $SIFT_TMP/initdb-k, copied from then on
checksum_template() {
	local t=$SIFT_TMP/initdb-k/$(cat "$1/initdb" "$1/postgres" | cksum | cut -d' ' -f1) tmp
	[[ -d $t ]] && { echo "$t"; return; }
	mkdir -p "${t%/*}" && tmp=$(mktemp -d "$t.XXXXXX") || return 1
	"$1/initdb" -k --no-sync -D "$tmp" -A trust -U postgres -E UTF8 --no-locale --no-instructions --wal-segsize=1 >"$TAPDIR/initdb-k.log" 2>&1 || { rm -rf "$tmp"; return 1; }
	mv -T "$tmp" "$t" 2>/dev/null || rm -rf "$tmp"
	echo "$t"
}

# initdb_node NAME OFFSET BINDIR: the cluster every test starts from, with data checksums on both sides
initdb_node() {
	local d t
	d=$(dir "$1")
	mkdir -p "$d/sock"
	echo "$2" >"$d/offset"
	t=$(checksum_template "$3") && cp -a "$t" "$d/data" || bail "initdb $1 failed"
	node_conf "$1" "port = $(port "$1")
listen_addresses = ''
unix_socket_directories = '$d/sock'
fsync = off
shared_buffers = 32MB
max_connections = 30"
}
