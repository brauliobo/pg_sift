# The change records of a published format 2 sift_table: a row copied from a heap table carries its values and not a pointer into that table's TOAST relation,
# and an UPDATE or DELETE names the row by its replica identity key alone
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib "$ENV{SIFT_REPO}/test/t/lib";
use SiftTest;
use PostgreSQL::Test::Cluster;
use Test::More;

my $primary = sift_node('primary', allows_streaming => 'logical');
$primary->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_sift'\nsift.test_create_format1 = off\nautovacuum = off");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_sift');

subtest 'R1-04: the record of a row copied from a heap table holds its values (format 2)' => sub {
	$primary->safe_psql('postgres', q{
		CREATE TABLE heap_src (id int PRIMARY KEY, big text);
		ALTER TABLE heap_src ALTER COLUMN big SET STORAGE EXTERNAL;
		INSERT INTO heap_src SELECT 1, string_agg(md5(g::text), '') FROM generate_series(1, 4000) g;
		CREATE TABLE pub_t (id int PRIMARY KEY, big text) USING sift_table;
		CREATE PUBLICATION pub_t_all FOR TABLE pub_t;
		SELECT pg_create_logical_replication_slot('toast_slot', 'pgoutput');
		INSERT INTO pub_t SELECT * FROM heap_src;
		DROP TABLE heap_src;});
	my $changes = q{SELECT count(*) FROM pg_logical_slot_get_binary_changes('toast_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'pub_t_all') WHERE length(data) > 128000};
	is($primary->safe_psql('postgres', $changes), 1, 'the insert is decoded with the whole value');
};

subtest 'R1-14: an UPDATE and a DELETE of a row with a wide value name it by the primary key' => sub {
	$primary->safe_psql('postgres', q{
		SELECT pg_create_logical_replication_slot('key_slot', 'pgoutput');
		UPDATE pub_t SET big = big || 'x' WHERE id = 1;
		DELETE FROM pub_t WHERE id = 1;});
	my $keys = q{SELECT string_agg(chr(get_byte(data, 0)) || length(data), ',' ORDER BY lsn) FROM pg_logical_slot_get_binary_changes('key_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'pub_t_all')
		WHERE get_byte(data, 0) IN (85, 68)};
	my $got = $primary->safe_psql('postgres', $keys);
	like($got, qr/^U\d+,D\d+$/, 'one update and one delete are decoded');
	my ($d) = $got =~ /D(\d+)/;
	cmp_ok($d, '<', 100, 'the delete carries the key, not the row');
};

done_testing();
