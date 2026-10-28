-- how many of 1,000,000 random addresses lie in a network, no GiST: the node of each address in the tree of prefixes is an integer lane of the sift_col index, so `<<=` is a range of nodes on the zones and the lane, not the operator on each address
-- label: `SELECT count(*) WHERE ip <<= '20.7.0.0/16'` on 1,000,000 random IPv4 addresses from 20.0.0.0 to 20.255.255.255; the variant `gist` is the same rows with a GiST index (inet_ops) and no sift_col index
--@ setup
DROP SCHEMA IF EXISTS contain_inet_gist CASCADE;
CREATE SCHEMA contain_inet_gist;
CREATE TABLE hosts AS SELECT id, inet '20.0.0.0' + (hashint8extended(id, 5) & 16777215)::int AS ip FROM orders;
CREATE INDEX hosts_col ON hosts USING sift_col (ip);
CREATE TABLE contain_inet_gist.hosts AS TABLE hosts;
CREATE INDEX hosts_gist ON contain_inet_gist.hosts USING gist (ip inet_ops);
VACUUM (FREEZE, ANALYZE) hosts;
VACUUM (FREEZE, ANALYZE) contain_inet_gist.hosts;
--@ pg
SELECT count(*) FROM hosts WHERE ip <<= inet '20.7.0.0/16'
