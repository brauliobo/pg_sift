# sift_table: several writers insert and update rows that span 2 to 5 pages (overflow pages) while VACUUM runs, so that the write buffers
# fill up and are replaced in the middle of rows. Every row must come back byte for byte (the overflow pages of a row once landed in
# another write buffer than its record under concurrent inserts and the read failed).
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $clients = 8;
my $txns = knob('SIFT_TXNS', 150, 40);
my $node = sift_node('main');
$node->start;
install_sift($node);
$node->safe_psql('postgres', 'CREATE TABLE w (id bigint, v text) USING sift_table');

# The first write of a new table lays out its tombstone segment and flags the metapage. A session that read the table meanwhile (the metapage
# is there, the segment not yet) must not take the table for laid out: its own first write then added a write buffer to a directory that
# was never flagged, and every later read of the table failed as corrupt.
$node->safe_psql('postgres', 'CREATE TABLE g (id int) USING sift_table');
my $gate = $node->background_psql('postgres');
my $first = $node->background_psql('postgres');
my $reader = $node->background_psql('postgres');
$gate->query_safe('SELECT pg_advisory_lock(7101)');
$first->query_until(qr/stalled/, "SET sift.table_init_gate = 7101;\n\\echo stalled\nINSERT INTO g VALUES (1);\n");
$node->poll_query_until('postgres', "SELECT count(*) = 1 FROM pg_locks WHERE locktype = 'advisory' AND NOT granted");
$reader->query_safe('SELECT count(*) FROM g');
# (psql stops at the first error, which ends the session)
ok(eval { $reader->query_safe('INSERT INTO g VALUES (2)'); 1 }, 'a session that read the table before its first write lays it out itself');
$gate->query_safe('SELECT pg_advisory_unlock(7101)');
$first->query_safe('SELECT 1');
$node->psql('postgres', 'SELECT count(*) FROM g', stdout => \my $rows_in_g);
is($rows_in_g, 2, 'and the stalled first writer finds it laid out: both rows are there');
$_->quit for ($gate, $first);
eval { $reader->quit };

my $dir = PostgreSQL::Test::Utils::tempdir;
my $len = '4100 + (:id % 7)::int * 5000';
open(my $f, '>', "$dir/ins.sql") or die;
print $f "\\set cnt :cnt + 1\n\\set id :client_id * 1000000 + :cnt\nINSERT INTO w SELECT :id, repeat(chr(97 + (:id % 26)::int), $len);\n";
close $f;
open($f, '>', "$dir/upd.sql") or die;
print $f "\\set id :client_id * 1000000 + random(1, 20)\nUPDATE w SET v = repeat(chr(97 + (id % 26)::int), 4100 + (id % 7)::int * 5000) WHERE id = :id;\n";
close $f;
open($f, '>', "$dir/vac.sql") or die;
print $f "VACUUM w;\n";
close $f;

$node->command_ok(['pgbench', '-n', '-h', $node->host, '-p', $node->port, '-c', $clients, '-j', $clients, '-t', $txns, '-D', 'cnt=0',
		'-f', "$dir/ins.sql\@20", '-f', "$dir/upd.sql\@3", '-f', "$dir/vac.sql\@1", 'postgres'], 'writers, updaters and VACUUM run side by side');

my $wrong = 'SELECT count(*) FROM w WHERE v <> repeat(chr(97 + (id % 26)::int), 4100 + (id % 7)::int * 5000)';
my $rows = $node->safe_psql('postgres', 'SELECT count(*) FROM w');
cmp_ok($rows, '>', $clients * $txns / 2, "rows inserted: $rows");
is($node->safe_psql('postgres', 'SELECT count(*) - count(DISTINCT id) FROM w'), 0, 'no row twice');
is($node->safe_psql('postgres', $wrong), 0, 'every row has its bytes');
$node->safe_psql('postgres', 'VACUUM w');
is($node->safe_psql('postgres', 'SELECT count(*) FROM w'), $rows, 'the same rows after VACUUM');
is($node->safe_psql('postgres', $wrong), 0, 'and the same bytes, now in column groups');
is($node->safe_psql('postgres', 'SELECT count(*) > 1 FROM sift.table_segments($$w$$)'), 't', 'several segments were involved');

$node->stop;
done_testing();
