-- The slot of a record in a write buffer page of a sift_table comes from disk: one that, or whose length, reaches past the page is
-- corruption (the readers dereference it), not a pointer out of the page.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT sift._test_tbl_page_rec(64, 16) AS inside;
SELECT sift._test_tbl_page_rec(0, 0) AS empty_slot;
SELECT sift._test_tbl_page_rec(65535, 0) AS slot_past_page;
SELECT sift._test_tbl_page_rec(100, 60000) AS length_past_page;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
