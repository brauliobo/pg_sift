-- a GROUP BY of the last of eight dimensions of 50 values, on an index of sixteen columns: the cube keeps the dimension in every unit
-- label: `SELECT d8, count(*), sum(m1) FROM cube_wide GROUP BY d8`, 8 dimensions of 50 values and 8 measures, 500,000 rows
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('cube_wide') IS NULL THEN
		CREATE TABLE cube_wide AS SELECT g AS id,
			(hashint4(g + 1) & 2147483647) % 50 AS d1, (hashint4(g + 8) & 2147483647) % 50 AS d2, (hashint4(g + 15) & 2147483647) % 50 AS d3, (hashint4(g + 22) & 2147483647) % 50 AS d4,
			(hashint4(g + 29) & 2147483647) % 50 AS d5, (hashint4(g + 36) & 2147483647) % 50 AS d6, (hashint4(g + 43) & 2147483647) % 50 AS d7, (hashint4(g + 50) & 2147483647) % 50 AS d8,
			round((((hashint4(g * 7 + 3) & 2147483647) % 100000) / 100.0)::numeric, 2)::numeric(12, 2) AS m1, (hashint4(g * 11 + 6) & 2147483647) % 1000 AS m2,
			round((((hashint4(g * 7 + 5) & 2147483647) % 100000) / 100.0)::numeric, 2)::numeric(12, 2) AS m3, (hashint4(g * 11 + 8) & 2147483647) % 1000 AS m4,
			round((((hashint4(g * 7 + 7) & 2147483647) % 100000) / 100.0)::numeric, 2)::numeric(12, 2) AS m5, (hashint4(g * 11 + 10) & 2147483647) % 1000 AS m6,
			round((((hashint4(g * 7 + 9) & 2147483647) % 100000) / 100.0)::numeric, 2)::numeric(12, 2) AS m7
			FROM generate_series(1, 500000) g;
		CREATE INDEX cube_wide_col ON cube_wide USING sift_col (d1, d2, d3, d4, d5, d6, d7, d8, m1, m2, m3, m4, m5, m6, m7);
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) cube_wide
--@ pg
SELECT d8, count(*), sum(m1) FROM cube_wide GROUP BY d8 ORDER BY d8
