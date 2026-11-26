-- sift.locks names every virtual block that a Format 2.0 relation locks (core/part_lock.h, table/f2/lock/esc.h and 2pc.h), at the first and the last block of each range;
-- pg_locks.page is a signed int4, so a block above 2^31 comes back as a negative number.
CREATE EXTENSION pg_sift;
SELECT b.name, b.blk, k.kind, k.detail
FROM (VALUES ('unique key first', 4294934528), ('unique key last', 4294950911), ('relation key', 4294955008), ('partition key first', 4294955009), ('partition key last', 4294959103),
			 ('relation rows', 4294959104), ('partition rows first', 4294959105), ('partition rows last', 4294963199), ('partition first', 4294963200), ('partition last', 4294964223),
			 ('maintenance', 4294965248), ('load', 4294965249), ('worker', 4294965250), ('split', 4294965251), ('vacuum read', 4294965252), ('row 2pc', 4294965264),
			 ('compaction first', 4294966272), ('compaction last', 4294967294), ('a data block', 42), ('below the unique keys', 4294934527)) b(name, blk),
	 LATERAL sift_internal.lock_block(b.blk) k
ORDER BY b.blk;
DROP EXTENSION pg_sift;
