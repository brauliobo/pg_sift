-- Deterministic generator of the archetype: entities, a stream of events with a Zipfian-skewed entity and a latest-version flag, and the log of jobs moving through states.
-- Usage: psql -X -v ON_ERROR_STOP=1 -v events=1000000 -v entities=20000 -v am=sift_table -v schema=arm_b -f generate.sql   (SF=1: 1M events and 20k entities, SF=10: ten times that; am: sift_table or heap; schema holds the tables of the arm)
SELECT setseed(0.53);
CREATE SCHEMA :"schema";
SET search_path = :"schema", public;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;

CREATE TABLE entities (id int PRIMARY KEY, region text NOT NULL, tier int NOT NULL, born date NOT NULL) USING :am;
INSERT INTO entities
SELECT g, (array['north', 'south', 'east', 'west', 'central'])[1 + g % 5], 1 + g % 4, date '2020-01-01' + (g * 7) % 1500
FROM generate_series(1, :entities) g;

CREATE TABLE events (
	id bigint PRIMARY KEY, entity_id int NOT NULL, kind text NOT NULL, at timestamptz NOT NULL, amount numeric(12,2), qty int NOT NULL,
	is_latest boolean NOT NULL, status text NOT NULL, body text) USING :am;
INSERT INTO events
SELECT g, 1 + (:entities * power(random(), 3))::int % :entities, (array['view', 'click', 'buy', 'refund', 'login', 'logout'])[1 + g % 6],
	timestamptz '2031-01-01' + g * interval '3 seconds', CASE WHEN g % 97 = 0 THEN NULL ELSE round((random() * 500)::numeric, 2) END, 1 + g % 9,
	g % 10 = 0, (array['new', 'open', 'done', 'failed'])[1 + g % 4], md5(g::text)
FROM generate_series(1, :events) g;

CREATE TABLE job_logs (job_id int NOT NULL, seq int NOT NULL, state text NOT NULL, at timestamptz NOT NULL, note text, PRIMARY KEY (job_id, seq)) USING :am;
INSERT INTO job_logs
SELECT j, s, (array['queued', 'running', 'retry', 'done'])[1 + s % 4], timestamptz '2031-01-01' + j * interval '1 minute' + s * interval '5 seconds', md5((j * 31 + s)::text)
FROM generate_series(1, :entities / 4) j, generate_series(0, 7) s;

ANALYZE entities;
ANALYZE events;
ANALYZE job_logs;
