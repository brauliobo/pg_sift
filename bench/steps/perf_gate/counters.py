# Hardware counters: the user-space instructions a case retires, from perf stat, next to its elapsed time.
# Instructions do not depend on how busy the host is: a slower elapsed time with equal instructions is a cache, IPC or parallel-scaling regression.

PERF_EVENT = 'instructions:u'
PERF_AVAILABLE = False


def perf_available(run=subprocess.run):
    result = run(['perf','stat','-x,','-e',PERF_EVENT,'true'],env={**os.environ,'LC_ALL':'C'},capture_output=True,text=True)
    return result.returncode==0 and any(','+PERF_EVENT in line for line in result.stderr.splitlines())


def parse_instructions(text):
    for line in text.splitlines():
        fields = line.split(',')
        if len(fields)>2 and fields[2]==PERF_EVENT:
            if not fields[0].isdigit():
                raise ValueError('perf stat counted no instructions: '+line)
            return int(fields[0])
    raise ValueError('perf stat reported no '+PERF_EVENT)


def await_ack(fd, perf):
    seen = b''
    while b'ack\n' not in seen:
        chunk = os.read(fd,64)
        if not chunk:
            raise RuntimeError('perf stat stopped before its acknowledgement: '+perf.stderr.read().decode())
        seen += chunk


def count_instructions(pids, action):
    """(instructions retired by PIDS and by the processes they fork while ACTION runs, ACTION's result); perf stat counts only between enable and disable"""
    command_read, command_write = os.pipe()
    ack_read, ack_write = os.pipe()
    with tempfile.TemporaryDirectory(prefix='perf-counter-') as tmp:
        output = Path(tmp)/'stat'
        perf = subprocess.Popen(['perf','stat','-x,','-e',PERF_EVENT,'-p',','.join(map(str,pids)),'--control',f'fd:{command_read},{ack_write}',
                                 '--delay=-1','-o',output],pass_fds=(command_read,ack_write),env={**os.environ,'LC_ALL':'C'},stderr=subprocess.PIPE)
        os.close(command_read)
        os.close(ack_write)
        try:
            os.write(command_write,b'enable\n')
            await_ack(ack_read,perf)
            result = action()
            os.write(command_write,b'disable\n')
            await_ack(ack_read,perf)
        finally:
            perf.send_signal(signal.SIGINT)
            perf.wait()
            perf.stderr.close()
            os.close(command_write)
            os.close(ack_read)
        return parse_instructions(output.read_text()),result


def postmaster_pid(rev):
    return int((rev.data/'postmaster.pid').read_text().splitlines()[0])


def counted_pids(rev, case):
    """the backend; a 4T run adds the postmaster, whose parallel workers are forked after perf attached"""
    backend = int(rev.conn.rows('SELECT pg_backend_pid()')[0][0])
    return [backend,postmaster_pid(rev)] if case.mode=='4T' else [backend]


def case_instructions(rev, case, deadline):
    prepare(rev.conn,case)
    elapsed(rev.conn,case,1,deadline)
    count,_ = count_instructions(counted_pids(rev,case),lambda: elapsed(rev.conn,case,1,deadline))
    return count


def fixture_rows(conn):
    return int(conn.rows('SELECT count(*) FROM seed')[0][0])


def measure_instructions(revisions, case, rows, deadline):
    """one counted execution per revision, after the timed blocks so that the counter never perturbs them"""
    counts = [case_instructions(rev,case,deadline) for rev in revisions]
    ratio = counts[1]/counts[0]
    return dict(event=PERF_EVENT,baseline=counts[0],candidate=counts[1],ratio=ratio,rows=rows,
                baseline_per_row=counts[0]/rows,candidate_per_row=counts[1]/rows)


def regression_kind(timing_status, instructions):
    """a slower case executes more instructions (code work) or the same ones slower (cache, IPC, parallel scaling)"""
    if timing_status!='regression':
        return None
    if instructions is None:
        return 'instructions_unmeasured'
    return 'instructions' if instructions['ratio']>MATERIAL else 'ipc_or_scaling'
