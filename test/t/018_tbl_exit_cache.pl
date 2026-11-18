# A backend that read a sift_table frees its cached state before its shared-memory exit (src/table/store.c cache_exit), and the transaction callback that
# runs after it, when the backend leaves a transaction on its way out, finds none of it. A standalone backend ends that way; with every freed chunk filled
# (MALLOC_PERTURB_) it exits cleanly after a read and a write of the table.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use IPC::Run qw(run);
use Test::More;

my $node = sift_node('tbl_exit_cache');
$node->start;
install_sift($node);
$node->safe_psql('postgres', 'CREATE TABLE c (id int PRIMARY KEY, v int) USING sift_table; INSERT INTO c SELECT g, g FROM generate_series(1, 1000) g');
$node->stop;

my ($out, $err);
local $ENV{MALLOC_PERTURB_} = 165;
my $ok = run [ 'postgres', '--single', '-j', '-D', $node->data_dir, '-c', 'config_file=' . $node->data_dir . '/postgresql.conf', 'postgres' ],
	'<', \"BEGIN;\n\nSELECT count(*) FROM c WHERE v > 10;\n\nUPDATE c SET v = v + 1 WHERE id < 100;\n\n", '>', \$out, '2>', \$err;
ok($ok, 'a standalone backend reads and writes a sift_table and exits') or diag "$out$err";
done_testing();
