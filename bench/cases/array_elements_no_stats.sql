-- the elements of a text[] column are kept when the table is indexed before its first ANALYZE or VACUUM (rows sized from the pages), so an element test reads a directory of rows, not every array
-- label: `labels @> ARRAY['label_000']` over 500,000 rows of 1 to 4 labels, the index built on the table as loaded (reltuples -1), `count` and `avg` by `kind`
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('an_events') IS NULL THEN
		CREATE TABLE an_events WITH (autovacuum_enabled = false) AS
		SELECT i AS id, (ARRAY['a','b','c','d','e','f','g','h'])[1 + i % 8] AS kind, (1 + i % 5)::int AS weight,
		       (SELECT array_agg(DISTINCT 'label_' || lpad((((hashint4(i * 8 + j) & 2147483647) % 40) * ((hashint4(i * 8 + j + 3) & 2147483647) % 5) % 200)::text, 3, '0'))
		          FROM generate_series(1, 1 + (hashint4(i + 19) & 2147483647) % 4) j) AS labels
		FROM generate_series(1, 500000) i;
		CREATE INDEX an_events_col ON an_events USING sift_col (id, kind, weight, labels);
	END IF;
END $do$;
VACUUM (FREEZE) an_events
--@ pg
SELECT kind, count(*) AS n, avg(weight) AS r FROM an_events WHERE labels @> ARRAY['label_000'] GROUP BY kind ORDER BY kind
