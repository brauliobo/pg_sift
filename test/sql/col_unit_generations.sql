-- A unit generation owns its whole allocation. Only the exact owning unit is rewritten,
-- after VACUUM's callback confirms death; unrelated units retain their generations.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET sift.eval_methods = 'unit_rewrite';
CREATE TABLE events (id int PRIMARY KEY, tenant_id int, counter_a int, label text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, i % 10, i, repeat('x', 64) FROM generate_series(1,8000) i ORDER BY hashint4(i), i;
CREATE INDEX events_c ON events USING sift_col (id,tenant_id,counter_a,label)
 WITH (track_changes = 'id', key_cut = 'id', key_cut_rows = 1024, dead_rewrite_pct = 100, vacuum_truncate = false);
VACUUM (FREEZE, ANALYZE) events;
-- A different unit is already mostly dead, without any owned tombstones.
CREATE TEMP TABLE closed_roots AS SELECT gen FROM sift.col_segments('events_c') WHERE kind = 'immutable';
CREATE TEMP TABLE closed_tids AS SELECT ctid AS tid FROM events WHERE id >= 7300;
-- the DELETE trigger of the option is silenced for this statement: the unit must be dead by what VACUUM finds, with no tombstone of the log owned by it
SET sift.test_no_track = on;
DELETE FROM events WHERE id >= 7300;
RESET sift.test_no_track;
VACUUM (INDEX_CLEANUP ON) events;
SELECT bool_and(units = 1) AND count(*) > 2 AS independently_owned FROM sift.col_segments('events_c') WHERE kind = 'immutable';
INSERT INTO events SELECT i,i%10,i,repeat('y',64) FROM generate_series(10000,10700) i;
SELECT EXISTS(SELECT FROM closed_tids d JOIN events e ON e.ctid=d.tid WHERE e.id>=10000) AS closed_tids_reused;
UPDATE events SET counter_a=counter_a+1 WHERE id>=10000;
VACUUM (INDEX_CLEANUP ON) events;
SELECT count(*)=(SELECT count(*) FROM closed_roots) AS reused_tid_not_a_new_owner
 FROM closed_roots JOIN sift.col_segments('events_c') USING(gen);
UPDATE events SET counter_a=counter_a+1 WHERE id=7250;
VACUUM (INDEX_CLEANUP ON) events;
SELECT count(*)=(SELECT count(*) FROM closed_roots) AS sparse_owned_change_below_price
 FROM closed_roots JOIN sift.col_segments('events_c') USING(gen);
SELECT bool_and(stamp::text::bigint > 3) AS inherited_committed_stamp FROM sift.col_stamps('events_c');
CREATE TEMP TABLE before AS SELECT gen FROM sift.col_segments('events_c') WHERE kind = 'immutable';
UPDATE events SET counter_a = counter_a + 1 WHERE id = 7000;
VACUUM (INDEX_CLEANUP ON) events;
SELECT count(*) = (SELECT count(*) FROM before) AS below_unit_price FROM before JOIN sift.col_segments('events_c') USING (gen);
UPDATE events SET counter_a = counter_a + 1 WHERE id <= 400;
SELECT count(*) FILTER (WHERE tombs > 0) = 1 AND count(*) FILTER (WHERE pages_any > 0) > 1 AS exact_ownership
 FROM sift._test_col_unit_tombs('events_c');
SELECT count(*) = (SELECT count(*) FROM before) AS no_early_replacement FROM before JOIN sift.col_segments('events_c') USING (gen);
VACUUM (INDEX_CLEANUP ON) events;
SELECT count(*) = (SELECT count(*) - 1 FROM before) AS only_owned_unit_replaced FROM before JOIN sift.col_segments('events_c') USING (gen);
SELECT bool_and(units = 1) AS promotion_preserves_units FROM sift.col_segments('events_c') WHERE kind = 'immutable';
SELECT count(*) = 0 AS tombstones_retired FROM sift._test_col_log_entries('events_c');
SELECT sift.col_verify('events_c', true);
SET sift.enable_agg = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off; SET enable_bitmapscan = off;
CREATE TEMP TABLE oracle AS SELECT tenant_id,count(*) n,sum(counter_a) s,sum(id) k FROM events GROUP BY tenant_id;
RESET sift.enable_agg; RESET enable_indexscan; RESET enable_indexonlyscan; RESET enable_bitmapscan;
SET enable_seqscan = off;
SELECT NOT EXISTS ((SELECT * FROM oracle EXCEPT SELECT tenant_id,count(*),sum(counter_a),sum(id) FROM events GROUP BY tenant_id)
 UNION ALL (SELECT tenant_id,count(*),sum(counter_a),sum(id) FROM events GROUP BY tenant_id EXCEPT SELECT * FROM oracle)) AS same_heap;
RESET sift.eval_methods;
SELECT NOT EXISTS ((SELECT * FROM oracle EXCEPT SELECT tenant_id,count(*),sum(counter_a),sum(id) FROM events GROUP BY tenant_id)
 UNION ALL (SELECT tenant_id,count(*),sum(counter_a),sum(id) FROM events GROUP BY tenant_id EXCEPT SELECT * FROM oracle)) AS same_off;
BEGIN;
UPDATE events SET counter_a = -1 WHERE id <= 400;
ROLLBACK;
VACUUM (INDEX_CLEANUP ON) events;
SELECT NOT EXISTS ((SELECT * FROM oracle EXCEPT SELECT tenant_id,count(*),sum(counter_a),sum(id) FROM events GROUP BY tenant_id)
 UNION ALL (SELECT tenant_id,count(*),sum(counter_a),sum(id) FROM events GROUP BY tenant_id EXCEPT SELECT * FROM oracle)) AS rollback_preserved;
SET sift.eval_methods = 'unit_rewrite';
CREATE INDEX events_cap ON events USING sift_col(id) WITH (key_cut = 'id',key_cut_rows = 64);
SELECT sift.col_verify('events_c', true);
DROP TABLE events;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
