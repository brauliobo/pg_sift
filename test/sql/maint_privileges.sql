-- promote, merge, verify, segments, segment_postings and index_info are for the table's owner or a role with MAINTAIN
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
DROP ROLE IF EXISTS sift_t_owner, sift_t_reader, sift_t_maint;
CREATE ROLE sift_t_owner;
CREATE ROLE sift_t_reader;
CREATE ROLE sift_t_maint;
GRANT USAGE ON SCHEMA sift, public TO sift_t_owner, sift_t_reader, sift_t_maint;

CREATE TABLE pr (id int, body text);
INSERT INTO pr SELECT i, 'alpha beta ' || i FROM generate_series(1, 300) i;
CREATE INDEX pr_idx ON pr USING sift (body);
ALTER TABLE pr OWNER TO sift_t_owner;
GRANT SELECT ON pr TO sift_t_reader, sift_t_maint;
GRANT MAINTAIN ON pr TO sift_t_maint;

-- a role that can only read the table (and use the schema) is refused every one of them
SET ROLE sift_t_reader;
SELECT count(*) FROM pr WHERE body @@> 'alpha';
SELECT sift.promote('pr_idx');
SELECT sift.merge('pr_idx');
SELECT sift.merge('pr_idx', true);
SELECT count(*) FROM sift.segments('pr_idx');
SELECT count(*) FROM sift.segment_postings('pr_idx');
SELECT sift.verify('pr_idx');
SELECT sift.verify('pr_idx', true);
SELECT * FROM sift.index_info('pr_idx');
RESET ROLE;

-- the owner and a role with MAINTAIN may
SET ROLE sift_t_owner;
SELECT sift.promote('pr_idx') >= 0 AS promoted;
SELECT sift.merge('pr_idx', true) >= 0 AS merged;
SELECT count(*) > 0 AS segments FROM sift.segments('pr_idx');
SELECT count(*) > 0 AS postings FROM sift.segment_postings('pr_idx');
SELECT sift.verify('pr_idx', true);
RESET ROLE;
SET ROLE sift_t_maint;
SELECT sift.promote('pr_idx') >= 0 AS promoted;
SELECT sift.merge('pr_idx') >= 0 AS merged;
SELECT sift.verify('pr_idx');
SELECT segments > 0 AS has_segments FROM sift.index_info('pr_idx');
RESET ROLE;

-- losing MAINTAIN loses the access
REVOKE MAINTAIN ON pr FROM sift_t_maint;
SET ROLE sift_t_maint;
SELECT sift.promote('pr_idx');
RESET ROLE;

DROP TABLE pr;
DROP OWNED BY sift_t_owner, sift_t_reader, sift_t_maint;
DROP ROLE sift_t_owner, sift_t_reader, sift_t_maint;
DROP EXTENSION pg_sift;
