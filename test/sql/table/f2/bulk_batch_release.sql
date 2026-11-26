-- A COPY into a sift_table that takes the direct route makes the values of its rows that need a copy (detoasted datums, the elements of declared paths) in a batch
-- context the table keeps for the session: a call of fewer rows than the reset interval leaves nothing in it when it returns (the rows stayed in it until the next load).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE br (id int, body text, doc jsonb) USING sift_table;
SELECT sift.table_set_paths('br', 'doc', 'doc->a int4') IS NOT NULL AS paths_set;
COPY br FROM PROGRAM 'seq 1 1000 | awk ''{ printf "%d\tb%d\t{\"a\": %d, \"b\": \"x%d\"}\n", $1, $1, $1, $1 }''';
SELECT count(*) AS contexts, sum(total_bytes) < 64 * 1024 AS released FROM pg_backend_memory_contexts WHERE name = 'sift cache bulk insert batch';
SELECT count(*), sum((doc->>'a')::int) FROM br;
DROP TABLE br;
DROP EXTENSION pg_sift;
