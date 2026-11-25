# Durability of a sift_table whose pages carry no LSN, the init of its pages on a standby, its projections after the index is rebuilt, its logical changes and
# the log of its visibility map:
#  - a table that writes no WAL (UNLOGGED, TEMP) keeps its page LSNs at 0, so the cache of a chain of pending deletes must tell an appended page from the one it read by what
#    the page holds, not by its LSN
#  - CREATE TABLE logs every page of the metadata area, so a standby has as many blocks as the primary
#  - sift.projection_defs outlives the projections of the relation (REINDEX, reset of an unlogged relation): the list shows them and a second create is refused as a duplicate
#  - a published table without the identity its UPDATE or DELETE needs refuses it, as a heap table does
#  - the first bits of a page of the visibility map are logged without a full image of the page they build
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = sift_node('primary', allows_streaming => 'logical');
$primary->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_sift'\nsift.test_create_format1 = off\nautovacuum = off\nfull_page_writes = on\nwal_compression = off");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_sift');

subtest 'R1-01: a table that writes no WAL reads the deletes and the updates it made after it cached its chains' => sub {
	my $rows = 'INSERT INTO %s SELECT g, g, g, repeat(\'x\', 40), g FROM generate_series(1, 2000) g';
	my $cols = '(id int PRIMARY KEY, k int, v int, pad text, w int) USING sift_table';
	for my $kind ('UNLOGGED', 'TEMP')
	{
		my $out = $primary->safe_psql('postgres', join(";\n",
			"CREATE $kind TABLE r1 $cols", sprintf($rows, 'r1'), 'VACUUM r1',
			'DELETE FROM r1 WHERE id = 20', 'SELECT count(*) FROM r1',
			'DELETE FROM r1 WHERE id = 30', 'SELECT count(*) FROM r1 WHERE id = 30',
			'UPDATE r1 SET v = -1 WHERE id = 10', 'SELECT v FROM r1 WHERE id = 10',
			'UPDATE r1 SET v = 9999 WHERE id = 10', 'SELECT v FROM r1 WHERE id = 10', 'DROP TABLE r1'));
		is($out, "1999\n0\n-1\n9999", "$kind: a point read after the second delete and the second update sees them");
	}
};

subtest 'R1-04: the change record of a row copied from a heap table holds its values, not a pointer into that table (format 1)' => sub {
	$primary->safe_psql('postgres', q{
		SET sift.test_create_format1 = on;
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

subtest 'R1-15: the first bits of a page of the visibility map are logged without an image of the page' => sub {
	my $from = $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$primary->safe_psql('postgres', q{
		CREATE TABLE vm (id int PRIMARY KEY, k int, v int, pad text, w int) USING sift_table;
		INSERT INTO vm SELECT g, g, g, repeat('x', 40), g FROM generate_series(1, 2000) g;
		CREATE INDEX vm_k ON vm (k);
		VACUUM vm;
		UPDATE vm SET v = -1 WHERE id = 1;
		VACUUM vm;});
	my ($out, $err) = ('', '');
	IPC::Run::run([ 'pg_waldump', '-p', $primary->data_dir . '/pg_wal', '-s', $from ], '>', \$out, '2>', \$err);
	my @sets = grep { /rmgr: custom142 .*fork vm/ } split /\n/, $out;
	ok(@sets > 0, 'VACUUM logs the bits of the map') or diag("$out\n$err");
	is(scalar(grep { /\bFPW\b/ } @sets), 0, 'no record of them carries an image of the page') or diag(join("\n", @sets));
};

subtest 'R1-09: a standby has the blocks of the metadata area of a new table' => sub {
	$primary->backup('b');
	my $standby = PostgreSQL::Test::Cluster->new('standby');
	$standby->init_from_backup($primary, 'b', has_streaming => 1);
	$standby->start;
	$primary->safe_psql('postgres', q{
		CREATE TABLE e (id int) USING sift_table;
		CREATE TABLE hx (id int, a int);
		CREATE INDEX hx_col ON hx USING sift_col (id, a);});
	$primary->wait_for_catchup($standby);
	for my $rel ('e', 'hx_col')
	{
		my $size = "SELECT pg_relation_size('$rel') / 8192";
		is($standby->safe_psql('postgres', $size), $primary->safe_psql('postgres', $size), "$rel has as many blocks on the standby as on the primary");
	}
	$standby->stop;
};

subtest 'R1-08: the projections of a relation whose directory lost them stay listed and cannot be created twice' => sub {
	$primary->safe_psql('postgres', q{
		CREATE TABLE ph (id int PRIMARY KEY, a int, b int);
		INSERT INTO ph SELECT g, g % 10, g % 20 FROM generate_series(1, 500) g;
		CREATE INDEX ph_col ON ph USING sift_col (id, a, b);
		SELECT sift.create_projection('ph_col', 'by_b', 'ORDER BY b');
		CREATE UNLOGGED TABLE pu (id int PRIMARY KEY, a int, b int) USING sift_table;
		SELECT sift.create_projection('pu', 'by_b', 'ORDER BY b');
		REINDEX INDEX ph_col;});
	$primary->stop('immediate');
	$primary->start;
	my $listed = "SELECT relation || ':' || name || ':' || state FROM sift.secondary_projections ORDER BY relation::text";
	is($primary->safe_psql('postgres', $listed), "ph_col:by_b:building\npu:by_b:building", 'a definition without a projection in the directory is listed as building');
	for my $rel ('ph_col', 'pu')
	{
		my ($ret, $out, $err) = $primary->psql('postgres', "SELECT sift.create_projection('$rel', 'by_b', 'ORDER BY b')", extra_params => [ '-v', 'VERBOSITY=verbose' ]);
		like($err, qr/ERROR:  42710: relation "$rel" already has a projection "by_b"/, "$rel: a second create is a duplicate object");
	}
};

subtest 'R1-05: a published table refuses an UPDATE and a DELETE without a replica identity' => sub {
	for my $format (1, 2)
	{
		my $on = $format == 1 ? 'on' : 'off';
		$primary->safe_psql('postgres', "SET sift.test_create_format1 = $on; CREATE TABLE nopk$format (id int, a int, b int, c int, v int) USING sift_table; INSERT INTO nopk$format VALUES (1, 1, 1, 1, 1)");
		$primary->safe_psql('postgres', "CREATE PUBLICATION pub$format FOR TABLE nopk$format");
		for my $dml ("UPDATE nopk$format SET v = v + 1 WHERE id = 1", "DELETE FROM nopk$format WHERE id = 1")
		{
			my ($ret, $out, $err) = $primary->psql('postgres', $dml, extra_params => [ '-v', 'VERBOSITY=verbose' ]);
			like($err, qr/ERROR:  55000: cannot (update|delete from) table "nopk$format" because it does not have a replica identity/, "format $format: $dml");
		}
	}
};

done_testing();
