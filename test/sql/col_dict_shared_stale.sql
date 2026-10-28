-- The ring of the shared dictionaries (col/dictshm.c) is written outside the lock: a writer that is not scheduled between its copy and its publication, while the
-- ring wraps over its place and another writer fills it, publishes a key for a place that holds the record of another. A reader of that key must get no dictionary
-- (it parses the segment), not the dictionary of the other key even when the two records have the same size.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT coalesce(sift._test_col_dict_shared_stale(), 'none') AS dictionary_of_the_first_key;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
