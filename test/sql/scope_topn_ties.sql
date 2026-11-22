-- The newest rows of a key when many rows share a time (the cut falls inside a tie), when rows were deleted, inserted or updated since the summary (the rows it lacks are read from the slabs and the
-- parts it does not name; each phase is checked on a new connection) and when a key has fewer rows than the limit; each answer is compared with the heap's (aggregate off), and the statements the ring answered are counted.
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
DROP TABLE IF EXISTS tt_ev, tt_pr CASCADE;
CREATE TABLE tt_ev (id bigint PRIMARY KEY, entity_id int NOT NULL, event_ts timestamp NOT NULL, amount numeric(12,2) NOT NULL, qty int NOT NULL);
INSERT INTO tt_ev SELECT g, 1 + (g % 40), TIMESTAMP '2025-01-01' + (g / 150) * interval '1 hour', (g % 977) / 10.0, g % 7 FROM generate_series(1, 30000) g;
CREATE TABLE tt_pr (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id));
INSERT INTO tt_pr SELECT e, 1 + (e % 5) FROM generate_series(1, 40) e;
INSERT INTO tt_pr VALUES (41, 9);
INSERT INTO tt_ev VALUES (30001, 41, '2025-02-01', 1, 1), (30002, 41, '2025-02-01', 2, 2), (30003, 41, '2025-02-02', 3, 3);
CREATE INDEX tt_scope ON tt_ev USING sift_col (id, entity_id, event_ts, amount, qty) WITH (scope = 'parent_id: entity_id -> tt_pr(entity_id, parent_id)');
VACUUM (ANALYZE) tt_ev; ANALYZE tt_pr;

DROP TABLE IF EXISTS topn_result;
CREATE TABLE topn_result (phase text, tried int, ring int, bad int);

CREATE OR REPLACE FUNCTION check_phase(phase text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	k int; l int; o int; d text; a text; b text; q text; plan text; tried int := 0; ring int := 0; bad int := 0;
BEGIN
	FOREACH k IN ARRAY ARRAY[1, 2, 3, 4, 5, 9, 77] LOOP
		FOREACH l IN ARRAY ARRAY[1, 2, 20, 64] LOOP
			FOREACH o IN ARRAY ARRAY[0, 7] LOOP
				IF l + o > 64 THEN CONTINUE; END IF;
				FOREACH d IN ARRAY ARRAY['DESC', 'ASC'] LOOP
					q := 'SELECT e.id, e.event_ts, e.qty FROM tt_ev e JOIN tt_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = ' || k
						|| ' ORDER BY e.event_ts DESC, e.id ' || d || ' LIMIT ' || l || ' OFFSET ' || o;
					PERFORM set_config('sift.enable_agg', 'on', true);
					EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n''), '''') FROM (' || q || ') x' INTO a;
					FOR plan IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
						IF plan LIKE '%Scope Read: ring%' THEN ring := ring + 1; END IF;
					END LOOP;
					PERFORM set_config('sift.enable_agg', 'off', true);
					EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n''), '''') FROM (' || q || ') x' INTO b;
					tried := tried + 1;
					IF a IS DISTINCT FROM b THEN bad := bad + 1; END IF;
				END LOOP;
			END LOOP;
		END LOOP;
	END LOOP;
	INSERT INTO topn_result VALUES (phase, tried, ring, bad);
END $$;

\connect -
SET client_min_messages = warning;
SELECT check_phase('clean');
DELETE FROM tt_ev WHERE id % 3 = 0 AND id > 20000;
\connect -
SET client_min_messages = warning;
SELECT check_phase('after delete');
VACUUM tt_ev;
\connect -
SET client_min_messages = warning;
SELECT check_phase('vacuumed');
INSERT INTO tt_ev SELECT g, 1 + (g % 40), TIMESTAMP '2025-03-01' + (g % 3) * interval '1 hour', 1, 1 FROM generate_series(40001, 40400) g;
\connect -
SET client_min_messages = warning;
SELECT check_phase('inserted');
VACUUM tt_ev;
\connect -
SET client_min_messages = warning;
SELECT check_phase('vacuumed again');
UPDATE tt_ev SET event_ts = event_ts + interval '3 hours', qty = qty + 1 WHERE id % 7 = 0;
\connect -
SET client_min_messages = warning;
SELECT check_phase('updated');
VACUUM tt_ev;
\connect -
SET client_min_messages = warning;
SELECT check_phase('updated and vacuumed');
SELECT phase, tried, ring AS answered_by_ring, bad AS mismatches FROM topn_result ORDER BY 1;
DROP TABLE tt_ev, tt_pr, topn_result CASCADE;
DROP FUNCTION check_phase(text);
DROP EXTENSION pg_sift;
