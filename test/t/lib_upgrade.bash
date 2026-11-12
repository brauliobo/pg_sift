# The shared part of the pg_upgrade tests (pg_upgrade.sh, col_track_revouch.sh), sourced after lib.bash: psql helpers, the server of a PG_CONFIG copied to its own
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
	local src=$TAPDIR/old-src testing=1
	[[ -z $OLD_REF ]] || testing=
	mkdir -p "$src"
	if [[ -n $OLD_REF ]]; then git -C "$REPO" archive "$OLD_REF" | tar -x -C "$src"
	else tar -C "$REPO" --exclude=.git --exclude='*.o' --exclude='*.d' --exclude='*.so' --exclude='*.dylib' --exclude='*.bc' --exclude=.build-flavor --exclude=bench/out -cf - . | tar -x -C "$src"; fi
	nice make -C "$src" -j4 USE_PGXS=1 PG_CONFIG="$1" SIFT_TESTING=$testing >"$TAPDIR/old-build.log" 2>&1 || {
		[[ -n $OLD_REF ]] || return 1
		echo "1..0 # SKIP the release $OLD_REF cannot be built for PostgreSQL $OLD_MAJOR"; return 77
	}
	make -C "$src" USE_PGXS=1 PG_CONFIG="$1" SIFT_TESTING=$testing DESTDIR="$2" install >>"$TAPDIR/old-build.log" 2>&1
}

# initdb_node NAME OFFSET BINDIR: the cluster every test starts from, with data checksums on both sides
initdb_node() {
	local d
	d=$(dir "$1")
	mkdir -p "$d/sock"
	echo "$2" >"$d/offset"
	"$3/initdb" -k --no-sync -D "$d/data" -A trust -U postgres -E UTF8 --no-locale --no-instructions --wal-segsize=1 >"$d/initdb.log" 2>&1 || bail "initdb $1 failed"
	node_conf "$1" "port = $(port "$1")
listen_addresses = ''
unix_socket_directories = '$d/sock'
fsync = off
shared_buffers = 32MB
max_connections = 30"
}
