# sift_table: VACUUM clears the live bits of a column group in place while a statement is fetching rows of it by TID. The fetch cached the
# TIDs of the live rows of a superblock, and found the row's place in the unit's vectors only later, from the live bits as they were by
# then: the TIDs of the rows VACUUM had removed in between were still in the cache, so the place was the one of a neighbour. The row read
# was a neighbour of the one asked for, and an UPDATE wrote the neighbour's id into the new version (duplicate key under updates and
# VACUUM side by side).
# One statement probes the index with a deleted row (its TID is cached, the row is not visible), waits on an advisory lock that another
# session holds while it vacuums, then probes a live row. The group has been vacuumed once before: the live bits of a group are only
# read from the store after its first VACUUM.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = sift_node('fetch');
$node->start;
install_sift($node);
$node->safe_psql('postgres', 'CREATE TABLE g (id int PRIMARY KEY, v int) USING sift_table; INSERT INTO g SELECT i, i * 10 FROM generate_series(1, 2000) i; VACUUM g');
is($node->safe_psql('postgres', "SELECT count(*) FROM sift.table_segments('g') WHERE kind = 'columns'"), 1, 'the rows are in a column group');
$node->safe_psql('postgres', 'DELETE FROM g WHERE id <= 10');
$node->safe_psql('postgres', 'VACUUM g');
$node->safe_psql('postgres', 'DELETE FROM g WHERE id BETWEEN 11 AND 20');

my $locker = $node->background_psql('postgres');
$locker->query_safe('SELECT pg_advisory_lock(7)');
my $probe = $node->background_psql('postgres');
my $q = 'SET enable_seqscan = off; SET enable_bitmapscan = off; '
	. 'SELECT i, (SELECT v FROM g WHERE id = i) FROM (VALUES (15), (100)) q (i) WHERE i <> 100 OR pg_advisory_lock(7) IS NOT NULL';
$probe->query_until(qr/go/, "\\echo go\n$q;\n\\echo done\n");
$node->poll_query_until('postgres', "SELECT count(*) = 1 FROM pg_locks WHERE locktype = 'advisory' AND NOT granted");
$locker->query_safe('VACUUM g');
$locker->query_safe('SELECT pg_advisory_unlock(7)');
my $out = $probe->query_until(qr/done/, '');
like($out, qr/^100\|1000\s*$/m, 'the row probed after VACUUM is the one asked for');
$probe->quit;
$locker->quit;

$node->stop;
done_testing();
