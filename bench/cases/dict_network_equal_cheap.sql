-- an equality on a network column of IPv4 and IPv6 addresses cut by key_cut: the dictionary of the leaf does not fit the cache of parsed dictionaries, so every statement reads its blocks again, and the blocks are LZ4 where the default formula says so
-- label: `count(*)` of `WHERE ip = <an IPv6 address with a mask>` over half IPv4 and half IPv6 addresses (random masks), no btree on `ip`; the lane `base` is the index written under `sift.col_format_minor = 14` (zstd blocks)
-- ordered: no
-- base: sift.col_format_minor = 14
--@ setup
DROP INDEX orders_col;
ALTER TABLE orders ADD COLUMN ip inet;
UPDATE orders SET ip = CASE WHEN id % 2 = 0 THEN set_masklen((format('2001:db8:%s:%s:%s:%s:%s:%s', to_hex((hashint8extended(id, 11) & 65535)::int), to_hex((hashint8extended(id, 12) & 65535)::int), to_hex((hashint8extended(id, 13) & 65535)::int), to_hex((hashint8extended(id, 14) & 65535)::int), to_hex((hashint8extended(id, 15) & 65535)::int), to_hex((hashint8extended(id, 16) & 65535)::int)))::inet, 32 + (hashint8extended(id, 21) & 127)::int % 97) ELSE set_masklen((inet '20.0.0.0' + (hashint8extended(id, 5) & 16777215)::int), 8 + (hashint8extended(id, 22) & 31)::int % 25) END;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, ip, amount sift.col_numeric_ops) WITH (key_cut = 'ip#1')
--@ pg
SELECT count(*) AS n FROM orders WHERE ip = '2001:db8:62bd:7b60:bd2f:dfe1:790c:7a01/86'
