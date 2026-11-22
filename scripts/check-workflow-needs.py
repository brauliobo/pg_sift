import sys, yaml
bad = 0
for path in sys.argv[1:]:
    doc = yaml.safe_load(open(path))
    jobs = doc['jobs']
    for name, job in jobs.items():
        needs = job.get('needs', [])
        needs = [needs] if isinstance(needs, str) else needs
        for n in needs:
            if n not in jobs:
                print('%s: job %s needs unknown job %s' % (path, name, n)); bad = 1
        if 'runs-on' not in job and 'uses' not in job:
            print('%s: job %s has no runs-on' % (path, name)); bad = 1
    on = doc.get(True, doc.get('on'))
    print('%s: ok, %d jobs (%s), triggers %s' % (path, len(jobs), ', '.join(jobs), ', '.join(on)))
sys.exit(bad)
