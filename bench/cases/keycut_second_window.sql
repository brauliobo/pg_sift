-- a window of dates over keys larger than a unit: the rows of a key sorted by the date and cut into units of 4,096 rows, so the two months of the window read the few units that hold them
-- label: `ks_facts` of 12 keys of 100,000 rows, dates scattered over 8 years in the order of the heap, `key_cut = 'dim_b_id, day'`; two one-month windows of the whole table, grouped by key
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('ks_facts') IS NULL THEN
		CREATE TABLE ks_facts AS SELECT g AS id, 1 + (g::bigint * 7919 % 12)::int AS dim_b_id, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int AS day, (g % 100)::float8 AS v
			FROM generate_series(1, 1200000) g;
		CREATE INDEX ks_facts_col ON ks_facts USING sift_col (dim_b_id, day, v) WITH (key_cut = 'dim_b_id, day');
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) ks_facts
--@ pg
SELECT dim_b_id, count(*) AS n, sum(v) AS total FROM ks_facts
WHERE day >= date '2023-03-01' AND day < date '2023-04-01' OR day >= date '2024-03-01' AND day < date '2024-04-01'
GROUP BY dim_b_id ORDER BY dim_b_id
