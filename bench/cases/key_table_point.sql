-- the rows of three keys of a foreign key column (6,313 values): only the parts whose key range holds them are read
-- label: `count(*) WHERE segment_id IN (1234, 2345, 3456)` (6,313 values, `key_order = 'segment_id'`)
-- gap: sift WS-113-7 (S113-17: no key-order part pruning in V6)
--@ pg
SELECT count(*) FROM orders WHERE segment_id IN (1234, 2345, 3456)
