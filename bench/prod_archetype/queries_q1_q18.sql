-- The eighteen queries of the archetype, each in its own block "-- Q-A<n>: <shape>" ended by a line with ";;". run.py replaces @E, @N and @J with the tables of an arm and compares the md5 of every
-- result across the arms: the oracle is the arm on heap tables.
-- Q-A1: count of every kind
SELECT kind, count(*) FROM @E GROUP BY kind ORDER BY kind
;;
-- Q-A2: sum and average of an exact number by status
SELECT status, sum(amount), avg(amount), count(amount) FROM @E GROUP BY status ORDER BY status
;;
-- Q-A3: distinct entities per kind
SELECT kind, count(DISTINCT entity_id) FROM @E GROUP BY kind ORDER BY kind
;;
-- Q-A4: a time window with a filter
SELECT kind, count(*), sum(qty) FROM @E WHERE at >= timestamptz '2031-01-03' AND at < timestamptz '2031-01-04' AND status <> 'failed' GROUP BY kind ORDER BY kind
;;
-- Q-A5: bins of an hour
SELECT date_bin(interval '1 hour', at, timestamptz '2031-01-01') AS bin, count(*), sum(amount) FROM @E GROUP BY bin ORDER BY bin LIMIT 48
;;
-- Q-A6: FILTER clauses
SELECT kind, count(*) FILTER (WHERE is_latest), sum(qty) FILTER (WHERE status = 'done'), max(amount) FILTER (WHERE qty > 5) FROM @E GROUP BY kind ORDER BY kind
;;
-- Q-A7: an IN list and a null test
SELECT status, count(*), sum(amount) FROM @E WHERE kind IN ('buy', 'refund') AND amount IS NOT NULL GROUP BY status ORDER BY status
;;
-- Q-A8: the latest version of each entity
SELECT entity_id, count(*) FROM @E WHERE is_latest GROUP BY entity_id ORDER BY count(*) DESC, entity_id LIMIT 50
;;
-- Q-A9: top entities by an exact sum
SELECT entity_id, sum(amount) FROM @E WHERE amount IS NOT NULL GROUP BY entity_id ORDER BY sum(amount) DESC, entity_id LIMIT 20
;;
-- Q-A10: a join with a dimension, grouped by its column
SELECT n.region, count(*), sum(e.amount) FROM @E e JOIN @N n ON n.id = e.entity_id GROUP BY n.region ORDER BY n.region
;;
-- Q-A11: a join with a filter on the dimension
SELECT n.tier, e.kind, count(*) FROM @E e JOIN @N n ON n.id = e.entity_id WHERE n.region = 'north' GROUP BY n.tier, e.kind ORDER BY n.tier, e.kind
;;
-- Q-A12: the rows of a key range
SELECT id, entity_id, at FROM @E WHERE id BETWEEN 100000 AND 100020 ORDER BY id
;;
-- Q-A13: the newest events
SELECT id, at, kind FROM @E ORDER BY at DESC, id DESC LIMIT 25
;;
-- Q-A14: min and max of a day
SELECT date_trunc('day', at) AS day, min(amount), max(amount), count(*) FROM @E GROUP BY day ORDER BY day LIMIT 30
;;
-- Q-A15: two keys
SELECT kind, status, count(*), sum(qty) FROM @E GROUP BY kind, status ORDER BY kind, status
;;
-- Q-A16: the last state of each job
SELECT state, count(*) FROM (SELECT DISTINCT ON (job_id) job_id, state FROM @J ORDER BY job_id, seq DESC) s GROUP BY state ORDER BY state
;;
-- Q-A17: the states of the jobs in a window
SELECT state, count(*) FROM @J WHERE at >= timestamptz '2031-01-01 02:00' AND at < timestamptz '2031-01-01 06:00' GROUP BY state ORDER BY state
;;
-- Q-A18: for each of the first jobs, the latest event at or before its first state
SELECT j.job_id, (SELECT max(e.at) FROM @E e WHERE e.entity_id = j.job_id AND e.at <= j.at) AS last_event
FROM @J j WHERE j.seq = 0 AND j.job_id <= 200 ORDER BY j.job_id
;;
