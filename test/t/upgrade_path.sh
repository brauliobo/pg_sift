#!/usr/bin/env bash
# tap: pooled
# Between a library and the SQL of its database: sift.upgrade_commands() (the RESET and the REINDEX of an index that holds an option 6.0 retired; an index that
# another session holds locked is listed as well, the call reads the catalogs and waits for nothing; a text index is never listed, with its mark or without it) and
# the new library over the SQL of 4.3 (the shared value store stays off, with no catalog error, until ALTER EXTENSION UPDATE).
. "$(dirname "$0")/lib.bash"

q() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock up)" -p "$(port up)" -U postgres -d "$db" "${@/#/--command=}"; }

plan 11
node_init up 31 "autovacuum = off"
node_start up || bail "start failed"
q "$(node_db up)" "CREATE DATABASE locked" "CREATE DATABASE marked" "CREATE DATABASE oldsql" >/dev/null

# two column indexes that wait for a REINDEX (key_columns after units of format 6); one of them is rebuilt in a session that stays open
q locked "CREATE EXTENSION pg_sift" "CREATE TABLE f (id int, g int)" "INSERT INTO f SELECT i, i % 9 FROM generate_series(1, 500) i" \
	"SET sift.test_col_format_minor = 6" "CREATE INDEX f_1 ON f USING sift_col (id, g)" "CREATE INDEX f_2 ON f USING sift_col (id, g)" \
	"ALTER INDEX f_1 SET (key_columns = 'g')" "ALTER INDEX f_2 SET (key_columns = 'g')" >/dev/null || bail "setup failed"
check "both indexes are listed, with a RESET and a REINDEX each, while nothing holds them" test "$(q locked "SELECT string_agg(object::text, ',' ORDER BY object::text, kind) FROM sift.upgrade_commands()")" == f_1,f_1,f_2,f_2
q locked "BEGIN" "REINDEX INDEX f_1" "SELECT pg_sleep(60)" >/dev/null 2>&1 &
for _ in $(seq 100); do
	[[ $(q locked "SELECT count(*) FROM pg_locks WHERE locktype = 'relation' AND mode = 'AccessExclusiveLock' AND granted AND relation = 'f_1'::regclass") != 0 ]] && break
	sleep 0.1
done
out=$(q locked "SELECT kind || ':' || object || ':' || coalesce(command, '') FROM sift.upgrade_commands() ORDER BY object::text" 2>&1)
q locked "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = 'locked' AND pid <> pg_backend_pid() AND query LIKE '%pg_sleep(60)%'" >/dev/null; wait
check "an index that is locked is listed, the call does not wait" test "$(grep -c '^reindex:f_1:REINDEX INDEX CONCURRENTLY public.f_1;$' <<<"$out")" == 1
check "the index that is not locked is still listed" test "$(grep -c '^reindex:f_2:REINDEX INDEX CONCURRENTLY public.f_2;$' <<<"$out")" == 1
check "the call does not fail" test "$(grep -c -i 'error' <<<"$out")" == 0

# a text index is read and written as before, with the mark of a build that holds no word cut at 255 bytes or without it: nothing to list
q marked "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE docs (id int PRIMARY KEY, body text)" \
	"INSERT INTO docs SELECT i, 'alpha' || i % 7 || ' beta' || i % 11 FROM generate_series(1, 500) i" "CREATE INDEX docs_idx ON docs USING sift (body)" >/dev/null || bail "setup failed"
check "a text index of this build is not listed" test "$(q marked "SELECT count(*) FROM sift.upgrade_commands() WHERE kind <> 'preload'")" == 0
check "its format says it holds long words as a prefix and a hash" test "$(q marked "SELECT long_words FROM sift.index_format('docs_idx')")" == t
q marked "SELECT sift._test_meta_note('docs_idx', 'old')" >/dev/null
check "an index without the mark is not listed" test "$(q marked "SELECT count(*) FROM sift.upgrade_commands() WHERE kind <> 'preload'")" == 0
q marked "REINDEX INDEX docs_idx" >/dev/null
check "its REINDEX leaves the list empty" test "$(q marked "SELECT count(*) FROM sift.upgrade_commands() WHERE kind <> 'preload'")" == 0

# the library over the SQL of 4.3: a string column is stored in the segments, with no catalog error
q oldsql "CREATE EXTENSION pg_sift VERSION '4.3'" "CREATE TABLE t (id int, a text, n int)" \
	"INSERT INTO t SELECT i, md5(i::text) || md5((i + 1)::text), i % 7 FROM generate_series(1, 3000) i" >/dev/null || bail "setup failed"
check "an index builds with the SQL of 4.3 and no value store" test "$(q oldsql "CREATE INDEX t_one ON t USING sift_col (id, a, n)" \
	"SELECT count(*) FROM pg_class WHERE relname LIKE '%_sift_values'" | tail -n 1)" == 0
q oldsql "ALTER EXTENSION pg_sift UPDATE" >/dev/null
check "after the update a REINDEX makes no store either" test "$(q oldsql "REINDEX INDEX t_one" \
	"SELECT count(*) FROM pg_class WHERE relname LIKE '%_sift_values'" | tail -n 1)" == 0
check "the index answers as the heap does" test "$(q oldsql "SELECT count(*) FROM t WHERE a = md5('7') || md5('8')")" == 1
finish
