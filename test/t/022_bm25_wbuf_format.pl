# The write buffer of a BM25 index in format 6 (a bloom of 8,192 bits for each full page, bm25/wbuf.c): a hot standby reads the pages after replay,
# crash recovery of a page that was being filled keeps the buffer verified and the rows, and a buffer of format 0 that is open when the
# library is upgraded (sift.test_wbuf_narrow stands for the older library) is read and appended to as it is, and the buffer opened after its promotion is format 6.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = sift_node('primary', allows_streaming => 1);
$primary->start;
install_sift_test($primary, 'needs the testing build (pg_sift_test)');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

# 120 terms to a document, 'common' in each: a page of a few documents holds several hundred different terms
my $doc = q{'common ' || (SELECT string_agg('t' || ((i * 7 + k * 13) % 4001), ' ') FROM generate_series(1, 120) k)};
sub rows { "INSERT INTO $_[0] SELECT i, $doc FROM generate_series($_[1], $_[2]) i" }
my $probe = sub { my ($node, $t) = @_;
	$node->safe_psql('postgres', "SET enable_seqscan = off; SELECT (SELECT count(*) FROM $t WHERE body \@\@> 'common')"
		. " || ' ' || (SELECT count(*) FROM $t WHERE body \@\@> 't7') || ' ' || (SELECT count(*) FROM $t WHERE body \@\@> 'qqabsent')"
		. " || ' ' || (SELECT count(*) FROM $t WHERE body ~ '\\yt7\\y')") };
sub fmt { $_[0]->safe_psql('postgres', "SELECT format FROM sift._test_wbuf_reads('$_[1]')") }

subtest 'format 6 buffer on a standby' => sub {
	$primary->safe_psql('postgres', "CREATE TABLE w (id int, body text) WITH (autovacuum_enabled = off); CREATE INDEX w_i ON w USING sift (body)");
	$primary->safe_psql('postgres', rows('w', 1, 1500));
	$primary->wait_for_catchup($standby);
	is(fmt($primary, 'w_i'), 6, 'the buffer is format 6');
	my @p = split ' ', $probe->($primary, 'w');
	is($p[0], 1500, 'every document is in the buffer index');
	is($p[2], 0, 'a term in no document');
	is($p[1], $p[3], 'a term agrees with the regular expression on the heap');
	is($probe->($standby, 'w'), $probe->($primary, 'w'), 'the standby reads the pages after replay as the primary');
	$standby->safe_psql('postgres', "SELECT sift.verify('w_i')");
	$primary->safe_psql('postgres', "SELECT sift.verify('w_i')");
	pass('both verify, the wide blooms included');
};

subtest 'crash recovery of a page being filled' => sub {
	$primary->safe_psql('postgres', rows('w', 1501, 1517));
	my $before = $probe->($primary, 'w');
	$primary->stop('immediate');
	$primary->start;
	is($probe->($primary, 'w'), $before, 'the rows after crash recovery');
	$primary->safe_psql('postgres', "SELECT sift.verify('w_i')");
	pass('verified after recovery');
	$primary->safe_psql('postgres', rows('w', 1518, 3000));
	$primary->safe_psql('postgres', "SELECT sift.verify('w_i')");
	$primary->wait_for_catchup($standby);
	is($probe->($standby, 'w'), $probe->($primary, 'w'), 'the standby again');
};

subtest 'a buffer of format 0 open at the upgrade' => sub {
	$primary->safe_psql('postgres', "CREATE TABLE o (id int, body text) WITH (autovacuum_enabled = off); CREATE INDEX o_i ON o USING sift (body)");
	$primary->safe_psql('postgres', "SET sift.test_wbuf_narrow = on; " . rows('o', 1, 400) . "; SELECT 1");
	is(fmt($primary, 'o_i'), 0, 'opened by the older library: format 0');
	$primary->safe_psql('postgres', rows('o', 401, 1500));	# the library of this tree appends to it
	is(fmt($primary, 'o_i'), 0, 'and it stays format 0 while it is open');
	my @p = split ' ', $probe->($primary, 'o');
	is($p[0], 1500, 'all the documents');
	is($p[1], $p[3], 'a term agrees with the heap');
	$primary->safe_psql('postgres', "SELECT sift.verify('o_i')");
	$primary->safe_psql('postgres', "SELECT sift.promote('o_i')");
	$primary->safe_psql('postgres', rows('o', 1501, 1600));
	is(fmt($primary, 'o_i'), 6, 'the buffer opened after the promotion is format 6');
	@p = split ' ', $probe->($primary, 'o');
	is($p[0], 1600, 'both kinds of segment and buffer answer');
	$primary->safe_psql('postgres', "SELECT sift.verify('o_i')");
	$primary->wait_for_catchup($standby);
	is($probe->($standby, 'o'), $probe->($primary, 'o'), 'the standby');
};

done_testing();
