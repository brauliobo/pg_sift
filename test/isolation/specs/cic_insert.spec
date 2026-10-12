# CREATE INDEX CONCURRENTLY with inserts and updates before, during and after
# its build and validation phases.
# requires: build verify

setup
{
	CREATE TABLE iso_cic (id int, body text);
	INSERT INTO iso_cic SELECT g, 'cic doc ' || g FROM generate_series(1, 1000) g;
}

teardown
{
	DROP TABLE iso_cic;
}

session s1
step s1b	{ BEGIN; }
step s1i	{ INSERT INTO iso_cic SELECT g, 'cic doc late ' || g FROM generate_series(2001, 2500) g; }
step s1u	{ UPDATE iso_cic SET body = 'cic updated ' || id WHERE id % 10 = 0; }
step s1c	{ COMMIT; }

session s2
step s2cic	{ CREATE INDEX CONCURRENTLY iso_cic_idx ON iso_cic USING sift (body); }

session s3
step s3i	{ INSERT INTO iso_cic SELECT g, 'cic doc during ' || g FROM generate_series(3001, 3500) g; }
step s3check	{ SELECT i.indisvalid AS valid, iso_agree('iso_cic', 'cic') AS agree, iso_agree('iso_cic', 'late OR during OR updated') AS agree_new, iso_verify('iso_cic_idx') AS verified FROM pg_index i WHERE i.indexrelid = 'iso_cic_idx'::regclass; }

# CIC waits for the open writer while another session inserts
permutation s1b s1i s2cic s3i s1u s1c s3check
permutation s1b s1u s2cic s1c s3i s3check
