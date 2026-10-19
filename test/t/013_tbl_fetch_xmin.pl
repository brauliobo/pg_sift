# sift_table: the scan that serves the fetches of rows by TID is kept from one transaction to the next, not rebuilt after every transaction
# that wrote (a write moves the xmin of the next snapshot, and the scan was dropped whenever the xmin changed: every UPDATE decoded the
# columns of its unit again). A newer xmin keeps the segments only while the directory is the one the scan was begun with.
# One session fetches rows by TID, one statement per transaction, while another commits writes (the xmin moves, the directory does
# not), then deletes, vacuums twice and inserts (segments leave the directory, their blocks are recycled and written again): every
# fetch must read the row asked for, from the segments that hold it.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = sift_node('fetchxmin');
$node->start;
install_sift($node);
$node->safe_psql('postgres', 'CREATE TABLE g (id int PRIMARY KEY, v int) USING sift_table; INSERT INTO g SELECT i, i * 10 FROM generate_series(1, 3000) i; VACUUM g');
is($node->safe_psql('postgres', "SELECT count(*) FROM sift.table_segments('g') WHERE kind = 'columns'"), 1, 'the rows are in a column group');

my $probe = $node->background_psql('postgres');
$probe->query_safe('SET enable_seqscan = off; SET enable_bitmapscan = off');
my $by_index = 'SELECT string_agg(id || \':\' || v, \',\' ORDER BY id) FROM g WHERE id = ANY($1)';
sub fetched
{
	my ($ids) = @_;
	(my $q = $by_index) =~ s/\$1/ARRAY[$ids]/;
	my $out = $probe->query_safe($q);
	$out =~ s/\s+//g;
	return $out;
}

$probe->query_safe('UPDATE g SET v = v + 1 WHERE id = 5');
$node->safe_psql('postgres', 'UPDATE g SET v = v + 1 WHERE id = 6');
$probe->query_safe('UPDATE g SET v = v + 1 WHERE id = 7');
is(fetched('5,6,7,8'), '5:51,6:61,7:71,8:80', 'rows fetched after the xmin moved, the directory the same');

$node->safe_psql('postgres', 'UPDATE g SET v = v + 1 WHERE id = 2000');
$probe->query_safe('UPDATE g SET v = v + 1 WHERE id = 2001');
is(fetched('5,2000,2001'), '5:51,2000:20001,2001:20011', 'rows of another superblock too');

# the segments leave the directory: the group is compacted, its blocks recycled and written again by the rows inserted next
$node->safe_psql('postgres', 'DELETE FROM g WHERE id > 100 AND id <= 2500');
$node->safe_psql('postgres', 'VACUUM g');
$node->safe_psql('postgres', 'VACUUM g');
$node->safe_psql('postgres', 'INSERT INTO g SELECT i, i * 10 FROM generate_series(5001, 6000) i; VACUUM g');
$probe->query_safe('UPDATE g SET v = v + 1 WHERE id = 5500');
$probe->query_safe('UPDATE g SET v = v + 1 WHERE id = 3000');
$probe->query_safe('UPDATE g SET v = v + 1 WHERE id = 5');
is(fetched('5,3000,5500,5999'), '5:52,3000:30001,5500:55001,5999:59990', 'rows fetched after the directory changed');
is($node->safe_psql('postgres', "SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM g WHERE id IN (5, 3000, 5500, 5999)"),
	'5:52,3000:30001,5500:55001,5999:59990', 'a new session reads the same rows');

$probe->quit;
$node->stop;
done_testing();
