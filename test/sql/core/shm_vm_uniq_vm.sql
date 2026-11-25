-- The vblock counter of the metapage of a sift_table never reaches the invalid block number (a row address that wraps collides with a live row, and a pass of VACUUM that wraps sets the bits of
-- the first page of the map), a record of the visibility map names a transaction of the table only while a snapshot can still be older than it, and redo refuses a record that is shorter than it says.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;
CREATE FUNCTION pg_temp.set_next_vblock(regclass, int8) RETURNS void AS 'pg_sift', 'sift_test_tbl_set_next_vblock' LANGUAGE C STRICT;
CREATE FUNCTION pg_temp.horizon_xid(xid8, xid8) RETURNS int8 AS 'pg_sift', 'sift_test_vm_horizon_xid' LANGUAGE C STRICT;
CREATE FUNCTION pg_temp.redo_set(bytea) RETURNS void AS 'pg_sift', 'sift_test_vm_redo_set' LANGUAGE C STRICT;
CREATE FUNCTION pg_temp.state_of(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ BEGIN EXECUTE stmt; RETURN 'ok'; EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE; END $$;

-- two vblocks are left before the invalid block number: the rows that fit are written, the row that does not is refused, and no row has the address of one that is live
CREATE TABLE vb (k int, v int) USING sift_table;
INSERT INTO vb SELECT g, g FROM generate_series(1, 100) g;
SELECT pg_temp.set_next_vblock('vb', 4294967293);
INSERT INTO vb SELECT g, g FROM generate_series(101, 200) g;
SELECT count(*) AS rows, count(DISTINCT ctid) AS addresses, max((ctid::text::point)[0]) < 4294967295 AS below_invalid FROM vb;
SELECT pg_temp.state_of('INSERT INTO vb SELECT g, g FROM generate_series(1, 5000) g') AS past_the_end;
SELECT count(*) AS rows, count(DISTINCT ctid) AS addresses FROM vb;
VACUUM vb;
SELECT count(*) AS rows_after_vacuum FROM vb;

-- a transaction names itself to a standby only within 2^31 transactions of the next one: an older one is older than every snapshot
SELECT pg_temp.horizon_xid('0', '1000') AS none,
       pg_temp.horizon_xid('5000', '6000') AS young,
       pg_temp.horizon_xid('4294967303', '4294968303') AS young_after_the_first_epoch,
       pg_temp.horizon_xid('4294967303', '6442450956') AS wrapped,
       pg_temp.horizon_xid('2000000000', '4300000000') AS older_than_half;

-- redo of a record that holds fewer bytes than its header, or fewer than its words need, is data corruption
SELECT pg_temp.state_of($$SELECT pg_temp.redo_set('\x00000000')$$) AS shorter_than_the_header;
SELECT pg_temp.state_of($$SELECT pg_temp.redo_set('\x000000000000000000000000000000000000000003000100')$$) AS words_missing;

DROP TABLE vb;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
