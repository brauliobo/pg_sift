"""The column lists of every schema version the records were written with, frozen: a file is read by the header it has (schema.read), so a change of FIELDS is a new version here, a
migration in schema.MIGRATIONS from the version before it, and a frozen sample in bench/test/records_samples. Columns are only ever added; the lists are never edited."""

HISTORY = {
    1: '''
run_id tip harness dataset rows keys rows_per_key heap_order heap_corr table_state allvisible_pct structures statement sql_hash kind protocol mode arm
arm_option picked structure temp rep est_cost plan_ms exec_ms wall_ms instr_m cycles_m instr_scope workers_requested workers_launched heap_hit
heap_read btree_hit btree_read gin_hit gin_read sift_hit sift_read other_hit other_read os_cache_dropped wal_bytes fpw cpus psi_cpu_start psi_cpu_end
psi_io_start psi_io_end shared_buffers_mb work_mem_mb pg_version settings settings_hash maintenance
'''.split(),
    2: '''
run_id tip harness dataset rows keys rows_per_key heap_order heap_corr table_state allvisible_pct structures statement sql_hash variant kind protocol
mode arm arm_option picked structure temp rep est_cost plan_ms exec_ms wall_ms instr_m cycles_m instr_scope workers_requested workers_launched
heap_hit heap_read btree_hit btree_read gin_hit gin_read sift_hit sift_read other_hit other_read os_cache_dropped wal_bytes fpw cpus psi_cpu_start
psi_cpu_end psi_io_start psi_io_end shared_buffers_mb work_mem_mb pg_version settings settings_hash maintenance
'''.split(),
    3: '''
run_id tip harness dataset rows keys rows_per_key heap_order heap_corr table_state allvisible_pct churn_history vacuums autovacuums settled settled_s
dead_pct tombstone_pct structures statement sql_hash variant kind protocol mode arm arm_option picked structure temp rep est_cost plan_ms exec_ms
wall_ms instr_m cycles_m instr_scope workers_requested workers_launched heap_hit heap_read btree_hit btree_read gin_hit gin_read sift_hit sift_read
other_hit other_read os_cache_dropped table_bytes index_bytes wal_bytes fpw cpus psi_cpu_start psi_cpu_end psi_io_start psi_io_end shared_buffers_mb
work_mem_mb pg_version settings settings_hash maintenance
'''.split(),
    4: '''
run_id tip harness dataset rows keys rows_per_key heap_order heap_corr table_state allvisible_pct churn_history vacuums autovacuums settled settled_s
dead_pct tombstone_pct structures statement sql_hash variant kind protocol mode arm arm_option picked structure temp rep est_cost plan_ms exec_ms
wall_ms instr_m cycles_m instr_scope workers_requested workers_launched heap_hit heap_read btree_hit btree_read gin_hit gin_read sift_hit sift_read
other_hit other_read os_cache_dropped table_bytes index_bytes wal_bytes fpw cpus noise psi_cpu_start psi_cpu_end psi_io_start psi_io_end
shared_buffers_mb work_mem_mb pg_version settings settings_hash maintenance
'''.split(),
    5: '''
run_id tip harness dataset rows keys rows_per_key heap_order heap_corr table_state allvisible_pct churn_history vacuums autovacuums settled settled_s
dead_pct tombstone_pct structures statement sql_hash variant kind protocol mode arm arm_option picked structure temp rep est_cost plan_ms exec_ms
wall_ms instr_m cycles_m instr_scope instr_n workers_requested workers_launched heap_hit heap_read btree_hit btree_read gin_hit gin_read sift_hit
sift_read other_hit other_read os_cache_dropped table_bytes index_bytes wal_bytes fpw cpus noise psi_cpu_start psi_cpu_end psi_io_start psi_io_end
shared_buffers_mb work_mem_mb pg_version settings settings_hash maintenance
'''.split(),
}
