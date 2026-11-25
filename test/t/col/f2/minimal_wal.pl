# Format major 2 under wal_level = minimal: an index built in the transaction that created its table writes no WAL (the pages are synced at commit), and it is intact after a crash.
# Under wal_level = replica the build of the same index writes a log of about the size of the index.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use Test::More;

my $rows = knob('SIFT_ROWS', 60000, 20000);
my $node = sift_node('minimal');
$node->append_conf('postgresql.conf', "sift.test_create_format1 = off\nwal_level = minimal\nmax_wal_senders = 0\nwal_skip_threshold = 0");
$node->start;
install_sift_test($node, 'the index is read through sift._test_f2_check_keys');

my $load = qq{
	CREATE TABLE ev (id bigint, tenant int, status text, name text, amount numeric(12, 2), score float8);
	INSERT INTO ev SELECT i, i % 50, (ARRAY['new', 'open', 'closed'])[1 + i % 3], 'user-' || (i % 5000), (i % 10000) / 100.0, (i % 977) / 10.0 FROM generate_series(1, $rows) i;};
my $index = "CREATE INDEX ev_idx ON ev USING sift_col (id, tenant, status, name, amount, score) WITH (key_order = 'id')";

my $before = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->safe_psql('postgres', "BEGIN; $load $index; COMMIT;");
my $after = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $bytes = $node->safe_psql('postgres', "SELECT '$after'::pg_lsn - '$before'::pg_lsn");
cmp_ok($bytes / $rows, '<=', 1.0, "the table and its index, built in one transaction under wal_level = minimal, log at most one byte a row ($bytes bytes, $rows rows)");

my $size = $node->safe_psql('postgres', "SELECT pg_relation_size('ev_idx')");
cmp_ok($size / $rows, '<=', 30, 'the index takes at most 30 bytes a row');
is($node->safe_psql('postgres', "SELECT sift._test_f2_check_keys('ev_idx')"), "parts=1 granules=" . int(($rows + 8191) / 8192) . " rows=$rows exact", 'its keys ascend');

my $same = "SELECT string_agg(id::text, ',' ORDER BY id) FROM ev WHERE tenant = 7 AND status = 'open'";
my $heap = $node->safe_psql('postgres', "SET enable_bitmapscan = off; SET enable_indexscan = off; $same");
my $inline = "SELECT i::text FROM sift._test_f2_meta_inline('ev_idx') i";
my $summary = $node->safe_psql('postgres', $inline);
like($summary, qr/^\(\d+,\d+,1,t\)$/, 'block 0 keeps the summary of the partition of the index');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $inline), $summary, 'after a crash the summary in block 0 is the one the directory has (the check of the manifest compares them)');
is($node->safe_psql('postgres', "SELECT sift._test_f2_check_keys('ev_idx')"), "parts=1 granules=" . int(($rows + 8191) / 8192) . " rows=$rows exact", 'after a crash the parts are there');
is($node->safe_psql('postgres', "SET enable_seqscan = off; SET enable_indexscan = off; $same"), $heap, 'the index answers as the heap does');

$node->stop;
done_testing();
