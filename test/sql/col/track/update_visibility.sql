-- The records an UPDATE leaves for the index (the address change of a row, the end of its old version) carry the command id of the statement. A trigger that advances the command counter
-- while the statement's triggers fire (a plpgsql trigger function that evaluates an expression does) moves it past the id of the statement, and the statement then ends without taking
-- another: the next statement of the transaction ran at the id the records carry and did not see them, so a Top-N read the replaced versions of the rows. A BEFORE UPDATE trigger may also
-- change an indexed column the SET list does not name, which PostgreSQL does not tell the index (indexUnchanged): the new version was read as the old one.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION answer(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('sift.agg_force', scan::text, true);
	EXECUTE 'SELECT coalesce(string_agg(x::text, '';'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION scan_planned(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	PERFORM set_config('sift.agg_force', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- the trigger function advances the command counter in the middle of the statement's triggers
CREATE TABLE ta (k text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL, pad int NOT NULL DEFAULT 0) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO ta (k, tenant, amount) SELECT 'c' || i, i % 10, i FROM generate_series(1, 12000) i;
CREATE INDEX ta_c ON ta USING sift_col (k, tenant, amount) WITH (track_changes = 'ctid');
ANALYZE ta;
CREATE FUNCTION ta_trg() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	IF pg_trigger_depth() = 1 AND NEW.amount % 7 = 0 THEN UPDATE ta SET pad = pad + 1 WHERE k = NEW.k; END IF;
	RETURN NULL;
END $$;
CREATE TRIGGER ta_t AFTER UPDATE ON ta FOR EACH ROW EXECUTE FUNCTION ta_trg();

BEGIN ISOLATION LEVEL REPEATABLE READ;
UPDATE ta SET k = k || '_1' WHERE amount % 7 = 4;
SELECT scan_planned('SELECT k, amount FROM ta ORDER BY amount DESC, k LIMIT 15') AS scan;
SELECT answer(q, true) = answer(q, false) AS same
FROM unnest(ARRAY['SELECT k, amount FROM ta ORDER BY amount DESC, k LIMIT 15',
				  'SELECT k, amount FROM ta ORDER BY amount, k LIMIT 15',
				  'SELECT tenant, count(*), sum(amount) FROM ta GROUP BY tenant']) q;
SELECT answer('SELECT k, amount FROM ta ORDER BY amount DESC, k LIMIT 8', true);
ROLLBACK;

-- a BEFORE UPDATE trigger changes an indexed column the statement does not set
CREATE TABLE tb (k int PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL, pad int NOT NULL DEFAULT 0) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO tb (k, tenant, amount) SELECT i, i % 10, i FROM generate_series(1, 3000) i;
CREATE INDEX tb_c ON tb USING sift_col (k, tenant, amount) WITH (track_changes = 'ctid');
ANALYZE tb;
CREATE FUNCTION tb_trg() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.amount % 11 = 0 THEN NEW.amount := NEW.amount + 1; END IF;
	RETURN NEW;
END $$;
CREATE TRIGGER tb_t BEFORE UPDATE ON tb FOR EACH ROW EXECUTE FUNCTION tb_trg();

UPDATE tb SET pad = pad + 1 WHERE k % 31 = 2;
SELECT scan_planned('SELECT tenant, count(*), sum(amount), min(amount), max(amount) FROM tb GROUP BY tenant') AS scan;
SELECT answer(q, true) = answer(q, false) AS same
FROM unnest(ARRAY['SELECT tenant, count(*), sum(amount), min(amount), max(amount) FROM tb GROUP BY tenant',
				  'SELECT k, amount FROM tb ORDER BY amount DESC, k LIMIT 15',
				  'SELECT count(*), sum(amount) FROM tb']) q;

DROP TABLE ta, tb;
DROP FUNCTION answer(text, bool), scan_planned(text), ta_trg(), tb_trg();
DROP EXTENSION pg_sift;
