#!/usr/bin/env bash
# CREATE EXTENSION and ALTER EXTENSION UPDATE end with a notice and the exact ALTER DATABASE command when pg_sift is in neither shared_preload_libraries nor
# session_preload_libraries; the command silences it; pg_sift.install_notice = off (the setting of the test clusters) silences it too.
. "$(dirname "$0")/lib.bash"
plan 10

node_init n 0
node_start n || bail "n did not start"
db() { PGOPTIONS="-c client_min_messages=notice ${2:-}" psql -X -At -h "$(dir n)/sock" -p "$(port n)" -U postgres -d postgres -c "$1" 2>&1; }
HINT="ALTER DATABASE postgres SET session_preload_libraries = 'pg_sift';"

out=$(db "CREATE EXTENSION pg_sift" "-c pg_sift.install_notice=on")
check "CREATE EXTENSION says the library is not preloaded" grep -qF 'NOTICE:  pg_sift is not preloaded in database "postgres": the first statement of each session is planned twice' <<<"$out"
check "and gives the command that preloads it" grep -qF "HINT:  $HINT" <<<"$out"

db "DROP EXTENSION pg_sift" >/dev/null
out=$(db "CREATE EXTENSION pg_sift")
check "the setting of the test clusters (pg_sift.install_notice = off) silences it" bash -c '! grep -q "not preloaded" <<<"$1"' _ "$out"

db "DROP EXTENSION pg_sift" >/dev/null
db "CREATE EXTENSION pg_sift VERSION '4.3'" >/dev/null
out=$(db "ALTER EXTENSION pg_sift UPDATE" "-c pg_sift.install_notice=on")
check "ALTER EXTENSION UPDATE says it too" grep -qF "HINT:  $HINT" <<<"$out"

db "DROP EXTENSION pg_sift" >/dev/null
db "CREATE EXTENSION pg_sift VERSION '4.3'; CREATE TABLE t (a int, b int); CREATE INDEX t_idx ON t USING sift_col (a, b)" >/dev/null
out=$(db "ALTER EXTENSION pg_sift UPDATE" "-c pg_sift.install_notice=on")
check "with sift indexes in the database the same notice says what is left to do" grep -qF 'DETAIL:  SHOW sift.<setting> says unrecognized configuration parameter in a session until the library is loaded. 1 sift indexes and tables in this database: SELECT * FROM sift.upgrade_commands()' <<<"$out"
check "SHOW sift.<setting> fails in a fresh session, as the notice says" bash -c '! psql -X -At -h "$1" -p "$2" -U postgres -c "SHOW sift.enable_agg" >/dev/null 2>&1' _ "$(dir n)/sock" "$(port n)"

db "DROP TABLE t; DROP EXTENSION pg_sift" >/dev/null
db "$HINT" >/dev/null
out=$(db "CREATE EXTENSION pg_sift" "-c pg_sift.install_notice=on")
check "the command silences it for the next session" bash -c '! grep -q "not preloaded" <<<"$1"' _ "$out"
db "DROP EXTENSION pg_sift" >/dev/null
db "CREATE EXTENSION pg_sift VERSION '4.3'; CREATE TABLE t (a int, b int); CREATE INDEX t_idx ON t USING sift_col (a, b)" >/dev/null
out=$(db "ALTER EXTENSION pg_sift UPDATE" "-c pg_sift.install_notice=on")
check "preloaded, the update still says what is left to do" grep -qF 'NOTICE:  pg_sift is installed in database "postgres"' <<<"$out"
check "and nothing about the preload" bash -c '! grep -q "not preloaded" <<<"$1"' _ "$out"
out=$(db "SELECT sift._preload_hint() IS NULL")
check "and sift._preload_hint() is NULL" test "$out" = t

finish
