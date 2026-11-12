-- The words of a directory entry that say which transactions a segment waits for (col/xstamp.h): next:base:open, with next the xid after the latest inserter, base the latest of the
-- transactions that had not finished and bit i of open the transaction i steps before it. The encoding and the union that a merge makes of two of them, on xids of the test's choosing,
-- including a wrap of the xid counter (the special xids 0 to 2 are skipped) and the 64 xids a stamp reaches; a union that cannot say what both say says nothing.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT what, got FROM (VALUES
	(1, 'no one in doubt', sift._test_xstamp_pack(100, '{}')),
	(2, 'one, the latest', sift._test_xstamp_pack(100, '{100}')),
	(3, 'one, before the latest xid of the rows', sift._test_xstamp_pack(100, '{90}')),
	(4, 'two, ten apart', sift._test_xstamp_pack(100, '{90,100}')),
	(5, 'the same one twice', sift._test_xstamp_pack(100, '{90,90}')),
	(6, 'the farthest it reaches (63 apart)', sift._test_xstamp_pack(100, '{37,100}')),
	(7, 'one too far (64 apart)', sift._test_xstamp_pack(100, '{36,100}')),
	(8, 'no latest inserter', sift._test_xstamp_pack(0, '{}')),
	(9, 'a special xid is none', sift._test_xstamp_pack(2, '{}')),
	(10, 'across the wrap of the counter', sift._test_xstamp_pack(5, '{4294967295,5}')),
	(11, 'plain with plain', sift._test_xstamp_union('101:0:0', '201:0:0')),
	(12, 'plain with plain, the other way', sift._test_xstamp_union('201:0:0', '101:0:0')),
	(13, 'waiting with plain', sift._test_xstamp_union('101:100:1', '201:0:0')),
	(14, 'plain with waiting', sift._test_xstamp_union('201:0:0', '101:100:1')),
	(15, 'two that wait, the first the later', sift._test_xstamp_union('101:100:1', '201:90:1')),
	(16, 'two that wait, the second the later', sift._test_xstamp_union('101:90:1', '201:100:1')),
	(17, 'the same transaction in both', sift._test_xstamp_union('101:100:1', '201:100:1')),
	(18, 'pushed out of the word', sift._test_xstamp_union('101:100:8000000000000001', '201:101:1')),
	(19, 'just in the word', sift._test_xstamp_union('101:100:4000000000000001', '201:101:1')),
	(20, 'a segment that says nothing', sift._test_xstamp_union('none', '101:0:0')),
	(21, 'with a segment that says nothing', sift._test_xstamp_union('101:100:1', 'none')),
	(22, 'across the wrap of the counter', sift._test_xstamp_union('6:5:9', '100:4294967290:1')),
	(23, 'across the wrap, too far', sift._test_xstamp_union('6:5:1', '100:4294967200:1'))
) t(n, what, got) ORDER BY n;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
