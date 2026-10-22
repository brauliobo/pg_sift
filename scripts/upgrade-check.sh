#!/usr/bin/env bash
# Upgrade preflight: runs sift.upgrade_check() (scripts/upgrade_check.sql) in each database and prints what to look at before
# ALTER EXTENSION pg_sift UPDATE, a library swap or a downgrade. Nothing is changed: the function is loaded in a transaction that is rolled back,
# so a database whose extension is older than this checkout can be checked too (run it as a superuser, on the primary, with the new library files
# installed).
#
#   scripts/upgrade-check.sh [--to RELEASE] [--strict] [--all | DATABASE_OR_CONNINFO...]
#
#   --to RELEASE   also report what that release could not read (a downgrade, or a standby that stays on it), e.g. --to 3.1
#   --strict       exit 1 on a warning too (default: on a blocker only)
#   --all          every database that accepts connections
#
# The connection comes from the usual PGHOST, PGPORT and PGUSER. Exit 0 when nothing blocks, 1 when a database has a blocker (or a warning with
# --strict), 2 when a database could not be checked.
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

rc=0
for db in "${dbs[@]}"; do
	has=$(psql -X -At -d "$db" -c "SELECT 1 FROM pg_extension WHERE extname = 'pg_sift'") || { echo "== $db: cannot connect" >&2; rc=2; continue; }
	[[ -n $has ]] || { echo "== $db: no pg_sift, skipped"; continue; }
	out=$(psql -X -q -At -F $'\t' -v ON_ERROR_STOP=1 -v to="$to" -d "$db" <<SQL
BEGIN;
\i $HERE/upgrade_check.sql
SELECT status, object, kind, detected, replace(coalesce(advice, ''), E'\n', ' ')
FROM sift.upgrade_check(nullif(:'to', ''))
ORDER BY CASE status WHEN 'blocker' THEN 0 WHEN 'warn' THEN 1 ELSE 2 END, object;
ROLLBACK;
SQL
	) || { echo "== $db: the check failed" >&2; rc=2; continue; }
	echo "== $db"
	printf 'STATUS\tOBJECT\tKIND\tDETECTED\tADVICE\n%s\n' "$out" | { column -t -s $'\t' 2>/dev/null || cat; }
	grep -q $'^blocker\t' <<<"$out" && { ((rc == 2)) || rc=1; }
	[[ -z $strict ]] || ! grep -q $'^warn\t' <<<"$out" || { ((rc == 2)) || rc=1; }
done
exit $rc
