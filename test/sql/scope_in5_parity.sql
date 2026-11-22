-- IN (SELECT member FROM pairs WHERE key IN (...)) is answered from the scope summary: every subset of up to five keys, over a table whose members belong to one key
-- over a table whose members are in one key but for a few that are in two (the layout clustered by key, which takes the copies of those members out of the sum), and over a table whose members belong
-- to several keys (the join answers), clean and after edits.
-- Each statement runs with the aggregate on and off; the rows must be the same.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
DROP TABLE IF EXISTS in_ev, in_ev2, in_ev3, in_pr, in_pm, in_pp CASCADE;
CREATE TABLE in_ev (id bigint PRIMARY KEY, entity_id int NOT NULL, event_ts timestamp NOT NULL, amount numeric(12,2) NOT NULL, qty int NOT NULL);
INSERT INTO in_ev SELECT g, 1 + (g % 60), TIMESTAMP '2025-01-01' + (g / 100) * interval '1 hour', (g % 977) / 10.0, g % 7 FROM generate_series(1, 40000) g;
CREATE TABLE in_pr (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id));
INSERT INTO in_pr SELECT e, 1 + (e % 7) FROM generate_series(1, 60) e;
CREATE TABLE in_pm (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id));
INSERT INTO in_pm SELECT e, 1 + (e % 7) FROM generate_series(1, 60) e;
INSERT INTO in_pm SELECT e, 1 + ((e + 2) % 7) FROM generate_series(1, 60) e WHERE e % 3 = 0;
CREATE INDEX in_scope ON in_ev USING sift_col (id, entity_id, event_ts, amount, qty) WITH (scope = 'parent_id: entity_id -> in_pr(entity_id, parent_id)');
CREATE TABLE in_ev2 (LIKE in_ev INCLUDING ALL);
INSERT INTO in_ev2 SELECT * FROM in_ev;
CREATE INDEX in_scope2 ON in_ev2 USING sift_col (id, entity_id, event_ts, amount, qty) WITH (scope = 'parent_id: entity_id -> in_pm(entity_id, parent_id)');
CREATE TABLE in_pp (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id));
INSERT INTO in_pp SELECT e, 1 + (e % 7) FROM generate_series(1, 300) e;
INSERT INTO in_pp SELECT e, 1 + ((e + 3) % 7) FROM generate_series(1, 300) e WHERE e % 33 = 0;
CREATE TABLE in_ev3 (LIKE in_ev INCLUDING ALL);
INSERT INTO in_ev3 SELECT g, 1 + (g % 300), TIMESTAMP '2025-01-01' + (g / 100) * interval '1 hour', (g % 977) / 10.0, g % 7 FROM generate_series(1, 60000) g;
CREATE INDEX in_scope3 ON in_ev3 USING sift_col (id, entity_id, event_ts, amount, qty) WITH (scope = 'parent_id: entity_id -> in_pp(entity_id, parent_id)');
VACUUM (ANALYZE) in_ev; VACUUM (ANALYZE) in_ev2; VACUUM (ANALYZE) in_ev3; ANALYZE in_pr; ANALYZE in_pm; ANALYZE in_pp;

CREATE TEMP TABLE in_result (phase text, pairs text, tried int, served int, bad int);

CREATE FUNCTION pg_temp.check_phase(phase text, ev text, pr text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	mask int; keys text; q text; a text; b text; plan text; tried int := 0; served int := 0; bad int := 0;
BEGIN
	FOR mask IN 1..127 LOOP
		SELECT string_agg(k::text, ',') INTO keys FROM generate_series(1, 7) k WHERE (mask >> (k - 1)) & 1 = 1;
		IF length(keys) > 9 THEN CONTINUE; END IF;
		q := 'SELECT count(*), sum(e.amount), sum(e.qty) FROM ' || ev || ' e WHERE e.entity_id IN (SELECT entity_id FROM ' || pr || ' WHERE parent_id IN (' || keys || '))';
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE q INTO a;
		FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
			IF plan LIKE '%SiftAggV6Scope%' THEN served := served + 1; END IF;
		END LOOP;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE q INTO b;
		tried := tried + 1;
		IF a IS DISTINCT FROM b THEN bad := bad + 1; END IF;
	END LOOP;
	INSERT INTO in_result VALUES (phase, pr, tried, served, bad);
END $$;

SELECT pg_temp.check_phase('clean', 'in_ev', 'in_pr');
SELECT pg_temp.check_phase('clean', 'in_ev2', 'in_pm');
SELECT pg_temp.check_phase('clean', 'in_ev3', 'in_pp');
DELETE FROM in_ev WHERE id % 5 = 0; DELETE FROM in_ev2 WHERE id % 5 = 0; DELETE FROM in_ev3 WHERE id % 5 = 0;
INSERT INTO in_ev SELECT g, 1 + (g % 60), TIMESTAMP '2025-02-01' + (g % 50) * interval '1 hour', 1, 1 FROM generate_series(50001, 50500) g;
INSERT INTO in_ev2 SELECT * FROM in_ev WHERE id > 50000;
INSERT INTO in_ev3 SELECT g, 1 + (g % 300), TIMESTAMP '2025-02-01' + (g % 50) * interval '1 hour', 1, 1 FROM generate_series(70001, 70500) g;
SELECT pg_temp.check_phase('edited', 'in_ev', 'in_pr');
SELECT pg_temp.check_phase('edited', 'in_ev2', 'in_pm');
SELECT pg_temp.check_phase('edited', 'in_ev3', 'in_pp');
VACUUM in_ev; VACUUM in_ev2; VACUUM in_ev3;
SELECT pg_temp.check_phase('vacuumed', 'in_ev', 'in_pr');
SELECT pg_temp.check_phase('vacuumed', 'in_ev2', 'in_pm');
SELECT pg_temp.check_phase('vacuumed', 'in_ev3', 'in_pp');
SELECT phase, pairs, tried, (served > 0) AS served, bad AS mismatches FROM in_result ORDER BY 1, 2;
DROP TABLE in_ev, in_ev2, in_ev3, in_pr, in_pm, in_pp CASCADE;
