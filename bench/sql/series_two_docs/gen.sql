-- Two jsonb documents per event (metrics: key -> number, payload: an array of parts), a mapping table and a day table: the generator of series_two_docs/query.sql.
-- psql -v n=100000 -f gen.sql (measured at 25,000 to 200,000 rows)
CREATE EXTENSION IF NOT EXISTS pg_sift;
DROP TABLE IF EXISTS events, dims, maps CASCADE;
CREATE TABLE dims (id int PRIMARY KEY, kind text NOT NULL);
INSERT INTO dims SELECT g, 'cat_' || chr(97 + g % 4) FROM generate_series(1, 50) g;
CREATE TABLE maps (src text, cat text, sub text);
INSERT INTO maps VALUES ('overall', 'cat_a', NULL), ('asp_a', 'cat_b', 'asp_a'), ('asp_b_x', 'cat_c', 'asp_b'), ('asp_b_y', 'cat_c', 'asp_b');
CREATE TABLE events (
  id int PRIMARY KEY,
  parent_id int NOT NULL,
  ts date NOT NULL,
  deleted_at timestamp,
  metrics jsonb NOT NULL,
  payload jsonb
);
INSERT INTO events
SELECT g, 1 + g % 50, current_date - (g * 7919 % 700), NULL,
       CASE WHEN g % 20 = 0 THEN '{}'::jsonb
            ELSE jsonb_build_object('overall', (g % 9) + 1, 'asp_a', (g % 10) + 1, 'asp_b_x', (g * 3 % 10) + 1, 'asp_b_y', (g * 7 % 10) + 1) END,
       CASE WHEN g % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || chr(97 + (g + j) % 4), 'sub', CASE WHEN j % 2 = 0 THEN 'asp_' || chr(97 + j % 3) END,
                                                                                    'sign', CASE (g + j) % 3 WHEN 0 THEN 'pos' WHEN 1 THEN 'neg' ELSE 'neu' END, 'score', (g * j) % 10))
                                              FROM generate_series(1, 1 + g % 3) j)) END
FROM generate_series(1, :n) g;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, ts, deleted_at, metrics, payload)
  WITH (paths = 'metrics.* float8, payload.parts[]{cat text, sub text, sign text, score float8}');
VACUUM (ANALYZE) events, dims, maps;
