-- The queries whose results are stored in expected.txt (scripts/make-compat-fixture.sh runs them on the 2.x
-- build, test/t/compat_2x_fixture.sh on the build under test): every one goes through the sift index.
SET enable_seqscan = off;
SET sift.enable_scan = on;
SET sift.enable_count = on;

\echo segments (gen kind ndocs dirty dead_docs)
SELECT gen, kind, ndocs, dirty, dead_docs FROM sift.segments('docs_idx') ORDER BY gen;

\echo rows
SELECT count(*), sum(id) FROM docs;

\echo count and sum(id) per query
SELECT 'alpha3', count(*), sum(id) FROM docs WHERE body @@> 'alpha3';
SELECT 'beta5 gamma', count(*), sum(id) FROM docs WHERE body @@> 'beta5 gamma';
SELECT 'alpha3 NOT beta5', count(*), sum(id) FROM docs WHERE body @@> 'alpha3 NOT beta5';
SELECT 'gamma OR rareterm', count(*), sum(id) FROM docs WHERE body @@> 'gamma OR rareterm';
SELECT 'w1*', count(*), sum(id) FROM docs WHERE body @@> 'w1*';
SELECT 'phrase', count(*), sum(id) FROM docs WHERE body @@> '"alpha3 beta5"';
SELECT 'filler', count(*), sum(id) FROM docs WHERE body @@> 'filler';
SELECT 'rareterm', count(*), sum(id) FROM docs WHERE body @@> 'rareterm';
SELECT 'dead rows of the dirty segment (id % 10 = 3, id <= 1500)', count(*) FROM docs WHERE id <= 1500 AND id % 10 = 3;
SELECT 'live rows of the dirty segment', count(*) FROM docs WHERE body @@> 'alpha0 OR alpha1 OR alpha2 OR alpha3 OR alpha4 OR alpha5 OR alpha6' AND id <= 1500;
SELECT 'write buffer rows (id > 2900)', count(*), sum(id) FROM docs WHERE body @@> 'w7' AND id > 2900;

\echo ranked (id score), ties broken by id
SELECT id, round(sift.score(ctid)::numeric, 3) FROM docs WHERE body @@> 'gamma OR rareterm OR alpha3'
	ORDER BY sift.score(ctid) DESC, id LIMIT 12;
SELECT id, round(sift.score(ctid)::numeric, 3) FROM docs WHERE body @@> 'beta5 filler'
	ORDER BY sift.score(ctid) DESC, id LIMIT 12;

\echo verify
SELECT sift.verify('docs_idx');
SELECT sift.verify('docs_idx', true);
\echo verified
