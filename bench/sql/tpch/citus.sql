SELECT create_distributed_table('dist.orders', 'o_orderkey');
SELECT create_distributed_table('dist.lineitem', 'l_orderkey', colocate_with => 'dist.orders');
SELECT create_distributed_table('dist.customer', 'c_custkey')
