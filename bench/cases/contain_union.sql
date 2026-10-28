-- how many of 1,000,000 random networks of both families overlap one network, no GiST: `&&` holds when a network is inside the constant or contains it, and the units that neither can hold are pruned on the zones of the node lane
-- label: `SELECT count(*) WHERE ip && '2001:db8:1::/48'` on 1,000,000 networks, half IPv4 (20.0.0.0 to 20.255.255.255, masks 8 to 32) and half IPv6 (2001:db8::/32, masks 32 to 128); the variant `gist` is the same rows with a GiST index (inet_ops) and no sift_col index
--@ setup
DROP SCHEMA IF EXISTS contain_union_gist CASCADE;
CREATE SCHEMA contain_union_gist;
CREATE TABLE nets AS SELECT id, CASE WHEN id % 2 = 0
	THEN set_masklen(format('2001:db8:%s:%s:%s:%s:%s:%s', to_hex((hashint8extended(id, 11) & 65535)::int), to_hex((hashint8extended(id, 12) & 65535)::int), to_hex((hashint8extended(id, 13) & 65535)::int),
		to_hex((hashint8extended(id, 14) & 65535)::int), to_hex((hashint8extended(id, 15) & 65535)::int), to_hex((hashint8extended(id, 16) & 65535)::int))::inet, 32 + (hashint8extended(id, 21) & 127)::int % 97)
	ELSE set_masklen(inet '20.0.0.0' + (hashint8extended(id, 5) & 16777215)::int, 8 + (hashint8extended(id, 22) & 31)::int % 25) END AS ip FROM orders;
CREATE INDEX nets_col ON nets USING sift_col (ip);
CREATE TABLE contain_union_gist.nets AS TABLE nets;
CREATE INDEX nets_gist ON contain_union_gist.nets USING gist (ip inet_ops);
VACUUM (FREEZE, ANALYZE) nets;
VACUUM (FREEZE, ANALYZE) contain_union_gist.nets;
--@ pg
SELECT count(*) FROM nets WHERE ip && inet '2001:db8:1::/48'
