#!/usr/bin/env bash
# A backend that has used the objects of an older extension keeps working across ALTER EXTENSION UPDATE, which drops and makes functions, operators and event triggers
# under new OIDs: the pushdown, the index read, a search and the index DDL find them again in the same session (what the session remembered of the catalog is dropped
# with the objects; test ext_recreate does the same across DROP EXTENSION and CREATE EXTENSION). The library is the one of this tree over the SQL of 4.3.
. "$(dirname "$0")/lib.bash"

plan 6
node_init eu 45 'autovacuum = off'
node_start eu || bail "start failed"

# one round of statements: the answers of the round are lines "<tag>|<value>"
round() {
	cat <<EOF
SELECT '$1 agg|' || plan_has('SELECT g, count(*) FROM t GROUP BY g', 'SiftAgg');
SELECT '$1 read|' || plan_has('SELECT id FROM t WHERE g = 3', 'Index Only Scan using t_');
SELECT '$1 search|' || plan_has('SELECT id FROM d WHERE body @@> ''w3'' ORDER BY sift.score(ctid) DESC LIMIT 3', 'SiftScan');
SELECT '$1 source|' || NOT coalesce(bool_or(slug = 'no_source'), false) FROM sift.explain_why('SELECT id FROM t WHERE g = 3');
SELECT '$1 rows|' || (SELECT count(*) FROM d WHERE body @@> 'w3') || ',' || (SELECT sum(c) FROM (SELECT count(*) c FROM t GROUP BY g) x);
CREATE INDEX t_ddl ON t USING sift_col (id, g);
DROP INDEX t_ddl;
EOF
}

session=$({
	cat <<'EOF'
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE EXTENSION pg_sift VERSION '4.3';
CREATE FUNCTION plan_has(q text, node text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF position(node in l) > 0 THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE TABLE t (id int, g int);
INSERT INTO t SELECT i, i % 10 FROM generate_series(1, 20000) i;
CREATE INDEX t_col ON t USING sift_col (id, g);
CREATE TABLE d (id int PRIMARY KEY, body text);
INSERT INTO d SELECT i, 'w' || i % 10 FROM generate_series(1, 1000) i;
CREATE INDEX d_body ON d USING sift (body);
VACUUM ANALYZE t, d;
EOF
	round before
	echo "SELECT 'version|' || extversion FROM pg_extension WHERE extname = 'pg_sift';"
	echo "ALTER EXTENSION pg_sift UPDATE;"
	echo "SELECT 'version|' || extversion FROM pg_extension WHERE extname = 'pg_sift';"
	round after
} | sql eu)

tags="agg read search source rows"
for tag in $tags; do
	b=$(sed -n "s/^before $tag|//p" <<<"$session") a=$(sed -n "s/^after $tag|//p" <<<"$session")
	out=$session
	check "the $tag answer is the same after ALTER EXTENSION UPDATE in the session that used the old objects" test -n "$b" -a "$b" != false -a "$a" == "$b"
done
out=$session
check "the extension was updated" test "$(sed -n 's/^version|//p' <<<"$session" | tr '\n' ' ')" == "4.3 4.3.6 "

finish
