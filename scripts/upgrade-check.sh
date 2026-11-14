#!/usr/bin/env bash
# Upgrade preflight for several databases: runs sift.upgrade_check() in each and prints what to look at before
# ALTER EXTENSION pg_sift UPDATE, a library swap or a downgrade. Nothing is changed: the function is loaded from sql/pg_sift_upgrade_check.sql (built from
# sql/5.0/27-upgrade-check.sql, installed beside the extension scripts as pg_sift_upgrade_check.sql) in a transaction that is rolled back, so a database
# whose extension is older than this checkout can be checked too (run it as a superuser, on the primary, with the new library files installed).
# From 5.0 on, SELECT * FROM sift.upgrade_check() in the database does the same.
#
#   scripts/upgrade-check.sh [--to RELEASE] [--strict] [--all | DATABASE_OR_CONNINFO...]
#
#   --to RELEASE   also report what that release could not read (a downgrade, or a standby that stays on it), e.g. --to 3.1
#   --strict       exit 1 on a warning too (default: on a blocker only)
#   --all          every database that accepts connections
#
# The connection comes from the usual PGHOST, PGPORT and PGUSER. Exit 0 when nothing blocks, 1 when a database has a blocker (or a warning with
# --strict), 2 when a database could not be checked.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
to=; strict=; all=; dbs=()
while (($#)); do
	case $1 in
		--to) to=${2:?--to needs a release}; shift ;;
		--strict) strict=1 ;;
		--all) all=1 ;;
		-h | --help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
		-*) echo "upgrade-check: unknown option $1" >&2; exit 2 ;;
		*) dbs+=("$1") ;;
	esac
	shift
done
[[ -n $all ]] && mapfile -t dbs < <(psql -X -At -d postgres -c "SELECT datname FROM pg_database WHERE datallowconn AND NOT datistemplate ORDER BY 1")
((${#dbs[@]})) || { echo "usage: scripts/upgrade-check.sh [--to RELEASE] [--strict] [--all | DATABASE...]" >&2; exit 2; }

"$HERE/sql-parts.sh" build
FILE=$HERE/../sql/pg_sift_upgrade_check.sql
rc=0
for db in "${dbs[@]}"; do
	has=$(psql -X -At -d "$db" -c "SELECT 1 FROM pg_extension WHERE extname = 'pg_sift'") || { echo "== $db: cannot connect" >&2; rc=2; continue; }
	[[ -n $has ]] || { echo "== $db: no pg_sift, skipped"; continue; }
	out=$(psql -X -q -At -F $'\t' -v ON_ERROR_STOP=1 -v to="$to" -d "$db" -f "$FILE") || { echo "== $db: the check failed" >&2; rc=2; continue; }
	echo "== $db"
	printf 'STATUS\tOBJECT\tKIND\tDETECTED\tADVICE\n%s\n' "$out" | { column -t -s $'\t' 2>/dev/null || cat; }
	grep -q $'^blocker\t' <<<"$out" && { ((rc == 2)) || rc=1; }
	[[ -z $strict ]] || ! grep -q $'^warn\t' <<<"$out" || { ((rc == 2)) || rc=1; }
done
exit $rc
