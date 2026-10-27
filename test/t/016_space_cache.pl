# Relocation publication survives ERROR without losing persistent backends' write targets.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = sift_node('space_cache');
$node->append_conf('postgresql.conf', "sift.async_maintenance = off\nmax_parallel_maintenance_workers = 0");
$node->start;
install_sift_test($node, 'publication error injection needs the testing build');
sub fixture
{
	my ($kind) = @_;
		my $am = $kind eq 'tbl' ? ' USING sift_table' : '';
		$node->safe_psql('postgres', "CREATE TABLE s (id int, v bigint, body text)$am WITH (autovacuum_enabled = false, vacuum_truncate = false)");
		$node->safe_psql('postgres', "CREATE INDEX idx ON s USING " . ($kind eq 'col' ? 'sift_col(v, body)' : 'sift(body)') . ' WITH (vacuum_truncate = false)') unless $kind eq 'tbl';
		my $rel = $kind eq 'tbl' ? 's' : 'idx';
		$node->safe_psql('postgres', "INSERT INTO s VALUES (0,0,'initialize')") if $kind eq 'tbl';
		$node->safe_psql('postgres', "SELECT sift._test_alloc('$rel', 512, 1, 0); INSERT INTO s SELECT g, g * 3, repeat(md5(g::text), 2) FROM generate_series(1,60000) g");
		$node->safe_psql('postgres', 'UPDATE s SET v = v + 1, body = body || chr(32) WHERE id % 10 = 0; DELETE FROM s WHERE id % 20 = 1; VACUUM s');
		$node->safe_psql('postgres', $kind eq 'tbl' ? 'ALTER TABLE s RESET (vacuum_truncate)' : 'ALTER INDEX idx RESET (vacuum_truncate)');
	my $view = $kind eq 'tbl' ? "sift.table_segments('s')" : $kind eq 'col' ? "sift.col_segments('idx')" : "sift.segments('idx')";
	return ($rel, $view);
}
for my $kind (qw(bm25 col tbl))
{
	subtest "$kind cached writer after publication ERROR without truncate" => sub {
		my ($rel, $view) = fixture($kind);
		my $writer = $node->background_psql('postgres');
		$writer->query_safe("INSERT INTO s VALUES (100001, 7, 'primed cached writer')");
		my $before = $node->safe_psql('postgres', "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM $view");
		my $bytes = $node->safe_psql('postgres', "SELECT pg_relation_size('$rel')");
		my ($out, $err) = ('', '');
		my $rc = $node->psql('postgres', 'SET sift.test_space_error = on; VACUUM s', stdout => \$out, stderr => \$err);
		ok($rc != 0 && $err =~ /error after open buffer relocation publication/, 'ordinary ERROR after publishing the open buffer');
		my ($old_header, $new_header) = $err =~ /header (\d+) to (\d+)/;
		ok(defined($old_header) && defined($new_header) && $old_header != $new_header, 'open buffer header moved before the error');
		isnt($node->safe_psql('postgres', "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM $view"), $before, 'replacement directory survives ERROR');
		is($node->safe_psql('postgres', "SELECT pg_relation_size('$rel')"), $bytes, 'physical truncation did not send an invalidation');
		$writer->query_safe("INSERT INTO s VALUES (100002, 11, 'uniquesentinel cached writer')");
		$writer->quit;
		my $query = $kind eq 'bm25' ? "SET enable_seqscan = off; SELECT count(*) FROM s WHERE body @@> 'uniquesentinel'" : $kind eq 'col' ? 'SET enable_seqscan = off; SELECT count(*) FROM s WHERE v = 11' : 'SELECT count(*) FROM s WHERE id = 100002';
		is($node->safe_psql('postgres', $query), '1', 'fresh directory-backed scan finds the persistent writer sentinel');
		is($node->psql('postgres', "SELECT sift.verify('idx', true)"), 0, 'BM25 verification succeeds') if $kind eq 'bm25';
		is($node->psql('postgres', "SELECT sift.col_verify('idx', true)"), 0, 'column verification succeeds') if $kind eq 'col';
		is($node->safe_psql('postgres', "SELECT sift._test_check('$rel')"), '0', 'all allocated pages still have one owner');
		$node->safe_psql('postgres', 'DROP TABLE s');
	};
}
for my $kind (qw(bm25 col tbl))
{
	subtest "$kind unpublished copy error cleanup" => sub {
		my ($rel, $view) = fixture($kind);
		my $before = $node->safe_psql('postgres', "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM $view");
		my ($out, $err) = ('', '');
		my $rc = $node->psql('postgres', 'SET sift.test_fail_pages = 1; VACUUM s', stdout => \$out, stderr => \$err);
		is($rc, 0, 'automatic relocation failure does not fail VACUUM');
		like($err, qr/WARNING:.*injected failure after 1 pages/s, 'copy actually failed under the maintenance error contract');
		is($node->safe_psql('postgres', "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM $view"), $before, 'unpublished replacement never entered the directory');
		is($node->safe_psql('postgres', "SELECT sift._test_check('$rel')"), '0', 'aborted target returned every allocated extent');
		is($node->safe_psql('postgres', 'SET sift.enable_agg = off; SELECT count(*) FROM s'), '57000' + ($kind eq 'tbl' ? 1 : 0), 'rows survive the failed copy');
		$node->safe_psql('postgres', 'DROP TABLE s');
	};
}
$node->stop;
done_testing();
