-- the rows the scan reads again from the heap (a value the typed leaf cannot store) are priced from a sample of the flagged units
-- label: `avg((doc ->> 'score')::float8) GROUP BY grp`, one document in 33 has the score as a string (3% of 100,000 items)
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('items_d33') IS NULL THEN
		CREATE TABLE items_d33 (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false);
		INSERT INTO items_d33 SELECT i, i % 7, jsonb_build_object('score', CASE WHEN i % 33 = 0 THEN to_jsonb(((i % 9) + 0.5)::text) ELSE to_jsonb((i % 9) + 0.5) END) FROM generate_series(1, 100000) i;
		CREATE INDEX items_d33_col ON items_d33 USING sift_col (id, grp, doc) WITH (paths = 'doc.score float8');
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) items_d33
--@ pg
SELECT grp, count(*), avg((doc ->> 'score')::float8) FROM items_d33 GROUP BY grp ORDER BY grp
