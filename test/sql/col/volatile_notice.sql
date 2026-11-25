-- A column the index covers cannot be changed by a HOT update. CREATE INDEX names the columns that look rewritten often (a timestamp of the last change, a counter,
-- a version) once; an index that leaves them out, or covers only other columns, says nothing.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = notice;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE vo (id int, set_id int, updated_at timestamptz, a_count int, comment_count int, lock_version int, amt int, "Updated_On" date);
INSERT INTO vo SELECT i, i % 10, now(), i % 5, i % 3, 0, i, current_date FROM generate_series(1, 1000) i;

-- one, several (in the order of the index), with the case of the name ignored
CREATE INDEX vo_one ON vo USING sift_col (set_id, updated_at, amt);
CREATE INDEX vo_many ON vo USING sift_col (a_count, set_id, comment_count, lock_version, "Updated_On");
-- covered columns that are not volatile: silent
CREATE INDEX vo_none ON vo USING sift_col (id, set_id, amt);
REINDEX INDEX vo_one;

SET client_min_messages = warning;
DROP TABLE vo;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
