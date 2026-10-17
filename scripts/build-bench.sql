-- Articles of scripts/build-bench.sh: :rows documents of 300 words drawn from a skewed vocabulary of 200000
-- (the same documents for the same :rows, whatever the build).
CREATE EXTENSION IF NOT EXISTS pg_sift;
DROP TABLE IF EXISTS art;
SELECT setseed(0.42);
CREATE TABLE art (id serial, body text) WITH (parallel_workers = 2);
INSERT INTO art (body)
SELECT (SELECT string_agg('w' || floor(power(random(), 3) * 200000)::int, ' ') FROM generate_series(1, 300))
FROM generate_series(1, :rows) i;
VACUUM ANALYZE art;
