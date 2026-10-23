-- the rows the scan reads again from the heap (a value the typed leaf cannot store) are priced from a sample of the flagged units
-- label: `avg((doc ->> 'score')::float8) GROUP BY grp`, one document in 2 has the score as a string (50% of 100,000 items)
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('items_d2') IS NULL THEN
		CREATE TABLE items_d2 (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false);
		INSERT INTO items_d2 SELECT i, i % 7, jsonb_build_object('score', CASE WHEN i % 2 = 0 THEN to_jsonb(((i % 9) + 0.5)::text) ELSE to_jsonb((i % 9) + 0.5) END) FROM generate_series(1, 100000) i;
		CREATE INDEX items_d2_col ON items_d2 USING sift_col (id, grp, doc) WITH (paths = 'doc.score float8');
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) items_d2
--@ pg
SELECT grp, count(*), avg((doc ->> 'score')::float8) FROM items_d2 GROUP BY grp ORDER BY grp
