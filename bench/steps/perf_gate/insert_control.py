# The control of the INSERT cases: the seed identity, the delay trigger and the CPU work it adds.
# The control proves the gate detects a slowdown it injected itself.

SEED_IDENTITY = "SELECT count(*),max(id),md5(string_agg(row(seed.*)::text,'|' ORDER BY id)) FROM seed"


def insert_identity(revisions):
    seeds = [r.conn.rows(SEED_IDENTITY) for r in revisions]
    require_equal(seeds[0],seeds[1],'INSERT seed identity')
    count,maximum,digest = seeds[0][0]
    count,maximum = int(count),int(maximum)
    if count<=0 or not -(2**31)<=maximum<=2**31-1-max(INSERT_PILOT,INSERT_GROWTH_CAP):
        raise ValueError('INSERT seed/full growth exceeds integer domain')
    return dict(seed_count=count,seed_max_id=maximum,first_id=maximum+1,seed_digest=digest)


def insert_footprint(conn):
    geometry = table_geometry(conn,'mutable')
    if geometry['units']<3 or geometry['column_segments']<1 or geometry['relation_blocks']<256:
        raise ValueError('mutable fixture lacks actual table unit/segment/page boundary')
    forks = relation_forks(conn)
    return dict(geometry=geometry,forks={name:forks[name] for name in MUTABLE_RELATIONS},
                totals={name:fork_totals(forks)[name] for name in MUTABLE_RELATIONS})


def cleanup_insert_sql(conn, statements, sink, primary):
    """Restore temporary INSERT SQL state without demoting a known primary error."""
    if primary is not None:
        sink['primary_error'] = dict(type=type(primary).__name__,error=str(primary))
    for statement in statements:
        try:
            conn.exec(statement)
        except Exception as cleanup:
            detail = dict(statement=statement,type=type(cleanup).__name__,error=str(cleanup))
            sink.setdefault('cleanup_errors',[]).append(detail)
            if primary is None or (isinstance(primary,Inconclusive) and not isinstance(cleanup,Inconclusive)):
                raise
            primary.add_note(f"INSERT SQL cleanup failed ({statement}): {type(cleanup).__name__}: {cleanup}")
            return False
    return True


def check_insert(conn, identity, operations, sink):
    require_equal(conn.rows(SEED_IDENTITY),[[str(identity['seed_count']),str(identity['seed_max_id']),identity['seed_digest']]],
                  'INSERT seed unchanged')
    case = next(c for c in CASES if c.lifecycle=='insert_trajectory')
    expected = case.expected_rows(identity['seed_max_id'],operations)
    # Both full row multisets, including NULLs and pad, come from explicit columns.
    query = f"""WITH expected AS ({expected}), actual AS (SELECT id,g,amount,body,pad FROM mutable),
    delta AS ((SELECT * FROM expected EXCEPT ALL SELECT * FROM actual)
              UNION ALL (SELECT * FROM actual EXCEPT ALL SELECT * FROM expected))
    SELECT (SELECT count(*) FROM expected),
           (SELECT md5(string_agg(row(e.*)::text,'|' ORDER BY id,g,amount,body,pad)) FROM expected e),
           (SELECT count(*) FROM actual),
           (SELECT md5(string_agg(row(a.*)::text,'|' ORDER BY id,g,amount,body,pad)) FROM actual a),
           (SELECT count(*) FROM delta)"""
    conn.exec('SET sift.enable_agg=off')
    try:
        count,digest,actual_count,actual_digest,difference = conn.rows(query)[0]
        sink.update(expected_count=int(count),expected_digest=digest,actual_count=int(actual_count),
                    actual_digest=actual_digest,multiset_difference=int(difference))
        require_equal(int(count),identity['seed_count']+operations,'INSERT expected count')
        require_equal((actual_count,actual_digest,difference),(count,digest,'0'),'INSERT full multiset')
        return sink
    finally:
        cleanup_insert_sql(conn,['SET sift.enable_agg=on'],sink,sys.exc_info()[1])


def insert_control_count(conn):
    return int(conn.rows("SELECT current_setting('perf_gate.delayed_rows')")[0][0])


def install_insert_control(conn, seed_max, iterations, *, active=True, sink=None):
    # Infrastructure DDL only is transactional. Every workload point remains autocommit.
    try:
        conn.exec('BEGIN')
        conn.exec("""CREATE FUNCTION perf_gate_cpu_work(row_id integer, iterations integer) RETURNS text
        LANGUAGE plpgsql VOLATILE AS $$
        DECLARE state text := row_id::text; i integer;
        BEGIN
          FOR i IN 1..iterations LOOP state := md5(state || ':' || row_id::text); END LOOP;
          RETURN state;
        END $$""")
        conn.exec(f"""CREATE FUNCTION perf_gate_insert_delay() RETURNS trigger LANGUAGE plpgsql AS $$
        BEGIN
          IF NEW.id > {seed_max} THEN
            PERFORM set_config('perf_gate.delayed_rows',(current_setting('perf_gate.delayed_rows')::bigint+1)::text,false);
            PERFORM set_config('perf_gate.cpu_checksum',perf_gate_cpu_work(NEW.id,{iterations}),false);
          END IF;
          RETURN NEW;
        END $$""")
        conn.exec('CREATE TRIGGER perf_gate_insert_delay BEFORE INSERT ON mutable FOR EACH ROW EXECUTE FUNCTION perf_gate_insert_delay()')
        conn.exec('ALTER TABLE mutable DISABLE TRIGGER perf_gate_insert_delay')
        conn.exec("SET perf_gate.delayed_rows='0'")
        if sink is not None: sink['commit_attempted']=True
        conn.exec('COMMIT')
        if sink is not None: sink['infrastructure_committed']=True
    except Exception:
        cleanup_insert_sql(conn,['ROLLBACK'],sink if sink is not None else {},sys.exc_info()[1])
        raise
    if active:
        conn.exec('ALTER TABLE mutable ENABLE TRIGGER perf_gate_insert_delay')
        if sink is not None: sink['active']=True


def remove_insert_control(conn, sink, primary):
    return cleanup_insert_sql(conn,['DROP TRIGGER perf_gate_insert_delay ON mutable',
                                    'DROP FUNCTION perf_gate_insert_delay()',
                                    'DROP FUNCTION perf_gate_cpu_work(integer,integer)'],sink,primary)


def require_control_absent(conn):
    require_equal(conn.rows("SELECT count(*) FROM pg_proc WHERE proname IN ('perf_gate_cpu_work','perf_gate_insert_delay')"),
                  [['0']],'control functions absent')
    require_equal(conn.rows("SELECT count(*) FROM pg_trigger WHERE tgname='perf_gate_insert_delay'"),[['0']],'control trigger absent')


def calibrate_cpu_work(conn, sink):
    """One fixed zero/16-iteration warm+measurement, no INSERT or verdict tuning."""
    sink.update(rows=CPU_PILOT_ROWS,pilot_iterations=CPU_PILOT_ITERATIONS,iteration_cap=CPU_ITERATION_CAP,
                blocks=[],complete=False,scope='client elapsed helper calls; no mutable writes; trigger/counter/seed overhead excluded')
    for iterations in (0,CPU_PILOT_ITERATIONS):
        block = dict(iterations=iterations,warm_seconds=[],operation_seconds=[],checksums=[])
        sink['blocks'].append(block)
        for key in ('warm_seconds','operation_seconds'):
            for ordinal in range(CPU_PILOT_ROWS):
                before=time.perf_counter()
                checksum=conn.rows(f'SELECT perf_gate_cpu_work({-ordinal-1},{iterations})')[0][0]
                block[key].append(time.perf_counter()-before)
                require_equal(checksum,cpu_checksum(-ordinal-1,iterations),'CPU calibration checksum')
                if key=='operation_seconds': block['checksums'].append(checksum)
    zero,loaded=(sum(b['operation_seconds'])/CPU_PILOT_ROWS for b in sink['blocks'])
    slope=(loaded-zero)/CPU_PILOT_ITERATIONS
    if not math.isfinite(slope) or slope<=0:
        raise Inconclusive('fixed CPU calibration has no positive finite incremental work evidence')
    sink.update(seconds_per_iteration=slope,complete=True)


def cpu_checksum(row_id, iterations):
    state=str(row_id)
    for _ in range(iterations): state=hashlib.md5((state+':'+str(row_id)).encode()).hexdigest()
    return state


def freeze_cpu_work(calibration, cpu):
    if not cpu.get('complete') or not finite_number(cpu.get('seconds_per_iteration')) or cpu['seconds_per_iteration']<=0:
        raise Inconclusive('missing completed same-invocation CPU calibration')
    # A large prospective functional signal: added helper CPU equal to slower clean per-row SQL.
    desired=math.ceil(max(calibration['sql_seconds_per_operation'])/cpu['seconds_per_iteration'])
    iterations=min(desired,CPU_ITERATION_CAP)
    return dict(iterations=iterations,desired_iterations=desired,limited_by_cap=iterations<desired,
                seconds_per_iteration=cpu['seconds_per_iteration'],predicted_added_seconds_per_row=iterations*cpu['seconds_per_iteration'],
                target_added_seconds_per_row=max(calibration['sql_seconds_per_operation']),
                policy='one fixed zero/16 slope; ceil(slower clean client SQL per row / slope), cap1024; never verdict tuned',
                provenance=cpu)
