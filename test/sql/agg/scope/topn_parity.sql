-- The newest rows of every key of a scope come out of the ring as the join and the sort give them: every key, several limits, offsets, directions of the second key.
-- The same statement runs with the aggregate off (the heap decides) and on; the rows must be the same, row for row.
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
CREATE TABLE ev_s11 (id bigint PRIMARY KEY, entity_id int NOT NULL, event_ts timestamp NOT NULL, amount numeric(12,2) NOT NULL, qty int NOT NULL);
INSERT INTO ev_s11 SELECT g, 1 + (g % 60), TIMESTAMP '2025-01-01' + (g / 150) * interval '1 hour', (g % 977) / 10.0, g % 7 FROM generate_series(1, 20000) g;
CREATE TABLE pr_s11 (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id));
INSERT INTO pr_s11 SELECT e, 1 + (e % 7) FROM generate_series(1, 60) e;
CREATE INDEX ev_s11_scope ON ev_s11 USING sift_col (id, entity_id, event_ts, amount, qty) WITH (scope = 'parent_id: entity_id -> pr_s11(entity_id, parent_id)');
VACUUM (ANALYZE) ev_s11;
ANALYZE pr_s11;
CREATE TEMP TABLE topn_bad (key int, lim int, off int, dir text);
DO $$
DECLARE
	k int; l int; o int; d text; a text; b text; q text;
BEGIN
	FOR k IN SELECT DISTINCT parent_id FROM pr_s11 ORDER BY 1 LOOP
		FOREACH l IN ARRAY ARRAY[1, 5, 20, 64] LOOP
			FOREACH o IN ARRAY ARRAY[0, 3] LOOP
				IF l + o > 64 THEN CONTINUE; END IF;
				FOREACH d IN ARRAY ARRAY['DESC', 'ASC'] LOOP
					q := 'SELECT coalesce(string_agg(x::text, E''\n''), '''') FROM (SELECT e.id, e.event_ts, e.qty FROM ev_s11 e JOIN pr_s11 p ON p.entity_id = e.entity_id WHERE p.parent_id = '
						|| k || ' ORDER BY e.event_ts DESC, e.id ' || d || ' LIMIT ' || l || ' OFFSET ' || o || ') x';
					PERFORM set_config('sift.enable_agg', 'on', true);
					EXECUTE q INTO a;
					PERFORM set_config('sift.enable_agg', 'off', true);
					EXECUTE q INTO b;
					IF a IS DISTINCT FROM b THEN INSERT INTO topn_bad VALUES (k, l, o, d); END IF;
				END LOOP;
			END LOOP;
		END LOOP;
	END LOOP;
END $$;
SELECT count(*) AS mismatches FROM topn_bad;
DROP TABLE ev_s11, pr_s11;
DROP EXTENSION pg_sift;
