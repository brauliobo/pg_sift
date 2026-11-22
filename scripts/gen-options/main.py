# Entry of scripts/gen-options.sh: runs the parts of this directory in one namespace, then the mode asked for.
import os
import sys

PARTS = ['sources', 'notes', 'tables', 'live', 'selftest']
for part in PARTS:
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), part + '.py')
    with open(path, encoding='utf-8') as f:
        exec(compile(f.read(), path, 'exec'), globals())

errors = []
if mode == '--selftest':
    errors = selftest()
    if not errors:
        print('options: selftest ok')
elif mode == '--snapshot':
    opts, errs = collect(sources())
    errors += errs
    if not errors:
        print('# released: %s, the defaults of the last release (scripts/gen-options.sh --snapshot)\n%s' % (release_label(), snapshot(opts)))
else:
    opts, notes, released = load(errors)
    if mode == '--missing':
        print('options: 0 missing (compact 6.0 schema)')
    elif mode == '--live':
        errors += live(opts)
        if not errors:
            print('options: live ok, the library agrees with src/ on every setting and reloption')
    elif not errors:
        settings, reloptions, table_options = render(opts, notes, released)
        files = {
            'docs/OPTIONS.md': [
                ('sift-settings', settings),
                ('sift-reloptions', reloptions),
                ('sift-table-options', table_options),
            ]
        }
        for path, blocks in files.items():
            old = new = read(path)
            for tag, block in blocks:
                new = splice(new, tag, block, errors, path)
            if mode == '--check' and new != old:
                errors.append('%s is not what the sources generate: run scripts/gen-options.sh (make options)' % path)
            elif mode == 'write' and not errors:
                with open(os.path.join(ROOT, path), 'w', encoding='utf-8') as f:
                    f.write(new)
        if not errors:
            n = [o for o in opts if not o['testing']]
            print('options: ok, %d settings, %d reloptions, %d table options listed' % (
                sum(o['kind'] == 'guc' for o in n),
                sum(o['kind'] == 'rel' for o in n),
                sum(o['kind'] == 'tbl' for o in n),
            ))
if errors:
    for e in errors:
        print('options: ' + e, file=sys.stderr)
    sys.exit(1)
