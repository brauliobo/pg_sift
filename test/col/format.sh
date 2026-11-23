# Sourced by scripts/coldiff.sh and scripts/coverage.sh: the format of the indexes a harness database creates (--format).
#   f2       Format 2.0 (the default of the release library); the testing build says sift.test_create_format1 = off
#   f1       Format 1 (the testing build only: sift.test_create_format1 = on)
#   release  the release build, which defines no test setting: none is read or set
# The caller defines sql (psql in the database that has pg_sift), and calls format_valid FORMAT and, once the database exists, format_apply FORMAT.

format_valid() { [[ $1 == f2 || $1 == f1 || $1 == release ]]; }

format_apply() {
	local fmt=$1 testing
	testing=$(sql -c "LOAD 'pg_sift'" -c "SELECT count(*) = 1 FROM pg_settings WHERE name = 'sift.test_create_format1'" | tail -1)
	case $fmt:$testing in
		release:f | f2:f) return 0 ;;
		release:t) echo "--format release needs the release build (SIFT_TESTING= scripts/pgtest.sh install): this library defines the test settings" >&2; exit 1 ;;
		f1:f) echo "--format f1 needs the testing build (scripts/pgtest.sh install): the release library creates Format 2.0 only" >&2; exit 1 ;;
	esac
	sql -c "DO \$\$ BEGIN EXECUTE format('ALTER DATABASE %I SET sift.test_create_format1 = %s', current_database(), '$([[ $fmt == f1 ]] && echo on || echo off)'); END \$\$"
}
