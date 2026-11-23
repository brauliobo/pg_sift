# The pinned release anchor: a manifest of what each case cost at the pinned release, and the drift of the candidate from it.
# Ten commits that each cost 2 % pass every comparison with their parent; against the anchor the 22 % add up.

ANCHOR_SCHEMA = 'perf-anchor-manifest v1'
ANCHOR_META = ('anchor_ref','anchor_sha','postgresql','rows','tier','recorded')
ANCHOR_COLUMNS = ('case_id','mode','anchor_ref','anchor_instr_per_row','anchor_ms_ratio_vs_native','anchor_blocks')
ANCHOR_UNMEASURED = '-'
DRIFT_INSTRUCTIONS = 1.10
DRIFT_WALL_VS_NATIVE = 1.15
DRIFT_BLOCKS = 1.10
DRIFT_BLOCKS_TOLERANCE = 2
NATIVE_PAIRS = 5


class AnchorManifest:
    def __init__(self, meta, rows):
        self.meta, self.rows = meta, rows


def manifest_number(text, label, line_no):
    if text==ANCHOR_UNMEASURED:
        return None
    try:
        value = float(text)
    except ValueError:
        raise ValueError(f'anchor manifest line {line_no}: {label} is {text!r}, not a number or {ANCHOR_UNMEASURED!r}') from None
    if not math.isfinite(value) or value<=0:
        raise ValueError(f'anchor manifest line {line_no}: {label} must be positive and finite, not {text!r}')
    return value


def parse_anchor_manifest(text):
    lines = text.splitlines()
    if not lines or lines[0]!=f'# schema: {ANCHOR_SCHEMA}':
        raise ValueError(f'anchor manifest line 1 must be "# schema: {ANCHOR_SCHEMA}"')
    meta, index = {}, 1
    while index<len(lines) and lines[index].startswith('# '):
        key,separator,value = lines[index][2:].partition(': ')
        if not separator or not value:
            raise ValueError(f'anchor manifest line {index+1}: metadata is "# key: value"')
        if key in meta or key not in ANCHOR_META:
            raise ValueError(f'anchor manifest line {index+1}: unknown or repeated metadata {key!r} (known: {", ".join(ANCHOR_META)})')
        meta[key] = value
        index += 1
    missing = [k for k in ANCHOR_META if k not in meta]
    if missing:
        raise ValueError('anchor manifest lacks metadata: '+', '.join(missing))
    if not re.fullmatch('[0-9a-f]{40}',meta['anchor_sha']):
        raise ValueError('anchor manifest anchor_sha must be a full 40-character commit SHA')
    if meta['tier'] not in ('pr','nightly') or not meta['rows'].isdigit() or int(meta['rows'])<=0:
        raise ValueError('anchor manifest tier is pr or nightly and rows a positive integer')
    meta['rows'] = int(meta['rows'])
    if index>=len(lines) or lines[index].split('\t')!=list(ANCHOR_COLUMNS):
        raise ValueError(f'anchor manifest line {index+1} must be the header: '+'\t'.join(ANCHOR_COLUMNS))
    known = {c.name for c in expand(CASES) if c.lifecycle=='read'}
    rows = {}
    for number,line in enumerate(lines[index+1:],index+2):
        fields = line.split('\t')
        if len(fields)!=len(ANCHOR_COLUMNS):
            raise ValueError(f'anchor manifest line {number}: {len(ANCHOR_COLUMNS)} tab-separated fields expected, found {len(fields)}')
        case_id,mode,ref = fields[:3]
        name = case_id if mode=='1T' else f'{case_id}@{mode}'
        if mode not in MODE_SETTINGS or name not in known:
            raise ValueError(f'anchor manifest line {number}: {case_id} in mode {mode} is not a read case of the catalog')
        if ref!=meta['anchor_ref']:
            raise ValueError(f'anchor manifest line {number}: anchor_ref {ref!r} differs from the metadata {meta["anchor_ref"]!r}')
        if (case_id,mode) in rows:
            raise ValueError(f'anchor manifest line {number}: {case_id} in mode {mode} is recorded twice')
        values = [manifest_number(f,label,number) for f,label in zip(fields[3:],ANCHOR_COLUMNS[3:])]
        if all(v is None for v in values):
            raise ValueError(f'anchor manifest line {number}: {case_id} in mode {mode} records nothing')
        rows[case_id,mode] = dict(zip(('instr_per_row','ms_ratio_vs_native','blocks'),values))
    if not rows:
        raise ValueError('anchor manifest records no case')
    return AnchorManifest(meta,rows)


def load_anchor_manifest(path):
    try:
        return parse_anchor_manifest(Path(path).read_text())
    except (OSError,ValueError) as exc:
        raise ValueError(f'--anchor-tsv {path}: {exc}') from None


def format_anchor_manifest(meta, rows):
    number = lambda v: ANCHOR_UNMEASURED if v is None else f'{v:.4g}'
    lines = [f'# schema: {ANCHOR_SCHEMA}',*(f'# {k}: {meta[k]}' for k in ANCHOR_META),'\t'.join(ANCHOR_COLUMNS)]
    lines += ['\t'.join([case_id,mode,meta['anchor_ref'],number(v['instr_per_row']),number(v['ms_ratio_vs_native']),number(v['blocks'])])
              for (case_id,mode),v in rows.items()]
    return '\n'.join(lines)+'\n'


def plan_blocks(plan):
    root = plan[0]['Plan']
    return root.get('Shared Hit Blocks',0)+root.get('Shared Read Blocks',0)


def warm_blocks(conn, case, deadline):
    """the shared buffers a steady-state execution touches: the first execution of a session also reads the catalogs and the metadata of its relations"""
    prepare(conn,case)
    elapsed(conn,case,1,deadline)
    return plan_blocks(json.loads(conn.rows('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON, TIMING OFF) '+case.sql)[0][0]))


def native_wall_ratio(conn, case, operations, deadline):
    """median of paired batches: the case over the same question put to the heap table, both under the profile of the mode"""
    native = dataclasses.replace(case,sql=case.oracle)
    ratios = []
    for _ in range(NATIVE_PAIRS):
        prepare(conn,case)
        elapsed(conn,case,1,deadline)
        wall = elapsed(conn,case,operations,deadline)
        elapsed(conn,native,1,deadline)
        ratios.append(wall/elapsed(conn,native,operations,deadline))
    return statistics.median(ratios)


def case_profile(conn, case, instructions, operations, deadline):
    """what the anchor manifest records of one case on one revision"""
    return dict(instr_per_row=instructions,
                ms_ratio_vs_native=native_wall_ratio(conn,case,operations,deadline) if case.oracle else None,
                blocks=warm_blocks(conn,case,deadline) or None)


def drift_check(recorded, measured, limit, tolerance=0):
    if recorded is None:
        return dict(status='unrecorded')
    if measured is None:
        return dict(status='unmeasured',recorded=recorded)
    return dict(status='regression' if measured>recorded*limit+tolerance else 'pass',recorded=recorded,measured=measured,
                ratio=measured/recorded,limit=limit)


def anchor_drift(manifest, case, profile, rows):
    entry = manifest.rows.get((case.name.split('@')[0],case.mode))
    if entry is None:
        return dict(status='unrecorded',source='manifest')
    checks = dict(instructions=drift_check(entry['instr_per_row'],profile['instr_per_row'],DRIFT_INSTRUCTIONS),
                  wall_vs_native=drift_check(entry['ms_ratio_vs_native'],profile['ms_ratio_vs_native'],DRIFT_WALL_VS_NATIVE))
    checks['blocks'] = (drift_check(entry['blocks'],profile['blocks'],DRIFT_BLOCKS,DRIFT_BLOCKS_TOLERANCE) if rows==manifest.meta['rows']
                        else dict(status='rows_differ',recorded_rows=manifest.meta['rows'],rows=rows))
    statuses = {c['status'] for c in checks.values()}
    return dict(source='manifest',checks=checks,
                status='regression' if 'regression' in statuses else 'pass' if 'pass' in statuses else 'unmeasured')


def anchor_cache_root():
    return Path(os.environ.get('XDG_CACHE_HOME') or Path.home()/'.cache')/'sift/anchor'


def stage_anchor(ref, cache_root, pg_config, jobs, log):
    sha = command(['git','rev-parse','--verify',ref+'^{commit}'],cwd=ROOT)
    return (sha,*stage_release(sha,cache_root,pg_config,jobs,log))


def stage_release(sha, cache_root, pg_config, jobs, log, source=ROOT):
    """(staged install, reused) of a commit: built once per commit and PostgreSQL major, then reused by every run.
    The stage and its `.fixture` stamp are the ones scripts/build-v510-fixture.sh writes, so that script can fill the cache as well."""
    major = re.search(r'\d+',command([pg_config,'--version'])).group()
    stage = cache_root/f'{sha[:12]}-pg{major}'
    stamp = f'{sha} pg{major}'
    if (stage/'.fixture').is_file() and (stage/'.fixture').read_text().strip()==stamp:
        return stage,True
    cache_root.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='anchor-',dir=cache_root) as tmp:
        staged = Path(tmp)/'stage'
        archive_tree(sha,Path(tmp)/'source',source,log)
        build_release(Path(tmp)/'source',staged,pg_config,jobs,log)
        if not list(staged.rglob('pg_sift.*so')):
            raise RuntimeError(f'the build of {sha} staged no pg_sift library')
        (staged/'.fixture').write_text(stamp+'\n')
        shutil.rmtree(stage,ignore_errors=True)
        staged.rename(stage)
    return stage,False


def unsupported(sink, case, reason):
    sink.append(dict(name=case.name,case=case.name.split('@')[0],mode=case.mode,group=case.group,smoke=case.smoke,gate=case.gate,
                     status='skipped',timing_status='skipped',complete=True,phase='complete',reason=reason))


def measure_against_anchor(anchor, candidate, case, deadline, report, output, record):
    """the case on the anchor and the candidate, paired like any other revisions; what the anchor cannot run is reported, not timed"""
    sink = report['vs_anchor']
    if case.needs and case.needs not in anchor.capabilities:
        return unsupported(sink,case,f'the anchor has no {case.needs} access method')
    try:
        result = measure([anchor,candidate],case,deadline,sink=sink,report=report,output=output,profile=False,label='_anchor')
    except CorrectnessMismatch as exc:
        sink[-1].update(status='skipped',timing_status='skipped',complete=True,phase='complete',reason=str(exc))
        return
    if record and result['status']!='skipped':
        instructions = result.get('instructions')
        report['anchor_profile'][case.name] = case_profile(anchor.conn,case,instructions and instructions['baseline_per_row'],result['operations'],deadline)


def anchor_record(report, args):
    """instructions and blocks do not depend on the host; a wall ratio is recorded only from a run that waited for a quiet host and never timed a noisy block"""
    host = report['quiet_host']
    quiet = host['enabled'] and host['blocks']>0 and host['noisy_blocks']==0
    rows = {(name.split('@')[0],'4T' if name.endswith('@4T') else '1T'):dict(profile,ms_ratio_vs_native=profile['ms_ratio_vs_native'] if quiet else None)
            for name,profile in report['anchor_profile'].items()}
    meta = dict(anchor_ref=args.anchor_ref,anchor_sha=report['anchor']['sha'],postgresql=report['postgresql'],
                rows=report['anchor']['rows'],tier=args.tier,recorded=report['date'][:10])
    text = format_anchor_manifest(meta,rows)
    parse_anchor_manifest(text)
    args.anchor_record.write_text(text)
