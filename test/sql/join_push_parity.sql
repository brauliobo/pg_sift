-- A join of a fact table with a small relation on an integer column is answered by the scan of the fact table's index with the values of the small side as a key (SiftAggV6Push). Every shape is
-- run with the aggregate on and off and the rows must be the same: an inner join on a key the small side holds once, an inner join that the small side may repeat (no key: PostgreSQL's join stays),
-- the semi-join of an IN (sub-select), grouped and not, with a filter on the fact and a HAVING, and a small side with no row. The statements the node answered are counted.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
DROP TABLE IF EXISTS jp_ev, jp_pr CASCADE;
CREATE TABLE jp_ev (id bigint PRIMARY KEY, entity_id int NOT NULL, event_ts timestamp NOT NULL, kind text NOT NULL, amount numeric(12,2) NOT NULL, qty int NOT NULL);
INSERT INTO jp_ev SELECT g, 1 + (g % 300), TIMESTAMP '2025-01-01' + (g / 100) * interval '1 hour', 'k' || (g % 6), (g % 977) / 10.0, g % 7 FROM generate_series(1, 60000) g;
INSERT INTO jp_ev SELECT g, NULL, TIMESTAMP '2025-02-01', 'k0', 1, 1 FROM generate_series(60001, 60050) g WHERE false;
CREATE TABLE jp_pr (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id));
INSERT INTO jp_pr SELECT e, 1 + (e % 7) FROM generate_series(1, 300) e;
INSERT INTO jp_pr SELECT e, 1 + ((e + 3) % 7) FROM generate_series(1, 300) e WHERE e % 5 = 0;
CREATE INDEX jp_ix ON jp_ev USING sift_col (id, entity_id, event_ts, kind, amount, qty);
VACUUM (ANALYZE) jp_ev; ANALYZE jp_pr;

CREATE TEMP TABLE jp_shapes (name text, q text);
INSERT INTO jp_shapes VALUES
 ('count',        'SELECT count(*), sum(e.amount) FROM jp_ev e JOIN jp_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = %K%'),
 ('bykind',       'SELECT e.kind, count(*), sum(e.amount), sum(e.qty) FROM jp_ev e JOIN jp_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = %K% GROUP BY e.kind'),
 ('window',       'SELECT count(*), sum(e.amount) FROM jp_ev e JOIN jp_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = %K% AND e.event_ts >= ''2025-01-10'' AND e.event_ts < ''2025-01-20'''),
 ('having',       'SELECT e.kind, count(*) FROM jp_ev e JOIN jp_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = %K% GROUP BY e.kind HAVING count(*) > 1000'),
 ('repeating',    'SELECT count(*), sum(e.qty) FROM jp_ev e JOIN jp_pr p ON p.entity_id = e.entity_id WHERE p.parent_id IN (%K%, %K2%)'),
 ('semi',         'SELECT count(*), sum(e.amount) FROM jp_ev e WHERE e.entity_id IN (SELECT entity_id FROM jp_pr WHERE parent_id IN (%K%, %K2%, 5))'),
 ('semi grouped', 'SELECT e.kind, count(*) FROM jp_ev e WHERE e.entity_id IN (SELECT entity_id FROM jp_pr WHERE parent_id IN (%K%, %K2%)) GROUP BY e.kind'),
 ('anti',         'SELECT count(*) FROM jp_ev e WHERE e.entity_id NOT IN (SELECT entity_id FROM jp_pr WHERE parent_id = %K%)');

CREATE TEMP TABLE jp_result (shape text, tried int, pushed int, bad int);

CREATE FUNCTION pg_temp.check_shapes() RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	s record; k int; q text; a text; b text; plan text; tried int; pushed int; bad int;
BEGIN
	FOR s IN SELECT * FROM jp_shapes LOOP
		tried := 0; pushed := 0; bad := 0;
		FOREACH k IN ARRAY ARRAY[1, 2, 3, 4, 5, 6, 7, 99] LOOP
			q := replace(replace(s.q, '%K2%', ((k % 7) + 1)::text), '%K%', k::text);
			PERFORM set_config('sift.enable_agg', 'on', true);
			EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO a;
			FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
				IF plan LIKE '%SiftAggV6Push%' THEN pushed := pushed + 1; END IF;
			END LOOP;
			PERFORM set_config('sift.enable_agg', 'off', true);
			EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO b;
			tried := tried + 1;
			IF a IS DISTINCT FROM b THEN bad := bad + 1; END IF;
		END LOOP;
		INSERT INTO jp_result VALUES (s.name, tried, pushed, bad);
	END LOOP;
END $$;

SELECT pg_temp.check_shapes();
DELETE FROM jp_ev WHERE id % 5 = 0;
INSERT INTO jp_ev SELECT g, 1 + (g % 300), TIMESTAMP '2025-02-01' + (g % 50) * interval '1 hour', 'k' || (g % 6), 1, 1 FROM generate_series(70001, 70500) g;
DELETE FROM jp_pr WHERE entity_id % 11 = 0;
INSERT INTO jp_pr VALUES (5, 6) ON CONFLICT DO NOTHING;
SELECT pg_temp.check_shapes();
VACUUM jp_ev;
SELECT pg_temp.check_shapes();
SELECT shape, sum(tried) AS tried, sum(pushed) AS pushed, sum(bad) AS mismatches FROM jp_result GROUP BY shape ORDER BY shape;
DROP TABLE jp_ev, jp_pr CASCADE;
